# Frame Generation: sdli1995 dlssg_for_sm86 v0.4.1 Integration

## 1. Overview & Environment
- **Trigger**: sdli1995 released `dlssg_for_sm86` 0.4.1 (tag `0.4.1`, 2026-10-27).
  - The release files were placed in `dlssg_for_sm86/sdli1995/`.
  - All 28 files match the release `manifest.json` SHA-256 hashes.
- **Work branch**: `dlssg-sm86`, per the permanent-branch workflow (Case A: mod update), then merged into `main`.
- **Analysis environment**: CachyOS rolling, kernel 7.3.0-4-cachyos, NVIDIA GeForce RTX 3080 Ti Laptop GPU, driver
  580.178.04.
- **0.4.0 baseline**: the 0.4.0 modular install in the Red Dead Redemption 2 folder (root proxy 853,280 bytes). The
  0.4.0 tag tree has the same sizes.

## 2. What Changed in 0.4.1 (Relevant to OptiScaler)
From the release changelog, the factory INI and `docs/INSTALL.en.md`:
- **XeSS-FG engine:** 2X/3X/4X (model 15 port), `[XeSSFG] Model` removed.
- **Super resolution:** `[SuperResolution] Mode` presets; `Mode=On` is invalid.
- **`[DLSSG] Preset`:** A/B/C only; `Auto` runs as A with a warning.
- **Removed keys, silently ignored:**
  - `[Compatibility]`: `HardwareBilinear`, `SM75Family`, `AccelerateNewerGpus` and others;
  - `[Runtime] CacheDirectory`;
  - `[Backends]`.
- **Old locations** `[FrameGeneration] Optimized` and `[Compatibility] Preset`: a warning when written beside
  `[DLSSG]`. Written **alone** with a non-default value, they are `configuration_invalid`.
- **Strict values:** an invalid value in a frame-generation section is `configuration_invalid`, and **frame
  generation is not enabled**.
- **Hard R580 driver floor:** a refused cubin is `driver_below_floor` / `kernel_image_unavailable`, with no PTX
  fallback.
- **Log:** a single `dlssg_sm86\logs\dlssg_sm86.log`.
- **Layout:** unchanged. A root `version.dll`, plus `dlssg_sm86/{backend,kernels,runtime,sr,xefg}`.

## 3. Validity Assessment
The production code was run against both versions, laid out as packaged (`dlssg_sm86.dll` +
`dlssg_sm86/{backend,kernels,runtime}`).

**Companion INI (`FormatIniContent030`): compatible, no change needed.**

| Key OptiScaler writes | 0.4.1 behaviour |
|---|---|
| `[General] Enabled=1`, `[FrameGeneration] Engine=DLSS`, `MaxGeneratedFrames` 1..5 | valid |
| `[FrameGeneration] Optimized` (old place) | warning only, because `[DLSSG] Optimized` is written too |
| `[DLSSG] Optimized` 0..3 | valid |
| `[DLSSG] Preset=Auto` (OptiScaler default) | warning, runs as A |
| `[Compatibility] Preset` (old place) | warning only, because `[DLSSG] Preset` is written too |
| `[Compatibility] Router` / `KernelImage` / `SpoofArchToGame` | valid |
| `[Compatibility] HardwareBilinear`, `[Runtime] CacheDirectory` | removed, ignored |
| `[Logging] Level` / `Directory`, `[Runtime] Mode=Bundled` | valid |

The old-place keys stay, because architecture rule §4 requires the dual 0.3.x / 0.4.x INI (SilyNoMeta 0.3.5 reads
them).

**Detection, before the fix:**

| Binary | Variant | Name | SM75 | 310.1 | DMFG |
|---|---|---|---|---|---|
| 0.4.0 (RDR2) | sdli1995 | `sdli1995 v0.4.0` | yes | no | no |
| 0.4.1 | sdli1995 | `sdli1995 v0.4.0` (**wrong**) | **no** (wrong) | no | no |
| SilyNoMeta (TLOU Part I) | SilyNoMeta | `SilyNoMeta v0.3.5-5` | yes | no | yes |

**Defects:**
1. The name was hard-coded for every modular sdli1995 build. SilyNoMeta names came from a fixed list of
   `Control engine v0.3.5[-5]` needles, so a local v0.3.5-4 build showed as `v0.3.5`.
2. `HasSm75KernelFamily` scanned only the proxy. 0.4.1 dropped the `sm75_family` string together with the
   `SM75Family` key, but `backend/sm86_backend.dll` still has `cubin_sm75` / `executed_on_sm75_hardware`, and
   `fg_kernels.dll` has 256 `sm_75` entries.
