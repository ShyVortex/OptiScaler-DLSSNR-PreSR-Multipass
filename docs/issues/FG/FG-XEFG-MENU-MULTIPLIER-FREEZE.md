# Frame Generation: XeFG Freeze When Changing the Multiplier From the Overlay Menu

## 1. Overview & Environment

- **Report**: DLSS Unlocked issue #57 (https://github.com/ShyVortex/dlss-unlocked/issues/57).
- **Reporter environment**: Windows 11, NVIDIA GeForce RTX 2060 (Mobile, Turing), driver 617.42.
  - Silent Hill Townfall (Unreal Engine, quirk `DisableXeFGChecks` + `AlwaysCaptureFSRFGSwapchain`).
  - 1920x1080, DLSS SR at 1920x1080.
- **Configuration**:
  - `[XeMFG] UnlockMFG=true`, `Source=OptiScaler`, `MaxInterpolatedFrames=3`.
  - This gives `FGInput::Upscaler` → `FGOutput::XeFG`, with `FGUseMutexForSwapchain` on.
  - `[InitFlags] DisplayResolution=false`. This was the earlier fix in the same thread for XeFG rejecting
    mismatched motion-vector/depth sizes.
- **Provider**: `libxess_fg.dll` 1.3.1.78, all 5 XeMFG patches applied, pacing detours installed.
- **Analysis environment** (logs only, no repro hardware): CachyOS rolling, kernel 7.3.0-4-cachyos, NVIDIA GeForce
  RTX 3080 Ti Laptop GPU, driver 580.178.04.

## 2. Reported Symptoms
After XeFG started working, "XeFG works now but crashes (screen freezes) while changing multiplier in-game through
OptiScaler". A v0.9.73 test build containing f904ab52 ("serialize SetInterpolatedFrameCount with render thread")
behaved the same: "No change, issue persists".

## 3. Issue Validity Assessment
This is a genuine OptiScaler defect, not misconfiguration. Both logs end in the overlay's multiplier handler, inside
a Present call chain.

**v0.9.72 (`80948410`), last lines:**
```text
[D] MenuOverlayDx::Present
[T] RenderImGui_DX12
[D] OptiInput::LogInputHealthSnapshotLocked EndFrame health ... menu:yes ...
[I] XeFG_Dx12::SetInterpolatedFrameCount XeFG SetInterpolatedFrameCount called with count: 5
```
The process hangs in the Intel calls (`SetEnabled`, `SetNumInterpolatedFrames`) that follow the log line.

**v0.9.73 (`f904ab5`), last lines:**
```text
[T] FGHooks::FGPresent Accuired FG->Mutex: 2
...
[D] FGHooks::hkFGPresent1 XeFG call skipping
[D] LocalPresent 1938
[D] MenuOverlayDx::Present
[T] RenderImGui_DX12
```
No `SetInterpolatedFrameCount` line appears: the hang moved in front of it.

## 4. Root Cause
- `FGHooks::FGPresent` takes `fg->Mutex` (owner 2) and calls `o_FGSCPresent`, which is Intel's proxy swapchain.
- Intel presents the real and generated frames through the real swapchain. That call reaches
  `wrapped_swapchain.cpp` `LocalPresent`, which draws the overlay (`MenuOverlayDx::Present` → `RenderImGui_DX12`).
- The `Max Interpolated Frames##xemfg` slider, and the two XeMFG enable paths in `menu_common.cpp`, called
  `state.currentFG->SetInterpolatedFrameCount()` directly, from inside Intel's Present.
- **v0.9.72:** `SetInterpolatedFrameCount` called `XeFGProxy::SetEnabled()` / `SetNumInterpolatedFrames()`, re-entering
  Intel's runtime from inside its own Present. That hangs.
- **v0.9.73:** f904ab52 added `OwnedLockGuard lock(Mutex, 560)` before the log line. `OwnedMutex` wraps a
  non-recursive `std::shared_mutex`, and the menu's own call chain already holds it through `FGPresent`. So the lock
  can never be acquired: a guaranteed deadlock instead of a likely one.

## 5. Prior-Attempt Audit
- f904ab52 is the failed approach (lock `SetInterpolatedFrameCount`). It is reverted, not extended.
- `FG-XEMFG-SR-FULLSCREEN-AND-MULTIPLIER.md` requires multiplier changes to stay live, without `fgChanged`,
  `Deactivate()` or the 10-frame pause.
- Architecture rule 8 requires `XeMfgLoader::SetMaxGeneratedFrames` to stay a live memory patch.
- Both constraints are kept.

## 6. Solution Applied (`2656da0f`)
- `framegen/IFGFeature.h`: new virtual `RequestInterpolatedFrameCount(UINT)`. The default forwards to
  `SetInterpolatedFrameCount`, so DLSSG and FSR-FG are unchanged.
- `framegen/xefg/XeFG_Dx12.h/.cpp`:
  - The override stores the request in `std::atomic<int> _pendingInterpolationCount` (-1 = none) and touches nothing
    in Intel's runtime.
  - `XeFG_Dx12::Present()` exchanges it at the top and calls `SetInterpolatedFrameCount`. This runs on the game's
    present thread, under `fg->Mutex`, before the frame reaches Intel: the same place where `Dispatch`'s config
    sync already calls `SetNumInterpolatedFrames` safely. The latest request wins, and 0 (passthrough) stays
    distinct from "none".
  - f904ab52's lock and include are reverted.
- `menu/menu_common.cpp`: the three XeMFG call sites use `RequestInterpolatedFrameCount`. `SetMaxGeneratedFrames` stays
  live in the slider.
- `hooks/Streamline_Hooks.cpp` is unchanged. `slDLSSGSetOptions` runs on the game thread and needs the synchronous
  result.
- If FG is inactive, `Present` is not reached and the request waits. The config already carries the value for
  swapchain recreation (`CreateSwapchain` syncs `FGXeFGInterpolationCount`).

## 7. Verification
- `tests/xefg_menu_multiplier_queue_unit.cpp` (7/7 PASS) checks:
  - the menu queues at all three sites;
  - `SetInterpolatedFrameCount` takes no mutex;
  - the request does not touch Intel's runtime;
  - `Present` applies the request before passthrough and dispatch;
  - the IFGFeature default forwards;
  - the queue semantics;
  - the Streamline path stays synchronous.

  It fails against the pre-fix sources.
- Existing suites pass: `xemfg_loader`, `xemfg_menu_exclusion`, `xemfg_multiplier_and_burst_pacing`,
  `xemfg_sr_parity`, `xemfg_sr`, `xemfg_sr_crash_fix`, `xefg_mfg_streamline`, `xefg_ring_buffer_pacing`,
  `xefg_hardening_diagnostics`, `xefg_diagnostic_ring`, `xefg_main_pacing_alignment`, `xefg_pacing_engine`,
  `xefg_swapchain_lifecycle`, `xefg_save_load_stability`.
- clang-format 23 clean; UTF-8 BOM present. `IFGFeature.h` was missing its BOM and now has one.
- **In game:** confirmed by the reporter on DLSS Unlocked #57 ("Works fine") with the CI build of `61f3d89f` (run
  40503225385) in Silent Hill Townfall: changing the multiplier from the OptiScaler menu no longer freezes the game.
- The reporter also noted shimmering in DLSS Quality mode that mostly disappears at Native. That is a separate,
  unconfirmed observation, not covered by this fix.
