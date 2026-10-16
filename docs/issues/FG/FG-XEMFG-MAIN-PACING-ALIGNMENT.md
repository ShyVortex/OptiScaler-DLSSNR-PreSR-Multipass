# Intel XeMFG Pacing: Main Branch Alignment and Custom Overhaul Regression Resolution

## 1. Overview & Verified Hardware Environment

- **OS / Distribution**: CachyOS Linux x86_64 (`cachyos`, rolling release)
- **Kernel Version**: `7.2.9-2-cachyos`
- **GPU**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (GA103, `0x170`, 16384 MiB VRAM)
- **NVIDIA Driver**: `580.178.04`
- **Display Resolution / Refresh**: 2560x1440 @ 164.9 Hz
- **Proton Runtime**: Proton Experimental (VKD3D-Proton / Wine)
- **Game Tested**: *Helldivers 2* (`helldivers2.exe`) via Steam

---

## 2. Reported Issue & Telemetry Analysis

During an extended, CPU-heavy gameplay mission in *Helldivers 2* with build `pre7` (`840aada9`), the user recorded severe, catastrophic frametime inconsistency and micro-stutter:
- Video evidence: `/home/angelo/Video/helldivers2_unstable_gameplay.mp4` (60.78s, 2560x1440 @ 60fps).
- MangoHud telemetry showed framerate fluctuating between 100–112 FPS (4X Multi-Frame Generation on a ~25–28 FPS native engine), but the frametime graph displayed a continuous, extreme sawtooth comb pattern with spikes up to 20–30+ ms.
- Runtime log: `/mnt/DataCachy/SteamLibrary/steamapps/common/Helldivers 2/bin/OptiScaler.log` (83 MB, 1,131,770 lines).
- Pacing cadence recorded in log:
  ```
  Frame 1:  0.75 ms
  Frame 2:  1.05 ms
  Frame 3:  7.50 ms
  Base:    16.30 ms
  Total Cycle: ~25.6 ms
  ```
- Out of 784 recorded frames, 327 were logged as clumps (<1 ms gap) with minimum gap `0.75 ms`.
- `XeFGPacing::ReportStats` logged:
  `real frame 7.62 ms (131.2 fps), target 1.91 ms/frame, fed 7.62 ms`.

---

## 3. Root Cause Analysis: The Custom Overhaul Regression

Comparing the implementation on branch `xemfg-global-injector` with the reference `main` branch (where `XeFGPacing` works properly in native DLSS-FG games):

1. **The Competing Pacing War**:
   On branch `xemfg-global-injector`, two pacing mechanisms fought each other:
   - Intel's native provider scheduler detour (`0x21EE30` / `0x225070` via `g_schedNative`).
   - OptiScaler's custom wall-clock pacer (`PaceFrame`).
2. **Uninitialized `g_burstAnchorQpc` in `PaceFrame`**:
   In `TryPace`, intermediate frames (1 and 2) were routed to `ScheduleFrame` and bypassed `PaceFrame`. Because `g_burstAnchorQpc` was only initialized on `index == 1` inside `PaceFrame`, it was never updated! When the final frame (index 3) called `PaceFrame`, its target deadline was either 0 or stale, immediately falling through to the anti-clump floor `g_lastPresentedQpc + intervalQpc / 2` (~7.5 ms).
3. **Base Frame Thread Blocking Trap**:
   `libxess_fg.dll` executes `xefgSwapChainPresent` synchronously on the game engine's main render thread. Intercepting base frame callers (`0x220A52`, `0x220DAD`) inside `NoteBasePresent` and introducing `WaitUntil` anti-clump delays directly stalled the engine from submitting subsequent frames, collapsing the measured period to 7.62 ms and warping presentation cadence.
4. **Why `main` Works Properly**:
   - `main` delegates intermediate frame pacing to Intel's native background scheduler (`g_schedNative` at `0x21EE30`), which spins until timestamps rebased by `TsDetour`.
   - `TsDetour` detects Intel's bug where `ringMeasuredOffset < median` clamps the step, and expands `g_nextDeadlineNs` by the true uniform unit `median / countPlus1`.
   - `main` lets Intel handle the last generated frame natively (`ProviderPacesLastFrame`).
   - `main` **never touches or delays the base application frame**: base frame callers bypass `TryPace` entirely in `Detour`, allowing the game engine to present immediately with 0 detour overhead.
   - `NoteFrame` measures engine frame time purely burst-to-burst (`nowQpc - lastBurstQpc`) at `index == 1`, filtering pause/spike (>500 ms) and micro (<1 ms) noise.

---

## 4. Applied Solution

1. **Full Restoration of `OptiScaler/framegen/xefg/XeFGPacing.h` to `main` Architecture**:
   - Restored clean `Detour`, `TryPace`, `ScheduleFrame`, `ProviderPacesLastFrame`, `TsDetour`, and `NoteFrame`.
   - Preserved strict MSVC UTF-8 BOM (`\xef\xbb\xbf`) and verified `clang-format --dry-run --Werror`.
2. **Clean Alignment of `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`**:
   - Removed heuristic clamp `if (frameRenderTime < 1.0 || (_ftDelta[fIndex] > 0.0 && frameRenderTime < 0.25 * _ftDelta[fIndex]))` from line 995, restoring `main`'s clean fallback cascade:
     ```cpp
     auto frameRenderTime = XeFGPacing::RenderTimeMs();

     if (!(frameRenderTime > 0.0))
         frameRenderTime = _ftDelta[fIndex];

     if (!(frameRenderTime > 0.0))
         frameRenderTime = state.lastFGFrameTime;
     ```
3. **Automated Unit Test Verification**:
   - Created `tests/xefg_main_pacing_alignment_unit.cpp` validating:
     - Burst-to-burst period measurement in `NoteFrame`
     - `TsDetour` clamp repair and uniform deadline rebasing
     - Base frame zero-wait bypass in `OnDetour`
     - Provider scheduler delegation without redundant waits
   - Verified that all existing unit tests (`xefg_ring_buffer_pacing_unit`, `xefg_pacing_engine_unit`, `xemfg_multiplier_and_burst_pacing_unit`, `xemfg_loader_unit`, `xemfg_sr_unit`, `xemfg_sr_crash_fix_unit`, `xefg_mfg_streamline_unit`, `xemfg_menu_exclusion_unit`) compile and pass.

---

## 5. Verification Checklist

- [x] Verified `XeFGPacing.h` is identical to `main` (with clang-format compliance).
- [x] Verified `XeFG_Dx12.cpp` fallback logic matches `main`.
- [x] Verified UTF-8 BOM (`\xef\xbb\xbf`) on all modified and new C++ source/header files.
- [x] Verified `clang-format --dry-run --Werror` passes on all modified files.
- [x] Verified all test suites pass with zero regressions.
