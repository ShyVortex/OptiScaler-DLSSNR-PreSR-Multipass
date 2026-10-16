# Frame Generation: Helldivers 2 Crash on XeMFG-SR Enablement (LowLatencyCtx & Reflex Hooks Null Dereference)

## 1. Overview & Environment

- **Operating System**: Linux (CachyOS x86_64, Kernel 7.2.9-2-cachyos, NVIDIA Driver 580.178.04, Proton Experimental / Wine / VKD3D-Proton)
- **Target Game**: *Helldivers 2* (build 1.003.x, Arrowhead Game Studios / Sony Interactive Entertainment)
- **GPU Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (16GB VRAM, GA103 / Ampere SM86)
- **Display Resolution & Upscaler**: 2560x1600, DLSS Super Resolution (Quality mode, DLSS SDK 3.7.20)
- **Target Runtimes**: `dxgi.dll` (OptiScaler v0.7.x), `libxess_fg.dll` (Intel XeFG v1.3.1)
- **Issue Reference**: Immediate game crash (`0xc0000005`) upon checking "Enable XeMFG-SR Injector" in the in-game menu overlay

---

## 2. Problem Statement & Minidump Triage

When running *Helldivers 2* (which natively supports DLSS Super Resolution but lacks native DLSS Frame Generation or Streamline), activating the **XeMFG-SR Injector** from the in-game OptiScaler menu overlay resulted in an immediate, hard process crash.

The OptiScaler log ended abruptly without an error message:
```text
[11:47:52.562154] [I] NvApiHooks::ApplySmoothMotionDrs NvApiHooks::ApplySmoothMotionDrs: skipped on non-Windows or non-Nvidia GPU
```

### 2.1 Minidump Inspection

Inspection of the generated minidump (`dump-2026-10-11-11.47.16-65387d49-cachyos-30800.dmp`) yielded:
- **Exception Code**: `0xc0000005` (`EXCEPTION_ACCESS_VIOLATION`)
- **Exception Address**: `0x6ffffa9ab6b5` in `dxgi.dll` (`OptiScaler`) at offset `0xcb6b5`
- **CPU Registers at Crash**:
  - `RIP`: `0x6ffffa9ab6b5`
  - `RCX`: `0x0000000000000008` (`nullptr + 8`)
  - `RDX`: `0x0000000000000000`
  - `RSP`: `0x00007ffc6292bbf0`
- **Faulting Instruction**:
  ```asm
  mov (%rcx), %rdx   ; Dereferencing RCX = 0x8 -> Crash!
  ```
- **Call Stack**:
  1. `dxgi.dll + 0xcb6b5`: `std::atomic<std::shared_ptr<LowLatencyTech>>::load()` (internal spinlock acquire at `this + 8`)
  2. `dxgi.dll + 0xefac1`: `LowLatency::update_low_latency_tech()` (at `this->currently_active_tech.load()`)
  3. `dxgi.dll + 0xf68ab`: `LowLatency::SetSleepMode()`
  4. `dxgi.dll + 0xf6860`: `nvapi_calls::NvAPI_D3D_SetSleepMode()`
  5. `dxgi.dll + 0x190fba`: `ReflexHooks::hkNvAPI_D3D_SetLatencyMarker()` / `hkNvAPI_D3D_SetSleepMode()`
  6. Game Engine Thread (`helldivers2.exe`)

---

## 3. Root Cause Analysis

The crash was caused by a combination of uninitialized state, missing null guards, and premature hook routing:

1. **Uninitialized `LowLatencyCtx`**:
   In `OptiScaler/nvapi/fakenvapi/low_latency.h`, `LowLatencyCtx::get()` returned the static pointer `lowlatency_ctx` directly without checking whether it was initialized. If `LowLatencyCtx::init()` had not been invoked, `get()` returned `nullptr`.

2. **Unchecked Pointer Dereference in `nvapi_calls.cpp`**:
   All 13 dispatch functions in `nvapi_calls.cpp` (such as `NvAPI_D3D_SetSleepMode`, `NvAPI_D3D_SetLatencyMarker`, `NvAPI_D3D_Sleep`) called `LowLatencyCtx::get()->...` blindly without validating the pointer. When `get()` returned `nullptr`, `update_low_latency_tech` executed with `this == nullptr`. MSVC's atomic `std::shared_ptr` implementation accesses an internal spinlock at offset `this + 8`, causing a memory access violation on `0x0000000000000008`.

3. **Lifecycle Gap on NVIDIA Hardware Starting with `NoFG`**:
   On NVIDIA cards where the game starts with `FGOutput::NoFG`, `NvApiHooks::Hook()` bypassed `fakenvapi::init` to let native NVIDIA NVAPI handle calls. When the user checked XeMFG-SR mid-game, `state.activeFgOutput` was updated to `FGOutput::XeFG`, but `fakenvapi::init(true)` had never been called to construct the `LowLatency` context.

4. **Premature Reflex Hook Hijacking**:
   In `Reflex_Hooks.cpp`, lines 37, 83, 96, 294, 381, and 728 forwarded all game Reflex calls directly to `nvapi_calls` solely based on `if (State::Instance().activeFgOutput == FGOutput::XeFG)`, without checking whether the XeFG swapchain had been created (`state.currentFGSwapchain != nullptr`) or whether the XeFG feature was active (`state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused()`). Consequently, the moment the menu checkbox was clicked, the game's high-frequency Reflex marker calls were hijacked into the uninitialized `fakenvapi` layer.

