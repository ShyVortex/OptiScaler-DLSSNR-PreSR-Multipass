# OptiFG with Intel XeFG: Frame Time Delta Correction and VKD3D Pacing Resolution

## 1. Overview & Verified Hardware Environment

- **OS / Distribution**: CachyOS Linux x86_64 (`cachyos`, rolling release)
- **Kernel Version**: `7.2.9-2-cachyos`
- **GPU**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (GA103, `0x170`, 16384 MiB VRAM)
- **NVIDIA Driver**: `580.178.04`
- **Display Resolution / Refresh**: 2560x1440 @ 164.9 Hz
- **Proton Runtime**: Proton Experimental (VKD3D-Proton / Wine 11.0)
- **Game Tested**: *Helldivers 2* (`helldivers2.exe`) via Steam

---

## 2. Reported Issue & Telemetry Analysis

During gameplay in *Helldivers 2* with build `pre8`, severe frame drops and stuttering persisted when using OptiFG (`FGInput::Upscaler`) and XeFG output (`FGOutput::XeFG` via `XeMFG-SR`).

Analyzing the runtime log (`/mnt/DataCachy/SteamLibrary/steamapps/common/Helldivers 2/bin/OptiScaler.log`) revealed two distinct issues:

1. **Sub-Millisecond `frameRenderTime` Corruption**:
   ```
   Reset: 0, Opti FT: 7.9575, Source FT: 35.6063, Set FT: 7.9575
   Reset: 0, Opti FT: 8.4594, Source FT: 46.7567, Set FT: 8.4594
   Reset: 0, Opti FT: 0.4845, Source FT: 35.0080, Set FT: 0.4845
   Reset: 0, Opti FT: 0.5220, Source FT: 15.3206, Set FT: 0.5220
   ```
   - The game engine's true frame interval was **35.6 ms to 47.4 ms** (measured in `_ftDelta[fIndex]`).
   - However, `XeFG_Dx12::Dispatch` fed `XeFGPacing::RenderTimeMs()` into `constData.frameRenderTime`.
   - `XeFGPacing::RenderTimeMs()` measured internal burst-level present intervals and collapsed down to **0.48 ms, 0.52 ms, and 7.95 ms**.
   - Intel's `libxess_fg.dll` scales motion vectors and extrapolates intermediate frames using `constData.frameRenderTime`. When fed 0.48 ms instead of 35.0 ms, motion vectors were scaled down by **98%**, effectively disabling motion extrapolation on intermediate frames and causing extreme visual judder.

2. **VKD3D-Proton VSync Lock Trap**:
   - `[XeMFG] ExtraPacing` was enabled in `OptiScaler.ini` (`ExtraPacing = true`).
   - In `FG_Hooks.cpp`, the VBlank sync lock was guarded only by `!usesDxvk`. Under D3D12/VKD3D-Proton, `usesDxvk` is `false`, causing `ExtraPacing` to execute:
     ```cpp
     if (SyncInterval < 1)
         SyncInterval = 1;
     Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
     ```
   - This forcibly locked every swapchain presentation in the Multi-Frame Generation burst to VSync under Vulkan WSI.
   - Presenting 4 to 6 frames per cycle with VSync lock caused Intel's native presentation scheduler (`g_schedNative`) to spend **8.40 ms per call** inside its wait loop:
     `scheduler 9913 calls, 0 refused, 8.40 ms avg inside; deadlines 13710 calls`.
   - Spending 25–42 ms per cycle stalled the game engine thread, collapsing the real engine frame rate from 35 FPS down to **9.6 FPS** (`real frame 104.59 ms`).

---

## 3. Applied Solution

### A. Prioritize True Engine Frame Delta for OptiFG (`XeFG_Dx12.cpp`)
In `XeFG_Dx12::Dispatch`, when OptiFG is active (`state.activeFgInput == FGInput::Upscaler`), `frameRenderTime` now strictly prioritizes the measured engine frame delta `_ftDelta[fIndex]` and `state.lastFGFrameTime`:
```cpp
double frameRenderTime = 0.0;

if (state.activeFgInput == FGInput::Upscaler)
{
    // For OptiFG / XeMFG-SR, prioritize the measured engine frame delta
    // from swapchain / DLSS-SR upscaler passes over QPC burst intervals
    if (_ftDelta[fIndex] > 0.0)
        frameRenderTime = _ftDelta[fIndex];
    else if (state.lastFGFrameTime > 0.0)
        frameRenderTime = state.lastFGFrameTime;
    else
        frameRenderTime = XeFGPacing::RenderTimeMs();

    if (frameRenderTime < 1.0 && state.lastFGFrameTime >= 1.0)
        frameRenderTime = state.lastFGFrameTime;
}
else
{
    // For native DLSSG input, prioritize XeFGPacing's decoupled render time
    frameRenderTime = XeFGPacing::RenderTimeMs();

    if (!(frameRenderTime > 0.0))
        frameRenderTime = _ftDelta[fIndex];

    if (!(frameRenderTime > 0.0))
        frameRenderTime = state.lastFGFrameTime;
}
```

### B. Exempt Linux and VKD3D-Proton from `ExtraPacing` VSync Lock (`FG_Hooks.cpp`)
In `FG_Hooks.cpp`, the `ExtraPacing` branch is now strictly disabled under Linux, Wine, VKD3D-Proton, and DXVK:
```cpp
else if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::XeFG &&
         config->XeMfgExtraPacing.value_or(false) && !state.isRunningOnLinux &&
         !IdentifyGpu::getPrimaryGpu().usesDxvk && !IdentifyGpu::getPrimaryGpu().usesVkd3dProton)
```
- In `OptiScaler.ini` and `Config.cpp`, `ExtraPacing` defaults to `false`.

---

## 4. Verification

- **Automated Tests**:
  - `tests/optifg_xefg_frametime_pacing_unit.cpp`: Validates that OptiFG prioritizes true engine deltas, filters sub-millisecond noise, and bypasses VSync locks on Linux/VKD3D.
  - All 10 repository unit tests compile and pass cleanly with 100% success rate.
- **Formatting**: Validated with `clang-format --dry-run --Werror`, UTF-8 BOM (`\xef\xbb\xbf`) preserved across all modified files.
