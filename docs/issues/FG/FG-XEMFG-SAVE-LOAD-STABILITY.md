# XeMFG Save-Load Black Screen, Presentation Pacing Stutter & XeSS 3.0 SDK Staging Resolution

## 1. Overview & Context

- **Environment**: Windows 11 x64, NVIDIA GeForce RTX 3080 Ti Laptop GPU (`0x170` Ampere architecture), Direct3D 12.
- **Target Applications**:
  - *Resident Evil Requiem* (`re9.exe`, Capcom RE Engine).
  - *The Last of Us Part II Remastered* (`tlou-ii.exe`, Naughty Dog Engine).
- **Target Runtimes**: Intel XeSS Frame Generation (`libxess_fg.dll` / `libxess.dll` / `libxell.dll` v3.0+ SDK) with native XeMFG multi-frame generation, and AMD FSR 3.1 Frame Generation.
- **Diagnostic Traces**: `OptiScaler_test6.log` (LogLevel=1) and `OptiScaler_test7.log` (LogLevel=1).

---

## 2. Reported Symptoms

Following the initial engagement of XeFG 2X and Multi-Frame Generation, the following issues were reported:
1. **Resident Evil Requiem Startup Crash**:
   - The game failed to launch into the menu, terminating with an unhandled exception pointing to `dxgi.dll!0x7ffca1f10000 + 0x13dc9f`, `RTSSHooks64.dll!0x180000000 + 0x720c1`, and `sl.interposer.dll!0x7ffcc8f00000 + 0x279fc`.
2. **The Last of Us Part II Black Screen & Framerate Degradation**:
   - The game launched, but soon transitioned into a permanent black screen with no way to proceed, accompanied by framerate degrading progressively down to near-zero.
3. **Save-Load Stability in RE Requiem**:
   - Loading saves with FG enabled previously exhibited black screen states when tagged resources became unavailable during loading screens.
4. **XeSS 3.0+ SDK Staging in CI Workflows**:
   - Ensure release packaging and GitHub Actions dynamically retrieve the official XeSS 3.0+ SDK binaries.

---

## 3. Issue Validity Assessment & Root Cause Analysis

### 3.1 RE Requiem Startup Crash (`OptiScaler_test7.log`)
`OptiScaler_test7.log` revealed:
```text
[17:24:10.530516] [D] DxgiFactoryHooks::CreateSwapChainForHwnd Width: 2560, Height: 1440, Format: 24, Count: 3, Flags: 842, Hwnd: C006C, SkipWrapping: false
[17:24:10.530555] [I] XeFG_Dx12::CreateSwapchain1 Releasing old swapchain
[17:24:10.535846] [D] XeFG_Dx12::CreateSwapchain1 Releasing swapchain, ref count: 2
[17:24:10.572240] [D] XeFG_Dx12::CreateSwapchain1 Releasing swapchain, ref count: 3221225472
```
- **Mechanism**:
  - `GameQuirk::DoNotPreserveFGSwapChain` had been applied to `re9.exe`, causing `FGPreserveSwapChain` to resolve to `false`.
  - When RE Engine launched and transitioned from its 1080p splash video to the 1440p main menu via `CreateSwapChainForHwnd`, `XeFG_Dx12::CreateSwapchain1` bypassed `ResizeBuffers` and entered lines 484–500:
    ```cpp
    if (State::Instance().currentRealSwapchain != nullptr) {
        UINT release = 0;
        do {
            release = State::Instance().currentRealSwapchain->Release();
        } while (release > 0);
    }
    ```
  - Forcibly invoking `Release()` in a loop on `currentRealSwapchain` until ref count hit 0 destroyed the active DXGI swapchain COM object out from under concurrent render and hook threads (`RTSSHooks64.dll`, `sl.interposer.dll`, `dxgi.dll`).
  - Ref count underflowed to `3221225472` (`0xC0000000`), immediately producing an access violation `0xC0000005`.
  - **Resolution**: Removed `GameQuirk::DoNotPreserveFGSwapChain` from `re9.exe` and `re9demo.exe`, and deleted the destructive `while (release > 0) currentRealSwapchain->Release();` loops from `XeFG_Dx12.cpp`. RE Engine smoothly resizes its swapchain via `ResizeBuffers`.

### 3.2 The Last of Us Part II Black Screen & Queue Starvation
- **Mechanism**:
  - In `IFGFeature::StartNewFrame()`, the discontinuity threshold had been widened to `(_frameCount - _lastDispatchedFrame) >= BUFFER_COUNT`.
  - `tlou-ii.exe` specifies `GameQuirk::AllowedFrameAhead2` (`FGAllowedFrameAhead = 2`).
  - When frames were rendered across cutscenes or loading transitions without dispatch, `_frameCount - _lastDispatchedFrame` reached 2.
  - Because `2 < BUFFER_COUNT (4)`, `StartNewFrame()` never resynchronized `_lastDispatchedFrame`.
  - In `GetDispatchIndex`, since `diff == 2 <= FGAllowedFrameAhead (2)`, it never caught up to `_frameCount`; instead it continually requested `_lastDispatchedFrame + 1` (historical frame `_frameCount - 1`).
  - Because the 4-slot ring buffer had already cleared and overwritten that slot, `FSRFG_Dx12::Dispatch` repeatedly logged `"Depth or Velocity is not ready, skipping"` and skipped dispatch.
  - `_lastDispatchedFrame` became locked 2 frames behind. Every subsequent frame attempted to dispatch an expired historical slot and failed, presenting no frames and causing a permanent black screen while presentation queue latency spiraled downward to 0 FPS.
  - Furthermore, `wrapped_swapchain.cpp` line 450 applied `XeMfgExtraPacing` without checking `fgFeatureActive`, forcing `SyncInterval = 1` even during passthrough or when FG was off.
  - **Resolution**: Restored `if (_lastDispatchedFrame == 0 || (_frameCount - _lastDispatchedFrame) > 2) _lastDispatchedFrame = _frameCount - 1;` in `IFGFeature.cpp`. In `wrapped_swapchain.cpp`, guarded `XeMfgExtraPacing` with `fg != nullptr && fg->IsActive() && !fg->IsPaused()`.