5. **Intel XeFG SwapChain Creation Invariant**:
   Intel XeSS-FG (`libxess_fg.dll`) requires wrapping the swapchain during DXGI swapchain creation (`FGHooks::CreateSwapChain`). A game launched with `NoFG` has an unwrapped DXGI swapchain. Enabling XeMFG-SR mid-game cannot dynamically re-wrap an existing swapchain without restarting the game. The UI previously suggested XeMFG-SR works live without a restart, leading users into the uninitialized state.

---

## 4. Architectural Solution Applied

### 4.1 Thread-Safe Self-Initializing `LowLatencyCtx`
In `OptiScaler/nvapi/fakenvapi/low_latency.h`:
- Guarded `LowLatencyCtx::init()` and `LowLatencyCtx::shutdown()` with `std::scoped_lock` on a static mutex `s_mutex`.
- Updated `LowLatencyCtx::get()` to auto-initialize on demand, guaranteeing it never returns `nullptr`:
  ```cpp
  static LowLatency* get()
  {
      if (!lowlatency_ctx)
          init();
      return lowlatency_ctx;
  }
  ```

### 4.2 Comprehensive Null Guards in `nvapi_calls.cpp`
Added null guards across all 13 dispatch functions in `nvapi_calls.cpp` (`GetSleepStatus`, `GetLatency`, `SetSleepMode`, `SetLatencyMarker`, `Sleep`, `SetAsyncFrameMarker`, and all Vulkan low-latency equivalents). If the context is null, functions return `ERROR()` (`NVAPI_ERROR`) immediately instead of dereferencing invalid memory.

### 4.3 Reflex Hook SwapChain & Feature State Gating
In `OptiScaler/hooks/Reflex_Hooks.cpp`:
- Introduced the invariant helper `IsXeFGActive()`:
  ```cpp
  static inline bool IsXeFGActive()
  {
      const auto& state = State::Instance();
      return state.activeFgOutput == FGOutput::XeFG && state.currentFGSwapchain != nullptr &&
             state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused();
  }
  ```
- Gated all XeFG Reflex interceptions (`hkNvAPI_D3D_SetSleepMode`, `hkNvAPI_D3D_Sleep`, `hkNvAPI_D3D_GetLatency`, `hkNvAPI_D3D_SetLatencyMarker`, `hkNvAPI_D3D12_SetAsyncFrameMarker`, and `setFPSLimit`) with `IsXeFGActive()`.
- When XeFG is not active or when the swapchain has not yet been bound to XeFG, Reflex calls fall through smoothly to native driver Reflex (`o_NvAPI_D3D_...`), completely eliminating the crash vector.

### 4.4 Early Lifecycle Initialization
- In `OptiScaler/dllmain.cpp`: Called `fakenvapi::init(true)` during post-attach pipeline auto-configuration when `xeMfgUnlock` or `xeMfgSrActive` is active.
- In `OptiScaler/menu/menu_common.cpp`: Called `fakenvapi::init(true)` immediately before `XeFGProxy::InitXeFG()` when enabling XeMFG-SR.

### 4.5 In-Game Menu Guidance & Swapchain Diagnostics
In `OptiScaler/menu/menu_common.cpp`:
- Updated XeMFG-SR checkbox labels to indicate restart requirement:
  `"Enable XeMFG-SR Injector (Multi-Frame Generation; restart)##xemfg_sr"`
- Added dynamic swapchain status diagnostic:
  ```cpp
  if (state.currentFGSwapchain == nullptr)
  {
      ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.6f, 0.2f, 1.0f)),
                         "Note: Save Settings and restart the game to initialize the XeFG SwapChain.");
  }
  ```
- Updated help marker tooltip to clarify that initial enablement requires saving settings and restarting so `libxess_fg.dll` can wrap the swapchain, while multiplier changes (2X to 6X) adjust live during active gameplay once initialized.

---

## 5. Verification & Anti-Regression Testing

1. **Automated Unit Tests (`tests/xemfg_sr_crash_fix_unit.cpp`)**:
   - `Test_LowLatencyCtx_LazyInit_And_Concurrency`: 16 concurrent threads accessing uninitialized `LowLatencyCtx::get()` with zero data races or crashes.
   - `Test_NvapiCalls_NullGuards`: Verified clean error returns on null context across all dispatch entry points.
   - `Test_ReflexHooks_Gating_Invariants`: Verified that `IsXeFGActive()` prevents hook hijacking when `currentFGSwapchain == nullptr`, `currentFG == nullptr`, or when the feature is paused/inactive.
   - `Test_Menu_Restart_Diagnostics`: Verified correct display and clearing of swapchain restart notices.
   - `Test_Pipeline_AutoConfiguration`: Verified post-attach auto-configuration and multiplier clamping.
   - **Result**: 5/5 test cases passing cleanly (`g++ -std=c++20 -pthread`).

2. **Existing Test Suite Verification**:
   - `tests/xemfg_sr_unit.cpp` (7/7 passing).
   - `tests/xemfg_menu_exclusion_unit.cpp` (6/6 passing).
   - `tests/low_latency_selection_unit.cpp` (passing).

3. **Compiler & Clang-Format Compliance**:
   - `clang-format --dry-run --Werror` passed across all modified and new files with 0 violations.
   - UTF-8 BOM (`\xef\xbb\xbf`) preserved on all affected source and header files.
