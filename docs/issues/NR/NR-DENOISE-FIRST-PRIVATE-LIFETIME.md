# Denoise-first integration onto v0.9.34

Base: ShyVortex `73fab132f9f48d194926b27124d9320b0b4cc870`.
Feature input: wilsjo2 PR123 `8fbdfa60df631c1f41bd39cf6afcbd96d76f1979`,
diffed against `f45ccf3a761df91450959d325ac0166cba5364c9`.
This is a selective feature port, not replacement with PR123's older base branch.
Existing descriptor leases, pre-Execute submission capture, resource ownership,
low-latency synchronization and bounded menu sizing are retained.

## Feature

Opt-in native D3D12 route: private 1:1 game RR (when usable RR guides exist),
otherwise DLSS SR; NR edits the clean image; default step 2 resamples that edit
onto the original jittered render before the game upscales it. This adds no model
or NR runtime DLL. Steps 0/1 are retained as experimental alternate routes.
The default is disabled; ordinary and finished-picture NR remain available.
Display-resolution MV and output-scaling requests retain the ordinary NR route.

## Correctness changes

1. Private creation waits for the exact recorded creation's successful GPU
   completion, not a changed logical frame epoch. Each private feature owns a
   single-shot `PrivateFeatureCreation`, using the production lifetime tracker
   and immutable recording probe. Reset, address reuse, delayed notification,
   failed Signal and device removal cannot prove readiness. The same issue in
   the existing DeferredSr context is fixed. Discard detection collects before
   consulting the probe, avoiding a false-discard race as a fence completes.
2. The final game's failed upscale invalidates private denoiser history for all
   three steps, including the default non-replacement route. Handed-off resources
   are restored on the matched After path before failure handling.
3. Explicit Retry retires both private contexts behind their GPU ownership,
   clears pending pairing and permits new allocation. It is not a per-frame retry.
4. The shared private adapter validates actual depth/MV Texture2D descriptors,
   single-sampling and active extents before barriers or runtime evaluation.
   The motion extent follows creation's low-resolution/display-resolution flag.
   Larger backing textures remain accepted; no additional format restriction.
5. New timers belong to each private generation rather than being destroyed
   immediately on device change. Current and retired generations receive exact
   creation/timer submission, reset and quarantine notifications. Child tokens
   complete before the resource-owner token; the top-level owner completes last.
6. Ordinary NR exclusion depends on accepted/consumed private work rather than
   a configuration checkbox. Unsupported, warming and failed private creation
   preserve the ordinary placement before its intermediate target is allocated.
   A late failure after NR evaluation must not advance the same model history
   a second time in that frame. Composition failure parks the private context;
   ordinary NR resumes on subsequent frames with reset history until explicit Retry.
   Consumption is propagated from the actual model evaluation, not inferred from
   successful final composition, which can fail after temporal history advanced.
   Private NR owns its model-initialization/warmup frame as well: falling back to
   ordinary post-SR at that point would rebuild the shared NR cache to a different
   placement (and often size), destroying the new feature before it becomes ready.
   A transient raw frame during this later warmup avoids a reproduced livelock.
   Early private-denoiser warmup and unsupported admission still retain ordinary NR.
7. A mismatched or abandoned handoff retires its generation instead of issuing
   a barrier on a different recording or reusing a texture with an unknown state.
   The destructor uses the complete readiness predicate, including private owners,
   rather than trusting only the top-level resource fence.
8. Lazy composite/private-upscale textures are committed directly in their readable
   resting state. Discarding the first-use recording cannot discard an initial
   state transition and leave the next frame's before-state incorrect.
9. Switching actual game-input routes temporarily requests game upscaler history
   reset. Stable routes do not repeatedly reset. The previous route is committed
   only after successful evaluation; Reset and jitter overrides are restored.
10. Private output replacement uses the state of its actual target. Pipeline
    intermediates are UAVs, independent of a configured final-game-output state.

## Validation

CPU tests execute production code or mechanically extracted production method
bodies, with only GPU/COM/runtime boundaries substituted. This is not evidence
of driver/GPU stability, visual quality or RTX4090 performance.

- `tests/nr_private_creation/run.ps1`: exact creation, production Deferred gate,
  retirement, callback ownership, failed submission and allocation regressions.
- `tests/nr_private_seam/run.ps1`: actual After/Retry/retirement, routing, failed
  composition/enlargement, same-frame history ownership and recovery.
- `tests/run_nr_private_guide_validation.ps1`: actual adapter Init/Evaluate
  prelude over accepted and rejected resource descriptors.
- `tests/nr_private_initial_state/run.ps1`: actual committed resource creation and
  lazy allocation with discarded initial command recordings.
- `tests/nr_game_history/run.ps1`: actual game evaluate scope, route transitions,
  failed evaluation retries and Reset/jitter restoration.
- `tests/nr_history_warmup/run.ps1`: actual model preparation, invocation and routing
  over multiple epochs, including equal-size placement-only rebuild livelock.
- `tests/run-nr-cpu-audits.ps1`: existing seven ownership, timing, descriptor,
  threading, allocation and encode-failure audit groups. Includes a deliberately
  failing post-only-notification negative control.
- Existing low-latency, ultrawide menu and FG/MFG CPU boundary regressions.

Pinned original-source RED evidence and corrected GREEN evidence are retained
outside version control in the task's dated output directory. Release builds
disable post-build packaging/deployment events. Local Ada tests retain the external
MFG loader and compile the built-in Blackwell-kernel unlock out; public build
defaults and other architectures are unchanged.

Source-only Flash and Sonnet reviews identified additional issues above. Reviewed
suggestions are not assumed true: backing-size equality was not established as an
NGX requirement (active subrect governs reads), and neither bypassing game evaluate
nor resetting its history continuously is used. The temporary transition-only reset
addresses input changes without destroying temporal accumulation on stable frames.
Optional zero-jitter steps 0/1 with jittered MV remain visually/GPU-unvalidated;
local initial tests use default step 2, which preserves the game's jitter contract.

Known pre-existing `mfg_options` failures: unsupported Dynamic request must
preserve accepted native mode/pacing and remain pending after native On succeeds.
They are outside this NR feature port and inactive in the external-MFG setup;
the suite is not represented as wholly passing.

Reference for active subrect and dynamic-resolution contracts:
[NVIDIA DLSS programming guide](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf).

## Deployment boundary

Only the already-installed OptiScaler proxy DLLs in Cyberpunk, Control Resonant
and Townfall are replaced. Existing NR runtimes, external MFG loaders, game mods
and Streamline files are not replaced. Townfall's SF-v2 runtime is preserved by
exact hash. No games, GPU smoke tests or continuous telemetry are launched.
