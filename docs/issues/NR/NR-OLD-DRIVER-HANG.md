# Neural Rendering: GPU Hang on Drivers Without NR Support (DLSS Unlocked #60)

## 1. Overview & Environment
- **Report**: DLSS Unlocked issue #60 (https://github.com/ShyVortex/dlss-unlocked/issues/60), "TLOU 2 freezes when
  Neural rendering is enabled".
- **Reporter environment**:
  - Windows 10 (10.0.19045), NVIDIA GeForce RTX 2080 Ti (Turing), driver **576.88** (NGX dispatcher `_nvngx.dll`
    32.0.15).
  - The Last of Us Part II, DLSS Unlocked 1.7.2 from Nexus (OptiScaler v0.9.72, `80948410`).
  - External FG with the SM75 MFG unlocker active.
  - `nvngx_dlssnr.dll` 310.9.16 beside the game.
- **Working comparison** (maintainer): Windows 11 (10.0.26300), RTX 3080 Ti Laptop GPU, newer driver (dispatcher
  32.0.16), same game and runtime.
- **Analysis environment** (logs only): CachyOS rolling, kernel 7.3.0-4-cachyos, NVIDIA GeForce RTX 3080 Ti Laptop
  GPU, driver 580.178.04.

## 2. Reported Symptoms
Enabling Neural Rendering freezes the game, and only a PC restart recovers. The maintainer could not reproduce it.

## 3. Issue Validity Assessment
Genuine. The reporter's driver is too old, but OptiScaler turned that into a hard GPU hang instead of refusing.

Reporter's log:
```text
[NVSDK_NGX_CreateFeature_Validate:661] error: required feature is not supported by NGX runtime, please update display driver
NR diagnostic CreateFeature(18): result=0xBAD0000C handle=0x0      <- NVSDK_NGX_Result_FAIL_OutOfDate
NR compatibility: loaded ...\nvngx_dlssnr.dll with 2 named caller-path imports, no helper DLL
NR compatibility: Init_Ext result=0x00000001
```
After that the render thread never logs again. Only the input thread continues, for about 50 s.

Maintainer's log:
```text
[NVSDK_NGX_CreateFeature_Validate:762] app id is 142241640                   <- validation passes
NR diagnostic CreateFeature(18): result=0xBAD0000B handle=0x0      <- FAIL_UnableToInitializeFeature
NR compatibility: candidate ...\nvngx_dlssnr.dll CreateFeature(18) result=0x00000001 handle=0x2
DLSS-NR: feature created at 1512x848 through direct compatibility runtime
```

## 4. Root Cause
- The direct compatibility runtime (`docs/NR-DIRECT-RUNTIME.md`) exists for a driver that knows NR but whose signed
  loader rejects the RTX 20/30/40 `nvngx_dlssnr.dll`. That rejection is `FAIL_UnableToInitializeFeature`, the
  maintainer's case.
- Upstream `47e134cb` widened the fallback to any failure without a handle (`docs/NR-RESTRICTION-AUDIT.md`). That
  included `FAIL_OutOfDate`, where the driver has no NR support at all.
- On 576.88 OptiScaler therefore created the model directly against a driver without NR support, and the GPU/driver
  hung.
- The cutoff:
  - 616.64 is the first driver with official NR support.
  - The maintainer verified that NR works on 610.88.
  - 576.88 fails.

## 5. Prior-Attempt Audit
- No earlier `docs/issues/` entry covers driver support.
- `NR-RESTRICTION-AUDIT.md` removed an NR **DLL** version/hash/size gate in v0.8.2. The fix below gates on the
  **driver** and leaves that DLL gate removed.
- The fork's contextual post-SR driver error (architecture rule §6) is preserved.

## 6. Solution Applied
- **`97f35314`: version gate.**
  - `IdentifyGpu::queryNvapi` reads `NvAPI_SYS_GetDriverAndBranchVersion` into `GpuInformation::nvidiaDriverVersion`.
  - `dlssnr/DlssNr_DriverGate.h` sets the minimum to 610.88 (`61088`, the oldest version verified to work) and
    recommends 616.64 (`61664`, the first official).
  - On native Windows below the minimum, the GPU worker sets a volatile `DlssNrEnabled = false`:
    - every NR path stays off;
    - the user's INI is never rewritten (`value_for_config` saves their own value);
    - the block survives `Config::Reload` and profile loads (`set_from_config` never replaces a set value).
  - Wine/Proton is exempt, because dxvk-nvapi reports Linux driver numbers. An unknown version does not block.
  - The NR proxy also refuses creation before any NGX call while blocked.
- **`521138df`: menu and hotkey.**
  - The "Enable Neural Rendering" checkbox is disabled while blocked.
  - An amber line and tooltip read: "Neural Rendering requires NVIDIA driver 610.88 or newer (616.64+ recommended;
    detected X). Update your driver to enable it."
  - The NR hotkey shows that message as a warning toast and consumes the key instead of toggling.
- **`dec66857`: no direct runtime after `FAIL_OutOfDate`.**
  - Every other failure still falls back.
  - The status reason becomes "the NVIDIA driver does not support Neural Rendering; update to 610.88 or newer
    (616.64+ recommended)".
  - This covers the cases the version gate exempts: Wine and unknown versions.
- `OptiScaler.ini` documents the requirement under `[DlssNr] Enabled`. `NR-RESTRICTION-AUDIT.md` lists the
  OutOfDate exclusion as retained.

## 7. Verification
- `tests/nr_driver_gate_unit.cpp` (13/13 PASS) covers:
  - the 61087/61088 boundaries, with 616.64 allowed;
  - Wine exempt and unknown allowed;
  - the messages;
  - the NVAPI source;
  - startup order;
  - reload survival;
  - the proxy refusal before NGX;
  - the menu and hotkey guards, and that only those two sites write `DlssNrEnabled`;
  - the OutOfDate exclusion with every other failure still falling back;
  - the status reason, with the contextual display-resolution message kept.

  Each group fails against the pre-fix sources.
- These suites still pass:
  - `nr_status_reporting`, `nr_format_bouncing`, `nr_candidate_paths_fallback`, `nr_pipeline_setup`,
    `nr_model_preset`, `nr_halo_dx12`, `nr_placement_config`, `nr_finished_picture_gate`, `nr_model_preset_menu`;
  - the `dlssg_sm86`, `turing_*` and XeFG/XeMFG suites.
- clang-format 23 clean; UTF-8 BOM present. `IdentifyGpu.h/.cpp` were missing it and now have it.
- **Pending:**
  - An MSVC build.
  - In game:
    - NR still enables on a driver ≥ 610.88;
    - the #60 reporter (576.88) sees the disabled checkbox and message, with no hang.
