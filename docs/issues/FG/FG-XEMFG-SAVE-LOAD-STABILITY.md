# XeMFG Save-Load Black Screen, Presentation Pacing Stutter & XeSS 3.0 SDK Staging Resolution

## 1. Overview & Context

- **Environment**: Windows 11 x64, NVIDIA GeForce RTX 3080 Ti Laptop GPU (`0x170` Ampere architecture), Direct3D 12.
- **Target Application**: *Resident Evil Requiem* (`re9.exe`, Capcom RE Engine).
- **Target Runtime**: Intel XeSS Frame Generation (`libxess_fg.dll` / `libxess.dll` / `libxell.dll` v3.0+ SDK) with native XeMFG multi-frame generation.
- **Diagnostic Trace**: `OptiScaler_test6.log` (505,960 lines, 43MB, LogLevel=1).

---

## 2. Reported Symptoms

Following the successful engagement of XeFG 2X and Multi-Frame Generation in *Resident Evil Requiem*, two critical stability issues and one build/packaging requirement were identified:
1. **Black Screen on Save Game Load with FG Enabled**:
   - Loading a saved game while DLSS FG / XeFG was active resulted in a permanent black screen where the 3D scene and pause menu failed to render, requiring Alt+F4 to quit.
   - Workaround: Disabling DLSS FG in-game prior to loading a save allowed the game to load normally, after which FG could be toggled on.
2. **Persistent Stuttering & Periodic Frame Drops During Gameplay**:
   - While XeFG produced interpolated frames (verified by debug markers and telemetry), significant motion instability, stutters, and periodic drops back to native 1X presentation remained.
3. **XeSS 3.0+ SDK Build Process Integration**:
   - Ensure release archives and CI workflows consistently stage the official XeSS 3.0+ SDK binaries, fetching dynamically from `https://github.com/intel/xess/tree/main/bin` if local files are missing.

---

## 3. Issue Validity Assessment & Trace Analysis (`OptiScaler_test6.log`)

Rigorous analysis of `OptiScaler_test6.log` confirmed all reported issues were genuine defects in OptiScaler's frame generation core and lifecycle management:

### 3.1 Ring Buffer Desynchronization (4,420 Occurrences)
In `OptiScaler_test6.log`, the following warning fired **4,420 times** throughout the session:
```text
[W] IFGFeature::StartNewFrame Frame count jumped too much! _frameCount: 13944, _lastDispatchedFrame: 13941
[W] IFGFeature::StartNewFrame Frame count jumped too much! _frameCount: 13942, _lastDispatchedFrame: 13939
[W] IFGFeature::StartNewFrame Frame count jumped too much! _frameCount: 13940, _lastDispatchedFrame: 13937
```
- **Mechanism**:
  - Capcom RE Engine employs multi-frame pipelining: CPU command recording runs 2 frames ahead of GPU present.
  - In `IFGFeature::StartNewFrame()`, a hardcoded check evaluated:
    `if (_lastDispatchedFrame == 0 || (_frameCount - _lastDispatchedFrame) > 2)`
  - When `_frameCount - _lastDispatchedFrame == 3` (completely valid in a 4-slot ring buffer `BUFFER_COUNT = 4`), `StartNewFrame` falsely concluded a frame jump had occurred and forcibly set `_lastDispatchedFrame = _frameCount - 1`.
  - When `XeFG_Dx12::Dispatch` subsequently called `GetDispatchIndex`, it attempted to dispatch `_lastDispatchedFrame + 1`—a future frame whose depth and motion vectors had not finished GPU rendering.
  - `XeFG_Dx12::Dispatch` logged:
    ```text
    [W] XeFG_Dx12::Dispatch Depth or Velocity is not ready, skipping
    [W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=-14 (XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES), framesPresented=1
    ```
  - This aborted interpolation and fell back to presenting only 1 native frame every 1–2 seconds, causing continuous frametime spikes and presentation stutter.

### 3.2 Motion Vector Scale Rejection
`OptiScaler_test6.log` captured:
```text
[E] XeFG Log: XeFG: Invalid argument. Motion vector scale should not be 0.
[W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=-4 (XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT), framesPresented=1
```
- When Streamline inputs passed 0.0f or uninitialized motion vector scales, `libxess_fg.dll` rejected the frame constants with `XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT`, dropping presentation back to 1 frame.

