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
2. **Ring Buffer Resynchronization (`IFGFeature.cpp`)**:
   - Restored proven `diff > 2` resynchronization clamp in `StartNewFrame()`, preventing stale buffer starvation in games using `AllowedFrameAhead2`.
3. **Pacing Gating (`wrapped_swapchain.cpp`)**:
   - `XeMfgExtraPacing` is strictly gated behind `fg != nullptr && fg->IsActive() && !fg->IsPaused()`.
4. **Remote SDK Retrieval (`package_release.ps1`, Workflows)**:
   - Automated dynamic retrieval of official Intel XeSS 3.0+ SDK binaries from `https://raw.githubusercontent.com/intel/xess/main/bin/`.

---

## 5. Automated Verification

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