### 3.3 Motion Vector Scale Safety
- Guarded `motionVectorScaleX` and `motionVectorScaleY` in `XeFG_Dx12::Dispatch()` so 0.0f inputs safely default to 1.0f, preventing `XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT`.

---

## 4. Solutions Applied

1. **Quirks & Swapchain Preservation (`Quirks.h`, `XeFG_Dx12.cpp`)**:
   - `re9.exe` and `re9demo.exe` preserve swapchains (`FGPreserveSwapChain = true`) and utilize `AllowedFrameAhead2`.
   - Eliminated all destructive COM release loops on `currentRealSwapchain`.
2. **Dynamic Passthrough & Error -14 Prevention (`XeFG_Dx12.cpp`, `FG_Hooks.cpp`)**:
   - In `XeFG_Dx12::Present()`, when `Dispatch()` returns `false` (due to missing depth/velocity during save/load screens), OptiScaler immediately invokes `XeFGProxy::SetEnabled(_swapChainContext, false)`, sets `_needResetHistory = true`, and syncs `_lastDispatchedFrame = _frameCount`.
   - Intel's `libxess_fg.dll` runtime transitions to passthrough mode instead of expecting frame tags, eliminating `XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES` (-14) and permanent black screens.
   - In `FG_Hooks.cpp`, `state.fgPresentIsCalled` and `fg->PostPresent()` are strictly gated behind `fgDispatched`, ensuring un-dispatched loading frames are not treated as interpolated frames by the swapchain wrapper.
3. **Temporal History Flush & Seamless Resumption (`XeFG_Dx12.cpp`)**:
   - When 3D world rendering resumes with valid resources, `Dispatch()` re-enables `XeFGProxy::SetEnabled(_swapChainContext, true)` and tags `constData.resetHistory = true`, flushing stale motion vectors from before the save/load transition.
   - In `XeFG_Dx12::Activate()`, added `State::Instance().activeFgInput == FGInput::DLSSG` to the activation condition so DLSS Frame Generation inputs (which provide dilated display-resolution motion vectors) activate cleanly without requiring manual quirk overrides.
4. **Frame Counter Synchronization & Jump Warning Elimination (`IFGFeature.cpp`)**:
   - In `IFGFeature::StartNewFrame()`, synchronized `_lastDispatchedFrame = _frameCount` during inactive or passthrough states (`!IsActive() || IsPaused()`), completely suppressing the 3,687 spurious `[W] IFGFeature::StartNewFrame Frame count jumped too much!` disk I/O log writes on the render thread.
   - Startup initialization (`_lastDispatchedFrame == 0`) now smoothly initializes `_lastDispatchedFrame = _frameCount - 1` without emitting an erroneous warning.
   - In `IFGFeature::SetFrameCount(frameId)`, protected against backwards regression under `AllowedFrameAhead2` when presentation markers lag render slots by $\le 4$ frames, preventing `-1` collisions in `GetDispatchIndex` and alternating frame drops.
5. **Remote SDK Retrieval (`package_release.ps1`, Workflows)**:
   - Automated dynamic retrieval of official Intel XeSS 3.0+ SDK binaries from `https://raw.githubusercontent.com/intel/xess/main/bin/`.

---

## 5. Automated Verification

- **Save/Load Stability & Presentation Pacing Test** (`tests/xefg_save_load_stability_unit.cpp`):
  - Validated dynamic `SetEnabled(false)` passthrough when depth/velocity inputs are unready during save/load screens.
  - Validated runtime re-activation with `resetHistory = true` temporal flush upon gameplay resumption.
  - Validated zero spurious jump warnings emitted across 120 loading screen frames.
  - Validated pipelined `SetFrameCount` render slot preservation under `AllowedFrameAhead2` and genuine counter resets.
  - Validated unmetered presentation preservation without forced VBlank locks.
- **Swapchain Lifecycle Test** (`tests/xefg_swapchain_lifecycle_unit.cpp`):
  - Validated swapchain preservation on `re9.exe`.
  - Validated clean `ResizeBuffers` across resolution changes (1080p splash to 1440p menu) without swapchain destruction.
  - Validated clean recreation fallback on `ResizeBuffers` failure.
  - Validated full state reset in `ReleaseSwapchain`.
- **Ring Buffer Pacing Test** (`tests/xefg_ring_buffer_pacing_unit.cpp`):
  - Validated pipelining up to 2 frames ahead without spurious jumps.
  - Validated that gaps > 2 immediately snap `_lastDispatchedFrame = _frameCount - 1` to prevent starvation loops.
  - Validated motion vector scale zero fallback.
- **Formatting & MSVC Compliance**:
  - UTF-8 BOM (`\xef\xbb\xbf`) preserved on all modified files.
  - Passed `clang-format --dry-run --Werror` cleanly with 0 violations.
