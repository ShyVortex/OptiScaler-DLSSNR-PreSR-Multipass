# Intel XeMFG SwapChain Initialization Failure & Non-Intel Hardware Validation Analysis

## 1. Overview & Environment

- **Target Game**: *Helldivers 2* (`helldivers2.exe`, Steam AppID `553850`)
- **Operating System**: CachyOS Linux x86_64 (Kernel `7.2.9-2-cachyos`)
- **Graphics Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (GA103 / Ampere SM86 `0x170`, Driver `580.178.04`)
- **Display Configuration**: 2560x1440 @ 164.9 Hz (Borderless Windowed / Fullscreen)
- **Compatibility Layer**: Proton Experimental / Wine 10 / VKD3D-Proton (Direct3D 12 on Vulkan)
- **Component Subsystem**: `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`, `OptiScaler/dllmain.cpp`
- **Related Issue Docs**:
  - `docs/issues/FG/FG-XEMFG-PRESENTATION-PACING.md`
  - `docs/issues/FG/FG-XEMFG-BASE-FRAME-PACING-AND-MULTIPLIER-UNLOCK.md`
  - `docs/issues/FG/FG-XEMFG-BASE-FRAME-PACING-REGRESSION.md`

---

## 2. Reported Symptoms

Following the multiplier ceiling uncap implementation in commit `55b47147`, *Helldivers 2* failed to initialize the XeFG swapchain context during game startup regardless of window mode (both in exclusive fullscreen and in borderless windowed mode).

1. In the in-game ImGui overlay under `Intel Xe Multi-Frame Generation for DLSS-SR (XeMFG-SR)`:
   - `DLL: found | Patches: 5/5 | Pacing: active (native provider)`
   - Persistent orange warning: `Note: Save Settings and restart the game to initialize the XeFG SwapChain.`
   - Inactive swapchain (`state.currentFGSwapchain == nullptr`), preventing frame generation from running.
2. In `OptiScaler.log`:
   ```text
   [17:40:54.743443] [I] XeFG_Dx12::CreateSwapchainContext XeFG context created
   [17:40:54.744567] [I] XeFG_Dx12::CreateSwapchain1 Max supported interpolations: 5
   [17:40:54.744591] [D] XeFG_Dx12::CreateSwapchain1 Inverted Depth: true
   [17:40:54.744604] [D] XeFG_Dx12::CreateSwapchain1 Jittered Velocity: false
   [17:40:54.744617] [D] XeFG_Dx12::CreateSwapchain1 High Res MV: false
   [17:40:54.744633] [E] XeFG_Dx12::CreateSwapchain1 D3D12InitFromSwapChainDesc error: XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT (4294967292)
   ```
   Intel's `libxess_fg.dll` rejected swapchain creation in 16 microseconds with error code `0xFFFFFFFC` (`XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT`).

---

## 3. Reverse-Engineering & Root Cause Analysis

### A. Non-Intel Validation Gate in `libxess_fg.dll`

Disassembly of export `xefgSwapChainD3D12InitFromSwapChainDesc` (RVA `0xC450` in `libxess_fg.dll` v1.3.1.78) revealed the execution path leading to `0xFFFFFFFC`:

1. `xefgSwapChainD3D12InitFromSwapChainDesc` calls internal validation function `0x1801ff980`:
   ```x86asm
   1801ff9a4: callq 0x1800037f0
   1801ff9ae: movq  %rax, %rcx
   1801ff9b1: callq 0x18000cfc0          ; queries Intel graphics driver
   1801ff9b6: cmpb  $0x0, 0x48(%rsp)     ; checks Intel driver version >= 1.3.0.0
   1801ff9bb: je    0x1801ffb41          ; branches if non-Intel GPU or no Intel driver!
   ...
   1801ff9eb: movabsq $0x1000300000000, %rax
   1801ff9f5: cmpq  %rax, %r8
   1801ff9f8: jb    0x1801ffb41          ; version check fallback
   ```

2. On NVIDIA Ampere (GA103 RTX 3080 Ti Laptop) under Linux/Wine, no Intel driver is present, so execution **unconditionally branches to `0x1801ffb41`**:
   ```x86asm
   1801ffb41: movl  0xc(%rsi), %esi      ; rsi = &params, offset 0xC is maxInterpolatedFrames
   1801ffb44: cmpl  $0x1, %esi           ; strictly validates params.maxInterpolatedFrames == 1!
   1801ffb47: je    0x1801ffbaf          ; IF EQUAL: returns true (al = 1)
   ; IF NOT EQUAL:
   1801ffb3d: xorb  %al, %al             ; returns false (al = 0)
   ```

3. When `0x1801ff980` returns `al = 0`, `D3D12InitFromSwapChainDesc` immediately returns:
   ```x86asm
   180201301: testb %al, %al
   180201303: jne   0x18020130f
   180201305: movl  $0xfffffffc, %eax    ; returns XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT
   18020130a: jmp   0x18020151f
   ```

### B. The Defect Introduced in Commit `55b47147`

