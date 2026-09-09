# Open issue review — 10 September 2026

Reviewed all ten open issues and their comments against `v0.7.5-nr-fixes` (`73f26daa`). Downloaded and read the Nioh 2 log, the Stellar Blade log archive, the updated standalone source contribution and the MFG implementation linked in #12. Reporter attachments were inspected as data/source, not executed.

| Issue | Finding and disposition |
| --- | --- |
| [#3](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/3) Aphelion HDR/purple halos | Reporter explicitly confirms v0.7.3 completely fixed it. Closed as resolved. |
| [#5](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/5) RR/upstream/async suggestions | Before/after RR placement and common controls shipped in v0.7.4. Kept open for a concrete remaining comparison/reproduction; no unsupported claim that another fork is universally better or that arbitrary async processing is safe. |
| [#6](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/6) Standalone NR contribution | Reviewed the September 8 source update. Kept as an enhancement requiring integration, rather than replacing newer production files with its v0.6.2-era versions. See integration findings below. |
| [#7](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/7) Requiem/Onimusha/Cyberpunk NR inactive | No reporter logs identify the failed stage. Hardened NGX routing and added diagnostics; requested per-game startup/reproduction logs, INI and placement details. Kept open. |
| [#9](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/9) Deferred flashing | Per-evaluate seam fix shipped in v0.7.5 through #11. Existing seam regression passes. Closed the dropout/flash report; separate fine-detail shimmer is not claimed fixed. |
| [#10](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/10) RR flicker | v0.7.3 guide fix predates current release; the older reports do not establish which problem remains. Added RR/deferred placement explanation and routing hardening. Requested a repeatable scene/settings and current evidence; kept open pending visual reproduction. |
| [#12](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/12) RTX 40 MFG plus NR | Log shows the built-in patch applied but does not prove correct MFG output. Two users report external/alternate unlockers work. Provided the existing external-FG mode and requested paired logs if needed. No RTX 40 hardware available here; kept open. |
| [#14](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/14) Before-RR option | Already implemented in v0.7.4 and present in v0.7.5. Closed the feature request, with failures tracked in #7/#10. Fixed the additional deferred-mode checkbox conflict discovered during review. |
| [#15](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/15) Reflex 2 | Kept as an enhancement. DLL availability does not supply a general late-camera-pose integration. Requested source/interface documentation; no demo binaries incorporated. |
| [#16](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/16) Nioh 2 crash | Identified a foreign-API optional-resource pointer escaping into DX12 NR. Fixed DX11 and Vulkan bridge handoffs. The crash log ends at the first exposure-processing stage with auto-exposure enabled, consistent with this defect. Kept open for reporter validation. |

## Changes

- Optional exposure/reactive resources are always overwritten with their DX12 resource or null before the bridge invokes a DX12 consumer. Auto-exposure and disabled-reactive paths previously retained the game's DX11/Vulkan pointer. DX11 restores the original optional values, including null, afterward.
- The NGX handle registry uses a checked lookup, synchronizes its own reads/writes, records only successful creations and forgets successful releases. Unknown native handles and known non-upscaler features skip both NR seams while retaining the original NGX evaluation.
- Live OptiScaler backend information supplements creation metadata for RR classification. A DLSSD backend excludes the SR-only deferred experiment even when the original record says SR. Debug logs include handle, creation feature ID, upscaler classification and RR classification.
- A stored deferred-mode setting no longer disables the ordinary before/after placement control while the live backend is RR. The overlay and runtime status explain that RR uses ordinary placement.

These changes do not establish the cause of every reported crash, flicker or lack of effect. They do not transform RR's auxiliary lighting/material inputs alongside colour.

## Standalone contribution integration findings

The newer attachment includes GPU-fence transport, settings tests and native-input handoff; the original performance/multipass limitations cannot simply be repeated as if the update did not exist. However:

1. Its replacement `DlssNr_Dx12.cpp` still contains the old forced-post RR/`ApplyAfterRR` implementation. Copying it wholesale would undo the current unified before-RR path and newer synchronization changes.
2. Current `EvaluateInternal` holds `g_nrMutex`. Adding the contribution's `PreferNativeInputs` call at the old location would acquire `ownerMutex` while holding `g_nrMutex`, whereas standalone Present holds `ownerMutex` while dispatch acquires `g_nrMutex`. Integration must avoid this lock-order inversion.
3. On a failed GPU wait in the contribution's `StandaloneNR` destructor, the catch releases ownership of `impl` but returns without clearing global `owner`. A subsequent native handoff dereferences `owner->impl`, although that owner object has been destroyed. Quarantine the failed standalone path and clear the owner before accepting a later handoff.
4. Dummy depth/zero motion and processing the final HUD are inherent limitations of this contribution's generic path. Its supplied test claims are not new verification on current main.

The contribution remains open for a rebased integration with native handoff/timeout/resize tests. No standalone code is included in this patch.

## Validation

- Release x64 build passed. Existing C4744 and LNK4098 link warnings remain.
- `tests/nr_ngx_routing_smoke.cpp`: missing/reused/released handles, removal of stale FG presence, concurrent registry reads/writes, and all eight combinations of optional-resource availability/auto-exposure/reactive disable passed. This is a CPU parameter-routing regression, not an NVIDIA model test.
- `tests/nr_seam_clock_smoke.cpp`: pairing, Present-counter stalls/changes, gaps, resets and bridge epochs passed.
- `git diff --check` passed.

Cyberpunk is installed locally, but the existing local log is from v0.7.3 and does not show RR. No fresh gameplay or visual-quality validation was performed in this session. Nioh 2, Requiem, Onimusha, Stellar Blade, Aphelion, NBA 2K26, Dawnwalker and Crimson Desert were not found in the inspected Steam libraries. The available GPU is an RTX 5090, so RTX 40 MFG results need reporter testing.
