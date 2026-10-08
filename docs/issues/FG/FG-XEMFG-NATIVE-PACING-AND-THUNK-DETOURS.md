# Intel XeSS Frame Generation (XeFG/XeMFG) Native Thunk Pacing & RE9 Quirks Resolution

## 1. Overview & Environment

- **Operating System**: Windows 11 x64 (build 10.0.26300).
- **Target Application**: *Resident Evil Requiem* (`re9.exe` / Capcom RE Engine v1.3.1.0) utilizing NVIDIA Streamline (`sl.interposer.dll`).
- **Hardware Architecture**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86, architecture `0x170`).
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll` v1.3.1.78, stamp `0x69CB0F4D`, size `0x015ED000`) with OptiScaler XeMFG Multi-Frame Generation Unlocker.
- **Analyzed Logs**: `OptiScaler_RE-test12.log` (623,330 lines) & `OptiScaler_RE-test13.log` (557,066 lines).
- **Branch**: `intel-xemfg`.

---

## 2. Reported Symptoms

1. **RE9 Window Pinning & Corner Snapping**:
   - Applying `ForceBorderlessWhenUsingXeFG` caused *Resident Evil Requiem* to launch in a borderless window pinned to the top-left corner `(0, 0)` of the desktop rather than filling the entire display, preventing full screen or fullscreen windowed gameplay.
2. **Persistent Multi-Frame Judder & Clumping Above 2X**:
   - Despite forcing DXGI VBlank flags (`SyncInterval = 1`, tearing stripped), generated frames in 4X FG were still delivered in an unmetered 11–12 ms clump (e.g. 6.0 ms, 2.5 ms, 3.6 ms) followed by an 18–24 ms stall.
   - Panning and motion in active gameplay exhibited persistent judder and micro-stutter.
   - In `OptiScaler_RE-test13.log`, the frame render time fed to Intel was `0.0f` (`Opti FT: 0, Source FT: 0, Set FT: 0`).

---

## 3. Issue Validity Assessment & Root Cause Analysis

### 3.1 Defect 1: RE Engine Window Positioning Conflict
- **Root Cause**: Capcom RE Engine coordinates display mode transitions internally through its own engine window manager. Forcing the DXGI swapchain to borderless mode via quirk bypassed RE Engine's internal window positioning calculations, leaving the window stuck at offset `(0, 0)`.
- **Resolution**: Revert `ForceBorderlessWhenUsingXeFG` and `OverrideVsyncWhenUsingXeFG` for `re9.exe` and `re9demo.exe`.

### 3.2 Defect 2: Intel `libxess_fg.dll` Internal Worker Thread Presentation Clumping
As reverse-engineered by Coldwood1026 (`Coldwood1026/OptiScalerDp4aUnlock`), generated frames above 2X are **not** presented through DXGI `Present()` in OptiScaler, but by `libxess_fg.dll`'s internal presentation worker thread:
1. **Skipped Scheduler for Intermediate Frames**:
   - Inside `libxess_fg.dll`, the burst present loop at `0x220280..0x22030A` presents frames `1..count-1` directly to the swapchain back-to-back without calling its internal scheduler (`0x21EE30`) at all.
2. **Corrupted Stack Base for the Final Generated Frame**:
   - The provider calls the scheduler (`0x21EE30`) only for the final generated frame (`index == count`).
   - At `0x224B30`, `base` is only written when `index == 1`. For `index > 1`, `base` is uninitialized stack memory.
   - (2X FG was the only mode that worked natively because at 2X, `count == 1` and `index == 1`).
3. **Overzealous Clamping**:
   - `0x224B30` clamps the presentation interval to an internal float `f` at `ring+0x1B8` (~8.5 ms), causing frames to clump together even when the real frame duration is 25–35 ms.
4. **Self-Referential Frame Time Feedback Loop**:
   - Feeding back the whole present-to-present duration closes a feedback loop (`period = renderTime * (count + 1)`), causing render times to balloon and freeze when bursts stall.
   - In `OptiScaler_RE-test13.log`, `_ftDelta[fIndex]` was unpopulated (`0.0f`), so Intel was told `frameRenderTime = 0.0`.

### 3.3 Defect 3: HWND Swapchain Scaling Incompatibility
- As fixed by janblade (`janblade/OptiScaler-F5-DLSSNR-Multipass`), `DXGI_MODE_SCALING_CENTERED` was mapped to `DXGI_SCALING_ASPECT_RATIO_STRETCH`, which DXGI rejects on HWND swapchains with `DXGI_ERROR_INVALID_CALL`.

---

## 4. Technical Solutions Applied

### 4.1 RE9 Quirks Revert & Centering Fix
1. In `OptiScaler/misc/Quirks.h`:
   - Removed `ForceBorderlessWhenUsingXeFG` and `OverrideVsyncWhenUsingXeFG` from `re9.exe` and `re9demo.exe`.
2. In `OptiScaler/hooks/FG_Hooks.cpp`:
   - Reverted the `OverrideVsync` check from the DXGI `XeMfgExtraPacing` block.
3. In `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`:
   - Mapped `DXGI_MODE_SCALING_CENTERED` to `DXGI_SCALING_STRETCH` for HWND swapchain compatibility.

### 4.2 Native XeMFG Thunk Pacing Engine (`XeFGPacing.h`)
Integrated `OptiScaler/framegen/xefg/XeFGPacing.h`, porting Coldwood1026's thunk detour engine:
1. **Three 16-byte Thunk Detours in `libxess_fg.dll`**:
   - **`PresentThunkRva = 0x25C0` -> `0x21F730`**: Intercepts the loop present caller (`0x2202ED`) and final burst present caller (`0x220467`). Intermediate frames `1..count-1` are routed through `ScheduleFrame()`.
   - **`SchedThunkRva = 0x3100` -> `0x21EE30`**: Directs intermediate frames into Intel's internal scheduler with their actual frame index.
   - **`TimestampThunkRva = 0x3430` -> `0x224B30`**: Repairs presentation deadlines in `TsDetour`:
     - Re-anchors `base` from the first frame's timestamp.
     - Adds back interval reductions clamped away by `min()`.
     - Advances each subsequent frame by `median / (count + 1)`.
2. **High-Precision Wall-Clock Pacing Fallback**:
   - When the scheduler is disabled by Intel's limiter (`ctx[0x340] != 0`), `PaceFrame` engages a calibrated wall-clock pacer (`WaitUntil` using `QueryPerformanceCounter` with `Sleep(0)` and `YieldProcessor()`).
3. **Decoupled Frame Render Time (`RenderTimeMs()`)**:
   - In `XeFG_Dx12::Dispatch()`, retrieves `XeFGPacing::RenderTimeMs()` (which subtracts the burst blocking duration `burstBlockQpc` from the measured period), falling back to `_ftDelta` and `lastFGFrameTime`.
   - Records the fed frame time via `XeFGPacing::NoteFedFrameTime()`.
4. **Lifecycle & Transactional Rollback**:
   - `XeFGPacing::Install(base)` checks all 16 expected bytes before hooking.
   - `XeFGPacing::Uninstall()` restores the pristine thunk bytes on module shutdown.
   - `XeMfgLoader::ApplyToMemory()` sets `PacingDetours = 3`, `PacingInstalled = true`, `VerifiedPacing = true`.

---

## 5. Verification & Test Results

### 5.1 Unit Tests (40/40 Passed)
- `tests/xefg_swapchain_lifecycle_unit`: 5/5 passed (RE9 fullscreen preservation, centered scaling mapping).
- `tests/xemfg_loader_unit`: 9/9 passed (Test 9: 3 thunks detour verification, 4X burst deadline calculation repair, pristine rollback).
- `tests/xefg_ring_buffer_pacing_unit`: 8/8 passed.
- `tests/xefg_save_load_stability_unit`: 5/5 passed.
- `tests/rtss_xefg_pacing_immunity_unit`: 3/3 passed.
- `tests/xemfg_menu_exclusion_unit`: 3/3 passed.
- `tests/xefg_mfg_streamline_unit`: 7/7 passed.

### 5.2 Formatting & Encodings
- `clang-format --dry-run --Werror`: 0 violations across all modified and new files.
- UTF-8 BOM (`\xef\xbb\xbf`): Verified intact on `XeFGPacing.h`, `XeMfgLoader.cpp`, `XeFG_Dx12.cpp`, `Quirks.h`, `FG_Hooks.cpp`, `OptiScaler.vcxproj`, and `OptiScaler.vcxproj.filters`.