In commit `55b47147`, to prevent OptiScaler from clamping multipliers in `SetInterpolatedFrameCount` and `Dispatch()`, `CreateSwapchain` and `CreateSwapchain1` set:
```cpp
params.maxInterpolatedFrames = intTarget; // where intTarget = effectiveMax = 5
```
Passing `params.maxInterpolatedFrames = 5` directly violated Intel's validation gate `cmpl $1, %esi` at `0x1801ffb44`, causing `D3D12InitFromSwapChainDesc` to immediately fail with `0xFFFFFFFC`!

### C. Multiplier Control Contract in Modern `libxess_fg.dll`

Intel's `libxess_fg.dll` has two distinct multiplier management mechanisms:
1. **Creation Time (`D3D12InitFromSwapChainDesc`)**: `params.maxInterpolatedFrames` represents the baseline initialization requirement. On non-Intel GPUs, it **must** be `1`.
2. **Runtime Dynamic Multiplier (`SetNumInterpolatedFrames`)**: Calling `XeFGProxy::SetNumInterpolatedFrames()(_swapChainContext, count)` at runtime sets the internal multiplier (1 to 5). In `libxess_fg.dll`, the internal context ceiling `0x16c(%rdi)` is patched by XeMFG patch `U4`, and default ceiling by `U3`, allowing runtime multipliers from 2X to 6X without needing `params.maxInterpolatedFrames > 1` during swapchain init.

### D. Startup Multiplier Synchronization Defect

In earlier builds, `_framesToInterpolate` was pre-initialized to `FGXeFGInterpolationCount.value_or_default()`. In `Dispatch()`, the check:
```cpp
if (_framesToInterpolate != Config::Instance()->FGXeFGInterpolationCount.value_or_default())
```
evaluated to false on frame 1. Consequently, `SetNumInterpolatedFrames` was never called until the user manually changed settings in the ImGui overlay, causing the game to stay at stock 2X FG until touched.

### E. Quirk Lifecycle Defect in `dllmain.cpp`

In `dllmain.cpp`, `CheckQuirks` checked `activeFgOutput == FGOutput::XeFG`. If false, it executed `quirks.reset(GameQuirk::ForceBorderlessWhenUsingXeFG)`. Because `CheckQuirks` ran before pipeline auto-configuration in some code paths, the quirk was permanently destroyed, preventing `ForceBorderlessWhenUsingXeFG` from enforcing borderless windowing.

---

## 4. Applied Solution

### 1. Robust SwapChain Init Parameters in `XeFG_Dx12.cpp`
In both `CreateSwapchain` and `CreateSwapchain1`:
```cpp
if (Config::Instance()->ForceXeLL.value_or_default() || XeFGProxy::SetNumInterpolatedFrames() != nullptr)
    params.maxInterpolatedFrames = 1;
else
    params.maxInterpolatedFrames = intTarget;
```
When `SetNumInterpolatedFrames` is present (modern `libxess_fg.dll`), `params.maxInterpolatedFrames` is always passed as `1`, cleanly passing Intel's `0x1801ffb44` validation on all GPU vendors.

### 2. Immediate Post-Creation Multiplier Binding
Right after `D3D12GetSwapChainPtr`:
```cpp
_framesToInterpolate = 0;
const auto targetCount = Config::Instance()->FGXeFGInterpolationCount.value_or_default();
if (XeFGProxy::SetNumInterpolatedFrames() != nullptr)
{
    SetInterpolatedFrameCount(targetCount);
}
else
{
    _framesToInterpolate = targetCount;
}
```
Setting `_framesToInterpolate = 0` and immediately invoking `SetInterpolatedFrameCount(targetCount)` guarantees that `libxess_fg.dll` receives the user-configured frame count (e.g. 2 for 3X FG, 5 for 6X FG) on frame 1.

### 3. Non-Destructive Quirk Lifecycle in `dllmain.cpp`
- Removed destructive `else quirks.reset(...)` for `ForceBorderlessWhenUsingXeFG` and `OverrideVsyncWhenUsingXeFG` in `CheckQuirks`.
- Added a post-pipeline configuration check after `activeFgOutput` and `activeFgInput` are resolved, automatically applying `FGXeFGForceBorderless` whenever `activeFgOutput == FGOutput::XeFG` unless explicitly overridden in `OptiScaler.ini`.

---

## 5. Verification & Test Results

1. **Automated Unit Testing (`tests/xemfg_multiplier_and_burst_pacing_unit.cpp`)**:
   - `Test 9`: Validated swapchain init parameters on non-Intel GPUs. Verified that `params.maxInterpolatedFrames = 5` is rejected, while `params.maxInterpolatedFrames = 1` succeeds cleanly.
   - `Test 10`: Validated immediate startup multiplier application on frame 1 without manual interaction across 3X and 6X FG.
   - `Test 11`: Validated `ForceBorderlessWhenUsingXeFG` quirk application upon XeFG pipeline activation.
   - All 11 unit tests passed with 100% success rate.
2. **Coding Standards**:
   - Preserved UTF-8 BOM (`\xef\xbb\xbf`) on all source files.
   - Verified `clang-format --dry-run --Werror` passed with zero violations.