### 3.3 Swapchain Recreation Collision on Save Game Load
`OptiScaler_test6.log` recorded:
```text
[12:10:08.027791] [I] Destroyed DXGISwapChain proxy ... native swap-chain ref count 0
[12:10:08.027827] [E] 'kFeatureDLSS_G' context is missing.
[12:10:08.027880] [D] DxgiFactoryHooks::CreateSwapChainForHwnd Width: 2560, Height: 1440
[12:10:08.027897] [W] FGHooks::CreateSwapChainForHwnd Looks like game is creating new swapchain, without releasing old one!
[12:10:08.027968] [W] XeFG_Dx12::CreateSwapchain1 FG swapchain already created for the same output window!
```
- During level/save loading, RE Engine destroys its existing DXGI swapchain and calls `CreateSwapChainForHwnd`.
- Streamline destroyed its `kFeatureDLSS_G` context and DXGISwapChain proxy.
- Because `FGPreserveSwapChain` defaults to `true` and `re9.exe` lacked `GameQuirk::DoNotPreserveFGSwapChain`, OptiScaler attempted `ResizeBuffers` on the old `currentFGSwapchain`.
- Handing back a zombie proxy swapchain detached from the game's newly created native swapchain left the game presenting into the void, resulting in an unrecoverable black screen.

---

## 4. Solutions Applied

### 4.1 Ring Buffer Pipelining & Threshold Fix (`IFGFeature.cpp`)
- Replaced the hardcoded `> 2` check with `(_frameCount - _lastDispatchedFrame) >= BUFFER_COUNT`.
- Pipelined frames with differences up to 3 are now correctly preserved without spurious jump clamps or false frame advances.

### 4.2 Game Engine Quirks for RE Requiem (`Quirks.h`)
- Added `GameQuirk::AllowedFrameAhead2` to `re9.exe` and `re9demo.exe` to allow RE Engine's 2-frame pipelining without premature dispatch catch-up.
- Added `GameQuirk::DoNotPreserveFGSwapChain` to `re9.exe` and `re9demo.exe` so swapchains are cleanly recreated on save/scene loads.

### 4.3 Motion Vector Scale Safety Guard (`XeFG_Dx12.cpp`)
- Guarded `motionVectorScaleX` and `motionVectorScaleY` in `XeFG_Dx12::Dispatch()`:
  ```cpp
  constData.motionVectorScaleX = (_mvScaleX[fIndex] != 0.0f) ? _mvScaleX[fIndex] : 1.0f;
  constData.motionVectorScaleY = (_mvScaleY[fIndex] != 0.0f) ? _mvScaleY[fIndex] : 1.0f;
  ```
  Preventing `XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT`.

### 4.4 Swapchain Lifecycle & Recreation Hardening (`XeFG_Dx12.cpp`)
- In `CreateSwapchain` and `CreateSwapchain1`:
  - If `FGPreserveSwapChain` is active but `ResizeBuffers` fails, OptiScaler now logs a warning and automatically falls back to full swapchain release and clean recreation.
  - When `readyToRelease` is true or `DoNotPreserveFGSwapChain` is active, the old swapchain is cleanly destroyed before creating the new context.
- In `ReleaseSwapchain`:
  - Reset `_swapChain = nullptr`, `_hwnd = NULL`, `_gameCommandQueue = nullptr`, `_isActive = false`, `_passthrough = false`, `_framesToInterpolate = 0`, and `_haveHudless.reset()`.

### 4.5 Remote XeSS 3.0 SDK Retrieval & CI Staging (`package_release.ps1`, Workflows)
- In `package_release.ps1`:
  - Checks local `XeMFG\SDK\bin\$name` and `external\xess\bin\$name`.
  - If missing locally, dynamically downloads from `https://raw.githubusercontent.com/intel/xess/main/bin/$name`.
- In `.github/workflows/package_release.yml` & `build.yml`:
  - Added dedicated step `Ensure XeSS 3.0+ SDK binaries are present` that verifies all 4 binaries exist in `external/xess/bin` or downloads them directly from `https://raw.githubusercontent.com/intel/xess/main/bin/` if absent.

---

## 5. Automated Verification

- **Ring Buffer & Motion Vector Unit Test** (`tests/xefg_ring_buffer_pacing_unit.cpp`):
  - Validated 4-slot ring buffer progression under 2-frame pipelining without spurious jumps.
  - Validated genuine overflow clamping when `diff >= BUFFER_COUNT`.
  - Validated motion vector scale zero inputs safely defaulting to `1.0f`.
- **Swapchain Lifecycle Unit Test** (`tests/xefg_swapchain_lifecycle_unit.cpp`):
  - Validated `re9.exe` and `re9demo.exe` quirk entries for `AllowedFrameAhead2` and `DoNotPreserveFGSwapChain`.
  - Validated clean swapchain recreation during simulated save loads.
  - Validated `ResizeBuffers` failure fallback recreation.
  - Validated full state reset in `ReleaseSwapchain`.
- **Formatting & CI Compliance**:
  - Preserved UTF-8 BOM on all modified files.
  - Passed `clang-format --dry-run --Werror` cleanly.
