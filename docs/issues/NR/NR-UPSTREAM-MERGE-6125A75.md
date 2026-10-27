# Neural Rendering: Selected Merge of Upstream Improvements (commit 6125a75)

## Overview & Environment
- **Operating System**: CachyOS Linux (Kernel `7.3.0-4-cachyos`)
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86)
- **Driver**: NVIDIA 580.178.04 (re-verified 2026-10-27 with `uname -r`, `/etc/os-release`, `nvidia-smi`)
- **Upstream commit**: `6125a75b` (wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
- **Merge branch**: `merge-upstream`

## Reported Issue / Goal
Upstream introduced several DLSS-NR improvements. This fork selectively merged low-risk, additive changes while preserving fork invariants (Pre-SR defaults, format-bouncing cache, presets, multi-mip guides, contextual driver resolution, buffer creation).

## Merged (Low-Risk) Improvements
1. **Teardown/shutdown fixes** + GPU lifetime tracking, proxy, capture, finished-picture bridge, StreamlinePicture.
2. **Vulkan NR support** (Vk feature, finished picture, image/parameter helpers) — additive.
3. **Peripheral spatial compression** — shader blobs only (invariant .cpp/.h skipped).
4. **Edge/history/halo filters** — shader blobs, the Edge-aware Enlargement mode, config keys and menu controls,
   and the Vulkan C++ for halo suppression and MatchGuides/RenderMotionScale. The DX12 C++ was **not** merged at
   first; see "PR #30 Review Findings & Fixes".
5. **Finished-picture routing (KCD2)** — FinishedCompose.cpp.
6. **Exposure/pipeline improvements** — Exposure.h, Pipeline_Dx12/Vk (with fork dimension fallback preserved).
7. **Denoise-first** — shader blob only (invariant .cpp/.h skipped).

## Skipped (Medium/High-Risk) Items
- Model-rebuild consolidation (touches format-bouncing cache).
- Exposure/sharpness/colour .cpp/.h changes (menu + config).
- Denoise-first .cpp/.h changes (may affect Pre-SR).
- Guide/white-point/jitter .cpp/.h changes (touches contextual resolution).

## Verification
- clang-format --dry-run --Werror: PASS on all modified C++ files.
- UTF-8 BOM: restored on all modified C++ files.
- Unit tests: not run (Windows-only build); CI to validate.

## Post-Merge Build Fix (CI run 40374490346 → 40377055698)
The first pushed merge (`aedff167`) failed MSVC compilation because merge commit `8ef1d025` had replaced several fork-hardened NR sources with their upstream variants, while dependent fork sources still used the fork transactional API. Five error clusters were reported:

1. `DlssNr::BeginFinishedPictureSubmission` missing (`ResTrack_dx12.cpp`, `DlssNr_Dx12.cpp`).
2. `DlssNr::RequestHistoryReset` missing (`menu_common.cpp`, `DlssNr_Menu.cpp`).
3. `GpuLifetime::BeginSubmission` / `QuarantineSubmission` / `GpuComplete` missing (`DlssNr_PrivateCreation.h`, `DlssNr_GpuTime.h`).
4. `DenoiseFirstContext::denoiseTime` / `enlargeTime` missing (`DlssNr_Dx12_FinishedQueue.cpp`).
5. `DlssNr_Vk::Dispatch` 15-argument overload missing (`DlssNr_Vk.cpp`, `DlssNrFeature_Vk.cpp`).

Validity assessment: genuine regression (header/source contract mismatch), not a user configuration issue. Fixes:
- Restored `DlssNr_GpuLifetime.h/.cpp`, `DlssNrFeature_Dx12.h`, `DlssNr_Dx12_FinishedQueue.cpp`, `DlssNr_Proxy.h/.cpp` and `DlssNr_Capture.h` from `main`.
- Declared `RequestHistoryReset()` in `DlssNr_Status.h`.
- Added the `InModelLayout` parameter to `DlssNr_Vk::Dispatch` in `DlssNr_Vk.h` for the merged Vulkan callers.

Result: Build (No Signing) run `40377055698` on `9799816a` completed successfully.

## PR #30 Review Findings & Fixes
The review of PR #30 (REQUEST CHANGES) found that the selective merge had left several features half-wired.
Each fix ports only the matching upstream 6125a75 hunks, never whole files: upstream's `DlssNr_Dx12_Models.cpp`,
`DlssNr_Dx12_ModelState.h`, `DlssNr_Dx12_DeferredSr.cpp` and `DlssNr_Dx12_State.h` would also remove the
FF7 `altSurfaces` cache, the contextual post-SR driver error and the fork's private-creation lifetime contracts.

| # | Finding | Validity | Fix | Commit | Test |
|---|---|---|---|---|---|
| 1 | Passes 3..10 lost the NR model preset combo (`selectedPass <= 2`); configured `Pass{N}Preset` shown as inherited | Genuine regression of `NR-LIGHTWEIGHT-MODEL-PRESET-SUPPORT.md` | Combo for every pass, upstream labels via `DlssNr_MenuPresets.h`; INI wording fixed (`Preset=0` is Automatic, only `auto` inherits) | `ab4d022d` | `nr_model_preset_menu_unit` |
| 2 | "Reject stale history" did nothing on DX12 (`accum {}` left `HistoryRejection = 0`) | Genuine: menu/config/shader merged, DX12 C++ not | `DlssNr_ResidualHistory.h` fills rejection, pre-exposure and its ratio, and drops history when the toggle flips; NaN/0 pre-exposure falls back to 1 | `3b2fd05b` | `nr_history_rejection_unit` |
| 3 | Halo suppression sliders did nothing on DX12 | Genuine: no DX12 dispatch, no quality constants in Encode | Halo dispatch into `passClamp` after the model chain; `passClamp` allocated whenever the filter is on, after the format swap, without rebuilding models | `2f93b39d` | `nr_halo_dx12_unit` |
| 4 | MatchGuides/RenderMotionScale were Vulkan-only, so the APIs scaled motion vectors differently below native | Genuine: DX12 C++ not merged | DX12 guide resample (mode 10); shared `DlssNrModelMotionScale` for both APIs; `depthSmall`/`motionSmall` stay out of `altSurfaces` | `be1bef7c` | `nr_motion_scale_unit` |
| 5 | 6 new `[DlssNr]` keys and `Transfer=5` undocumented in `OptiScaler.ini` | Genuine | Documented with defaults | `3d353c40` | `nr_ini_keys_unit` |

Also confirmed during the review: the merged shader blobs are byte-identical to upstream 6125a75 apart from the
UTF-8 BOM, and match their `.hlsl` sources.

### Verification
- clang-format 23.1.4 (local) and 23.1.3 (CI, PyPI): clean on all modified C++ files; UTF-8 BOM present.
- Linux `g++ -std=c++20`, all passing: `nr_placement_config_unit`, `nr_pipeline_setup_unit`,
  `nr_buffer_resource_unit`, `nr_status_reporting_unit`, `nr_format_bouncing_unit`, `nr_model_preset_unit`,
  `nr_model_preset_menu_unit`, `nr_history_rejection_unit`, `nr_halo_dx12_unit`, `nr_motion_scale_unit`,
  `nr_ini_keys_unit`, `dlssg_sm86_ini_smoke`, `dlssg_sm86_v030_ini_unit`, `turing_menu_recognition_unit`,
  `streamline_turing_spoof_unit`, `turing_dual_runtime_unit`. Each new test was also run against a deliberately
  broken copy of the source to confirm it fails.
- Not verifiable here: the MSVC build (CI after push), the Windows-only shader smoke tests, and the visual effect
  of each control. **Pending in-game DX12 check**: WorkingScale < 100% (no flicker, correct motion), halo
  sliders, and "Reject stale history" with RR.

## Notes
- `.agents/rules/optiscaler-architecture.md` invariants preserved.
- Medium/high-risk items remain for future file-by-file review.
- `MatchGuides` and `RenderMotionScale` default to true, so DX12 below 100% WorkingScale now behaves like upstream
  and Vulkan. At 100% the motion scale is unchanged for games whose motion region matches the size the vectors
  are measured in.
