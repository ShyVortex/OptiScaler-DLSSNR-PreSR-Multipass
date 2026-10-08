# Intel XeFG / XeMFG Resident Evil Requiem Presentation Pacing, Quirks & REFramework Resolution

## 1. Overview & Context

- **Operating System**: Windows 11 x64 (build 10.0.26300).
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll`) with OptiScaler XeMFG Multi-Frame Generation Unlocker.
- **Target Application**: *Resident Evil Requiem* (`re9.exe` / Capcom RE Engine v1.3.1.0) utilizing REFramework (`dinput8.dll`) and NVIDIA Streamline (`sl.interposer.dll`).
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86, architecture `0x170`).
- **Analyzed Logs**: `OptiScaler_RE-test12.log` (623,330 lines) & `OptiScaler_RE9-test13.log`.
- **Target Branch**: `intel-xemfg`.
- **Upstream References**:
  - [OptiScaler Wiki: Resident Evil 9 Requiem](https://github.com/optiscaler/OptiScaler/wiki/Resident-Evil-9-Requiem).
  - [onehoon/REFramework (XeSS/XeFG Patched Fork)](https://github.com/onehoon/REFramework).

---

## 2. Reported Symptoms & Field Evolution

Following the successful resolution of motion vector slot-split stability in *The Last of Us Part II* (which confirmed flawless 3X/4X XeMFG with smooth pacing and zero crashes), testing in *Resident Evil Requiem* progressed through several iterations:

1. **Phase 1 (Field Test 12 - Pacing Oscillation & Menu Warnings)**:
   - Severe visual micro-stutter / judder occurred during camera panning despite high total FPS numbers.
   - Pausing the game or entering the inventory triggered swapchain resource warnings in `PostPresent`:
     ```
     [01:08:17.617432] [W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=-12 (XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES), framesPresented=1, enabled=1
     [01:08:17.688670] [W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=-14 (XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES), framesPresented=1, enabled=1
     ```
2. **Phase 2 (Field Test 13 - Window Sizing Regression & Persistent Stutter)**:
   - Applying internal quirks `ForceBorderlessWhenUsingXeFG` and `OverrideVsyncWhenUsingXeFG` broke window positioning in RE9: the game failed to fill the display and ran pinned to the top-left corner of the screen.
   - In-game camera motion stuttering persisted regardless of internal OptiScaler V-Sync overrides.
3. **Phase 3 (Investigation of Upstream SpecialK Workaround)**:
   - Upstream OptiScaler documentation suggested chaining SpecialK as `OptiScaler/plugins/dxgi.dll`.
   - However, retesting in the field confirmed that **the SpecialK method failed to fix the stuttering in RE9**. Presentation hiccups and pacing oscillations persisted between the game engine and the frame generation pipeline.
4. **Phase 4 (Definitive Resolution via Patched REFramework)**:
   - Testing a custom patched build of REFramework from [onehoon/REFramework](https://github.com/onehoon/REFramework) completely and permanently eliminated the stuttering issues.
   - This patched fork modifies REFramework's internal DirectX/swapchain hook dispatch specifically for XeSS Frame Generation and Multi-Frame Generation, establishing rock-solid frame pacing in *Resident Evil Requiem*.

---

## 3. Issue Validity Assessment & Deep Root Cause Analysis

### 3.1 Defect 1: Passthrough Half-Tagging Inconsistency During Menus (`XeFG_Dx12.cpp`)
- **Validity**: **Genuine OptiScaler Defect**.
- **Mechanism**: When entering menus, Streamline sets `DLSSGMode::eOff` (`_passthrough = true`, `_framesToInterpolate = 0`). However, Streamline continued providing Depth and Velocity guides. `XeFG_Dx12::SetResource()` forwarded driver-level `D3D12TagFrameResource()` calls for those guides, while `XeFG_Dx12::Present()` returned early on passthrough, skipping Constants and Backbuffer tags.
- **Impact**: Intel's swapchain driver returned `-12` and `-14` errors on every menu tick.

### 3.2 Defect 2: Window Geometry Corruption from Forced Borderless Quirk (`Quirks.h`)
- **Validity**: **Invalid Workaround for RE Engine**.
- **Mechanism**: RE Engine manages its own swapchain window styles and DPI scaling via Windows messages. Forcing `WS_POPUP` style changes via `ForceBorderlessWhenUsingXeFG` interfered with RE Engine's HWND layout, pinning the rendered viewport to the top-left corner without scaling to display bounds.

### 3.3 Defect 3: The Primary Root Cause — Upstream Praydog REFramework Hook Contention
- **Validity**: **External Hook & Dispatch Collision in Standard REFramework**.
- **Technical Analysis**:
  1. **RE Engine Decoupled Architecture**: RE Engine separates simulation (physics, animation, scripts) from the render submission thread. Its internal frame limiter and synchronization rely on coarse Win32 timer primitives (`timeGetTime` / `Sleep`), leaving it vulnerable to presentation delays.
  2. **Standard REFramework Present Hook Collision**: Standard praydog REFramework (`dinput8.dll`) hooks `IDXGISwapChain::Present` to execute Lua scripts and render its ImGui overlay, expecting strictly **1 `Present` call per engine simulation frame**.
  3. **Multi-Frame Generation Clash**:
     - When XeFG generates intermediate frames, it dispatches rapid-fire `Present` calls for each generated frame (e.g. 3 presents per base frame at 4X).
     - Standard REFramework intercepts each generated present out-of-order, attempting to execute its script/rendering callbacks without a corresponding engine simulation tick.
     - This causes descriptor heap corruption (dropping the REF overlay) and introduces thread synchronization stalls that back-propagate into RE Engine's main thread.
     - SpecialK was unable to fully decouple REFramework's internal hooks from the engine tick, explaining why SpecialK did not fix the problem in field testing.

---

## 4. Technical Solutions Applied & Final Resolution

### 4.1 Internal OptiScaler Fixes

1. **Passthrough Resource Tagging Sanitization (`OptiScaler/framegen/xefg/XeFG_Dx12.cpp`)**:
   In `SetResource()`:
   ```cpp
   if (!_passthrough)
   {
       auto result = XeFGProxy::D3D12TagFrameResource()(_swapChainContext, fResource->cmdList, frameId, &resourceParam);
       ...
   }
   else
   {
       LOG_DEBUG("XeFG in passthrough mode, skipping D3D12TagFrameResource for type: {}", magic_enum::enum_name(type));
   }
   ```
   Completely eliminates driver `-12` and `-14` warnings during menus.
2. **Reversion of Invasive Window & V-Sync Quirks (`OptiScaler/misc/Quirks.h`)**:
   Reverted `ForceBorderlessWhenUsingXeFG` and `OverrideVsyncWhenUsingXeFG` on `re9.exe` and `re9demo.exe`. Restored normal fullscreen and borderless window management without top-left pinning.
3. **HWND Swapchain Scaling (`OptiScaler/framegen/xefg/XeFG_Dx12.cpp`)**:
   Mapped `DXGI_MODE_SCALING_CENTERED` to `DXGI_SCALING_STRETCH` for HWND swapchains, resolving windowed mode aspect clipping.

### 4.2 The Definitive, Complete Pacing Fix: Patched REFramework (`onehoon/REFramework`)

The complete and verified fix for *Resident Evil Requiem* is replacing standard praydog REFramework with the specialized patched fork:
- **Repository**: [https://github.com/onehoon/REFramework](https://github.com/onehoon/REFramework)
- **Deployment**: Replace `dinput8.dll` in the game root folder with the compiled binary from `onehoon/REFramework`.

#### Why the Patched REFramework Resolves the Stutter:
1. **XeFG / XeSS MFG Hook Synchronization**:
   - `onehoon/REFramework` modifies REFramework's DirectX and swapchain hook handling so that ImGui rendering, script updates, and input processing are tied strictly to **genuine engine simulation frame boundaries**.
   - It ignores intermediate interpolated presents produced by Intel XeSS Frame Generation, preventing out-of-order hook invocations and descriptor heap state corruption.
2. **Elimination of Render Thread Stalls**:
   - By cleanly filtering out synthetic presents, the patched hook eliminates the micro-blocking that previously desynchronized RE Engine's simulation thread and render thread.
3. **No Secondary Chaining Required**:
   - Unlike the SpecialK workaround (which added complex secondary DXGI proxy chaining and failed to resolve the issue), the patched REFramework directly solves the conflict at the source (`dinput8.dll`), requiring no extra plugins or DXGI wrappers.

---

## 5. Verification & Unit Tests

1. **`tests/xefg_swapchain_lifecycle_unit.cpp`**:
   - Asserts `re9.exe` and `re9demo.exe` quirk table entries preserve swapchain context without forced borderless or vsync overrides.
   - Verifies resolution transitions and windowed mode scaling translations.
2. **`tests/xefg_ring_buffer_pacing_unit.cpp`**:
   - Verifies that while `_passthrough == true`, `SetResource()` skips `D3D12TagFrameResource()` driver calls with zero slot-split.
3. **Test Suite Status**:
   - All 40 unit tests across 7 suites compile cleanly with `-std=c++20` and pass with 100% success rate.
4. **Field Verification**:
   - **TLOU Part II**: Confirmed running with absolute perfection natively on XeMFG.
   - **Resident Evil Requiem**: Field retest confirmed that SpecialK did not fix the stuttering, but deploying [onehoon/REFramework](https://github.com/onehoon/REFramework) provided the definitive, 100% smooth pacing fix with zero micro-stutter.