3. Release packaging (`package_release.ps1`, Package Release workflow) was pinned to `tags/0.4.0`.
4. Users got no explanation for the hard R580 floor, and `HardwareBilinear` is a no-op on 0.4.1.

## 4. Solution Applied (`dlssg-sm86`)
- **`b6b2129a`: version read from the binary, never hard-coded.**
  - sdli1995 0.4.1+ states it in its log-session metadata (`version\0` + `0.4.1`).
  - SilyNoMeta states it after `Control engine v`, in ASCII or UTF-16LE, parsed generically.
  - Without a stated version the name is just the family: `sdli1995` / `SilyNoMeta`.
  - Classification is unchanged; checked on all local binaries.
- **`8522d526`: SM75 detected in the modular backend when the proxy has no marker.**
  - A proxy that says "no SM75" still wins.
  - `ResolveRouter` takes the runtime: Turing + Auto gives SM75 only on 310.1. On 310.9 (all 0.4.x) it stays Auto,
    and the mod picks SM75 itself, per architecture rule §4.
  - This also moves 0.4.0 Turing from `SM75` to `Auto`. An explicit `SM75` is honoured when the kernels exist.
  - The menu router label uses the real status ("Auto (chosen by the mod)").
- **`1e8cdd5f`: packaging pinned to `tags/0.4.1`.**
  - The tag's git tree has the root `version.dll` (815,240 bytes) and the backend / kernels / runtime modules, with
    sizes matching the manifest.
  - `sr/` (release assets only) and `xefg/` stay pruned.
- **`919b4477`: menu notes.**
  - An amber R580 note for a known Windows driver below 580.00 (not under Wine, not for an unknown version).
  - `HardwareBilinear` labelled "(0.3.x / 0.4.0 only)".

Detection after the fix: 0.4.1 is `sdli1995 v0.4.1` with SM75 = yes; 0.4.0 is `sdli1995` with SM75 = yes;
SilyNoMeta is `SilyNoMeta v0.3.5-5` (TLOU Part I) or `v0.3.5-4` (the local `dlssg_for_sm86/SilyNoMeta`).

## 5. Verification
- New and extended tests:
  - `dynamic_mfg_detection_unit` (cases 10f-10j; real binaries: `sdli1995 v0.4.1`, `SilyNoMeta v0.3.5-4`);
  - `turing_dual_runtime_unit` (Case 8: backend SM75, plus the real 0.4.1 release);
  - `dlssg_sm86_ini_smoke` (router per runtime);
  - `sm86_packaging_pin_unit` (3/3);
  - `ampere_mfg_status_text_unit` (7/7).

  Each fails against the previous code or pin.
- These suites still pass:
  - `ampere_*`, `turing_*`, `dlssg_sm86_*`, `streamline_*` (except the Windows-only `streamline_active_plugin_smoke`);
  - `xemfg_menu_exclusion`, `nr_driver_gate`, `xefg_menu_multiplier_queue`.
- clang-format 23 clean; UTF-8 BOM present.
- **In game:** confirmed working with 0.4.1 on Ampere (RTX 3080 Ti Laptop) by the maintainer on 2026-10-27, before
  the merge into `main` (PR #31). Turing hardware has not been tested in game; it is covered by the unit tests and the
  real-binary checks.

## 6. Follow-up: Options 0.4.1 No Longer Reads (`a503774b`, `f2668edb`, on `main`)
Two RTX 20/30 options still appeared to work with 0.4.1, although that build ignores them:
- **Hardware Bilinear:** `[Compatibility] HardwareBilinear` is a removed key and is not read.
  - The checkbox is now greyed out, with a tooltip saying why, when the loaded mod is sdli1995 and **states**
    0.4.1 or newer.
  - The saved value is never reset, and the companion INI keeps writing it (rule §4, dual 0.3.x / 0.4.x INI).
- **Preset `Auto`:** 0.4.1 runs `[DLSSG] Preset=Auto` as `A`.
  - The combo's first entry reads "Auto (runs as A in this build)" for the same builds.
  - The stored value and the INI stay `Auto`.

Supporting changes:
- `AmpereMfgLoader::Status` gains `ModVersion`, the version the binary states, which `DetectModVariant` also returns.
- `AmpereMfgStatusText::CompareModVersion` orders dotted versions numerically.

Builds that state no version (sdli1995 0.4.0 and older), SilyNoMeta and an unloaded mod behave as before. An
unversioned sdli1995 build can't be told apart from 0.4.0, which still reads both keys.

Tests:
- `ampere_mfg_status_text_unit` (11/11): version ordering, the lock matrix, menu wiring with no write while locked,
  the Preset label, and an unchanged INI.
- `dynamic_mfg_detection_unit`: the version out-parameter, from synthetic and real binaries.
