# Frame Generation: Remnant 2 Shows No Generated Frames With the RTX 20/30 Unlocker (Generation-A DLSS-G Runtime)

## 1. Overview & Environment
- **Report**: DLSS Unlocked issue #61 (https://github.com/ShyVortex/dlss-unlocked/issues/61), "DLSS doesn't display
  generated frames".
- **Reporter environment**:
  - Windows 11 (10.0.26200), NVIDIA GeForce RTX 3060 Ti (Ampere, arch `0x170`), driver 617.42.
  - DLSS Unlocked v0.9.80 (installer), upgraded from v0.9.72.
  - Remnant 2 (Steam), Unreal Engine 5.2. It ships DLSS SR 3.7.20 and DLSS-G (`nvngx_dlssg.dll`) **3.7.10** in
    `Remnant2/Plugins/Shared/Streamline/Binaries/ThirdParty/Win64`.
- **Configuration**:
  - `FrameGen.External=true`, `DLSSG.AmpereMfgUnlock=true`, `AmpereMfgMode=External`, everything else auto.
  - sdli1995 `dlssg_sm86` **v0.4.1** loaded.
  - `LogLevel=0`.
- **Analysis environment** (logs only): CachyOS, kernel 7.3.0-4-cachyos, NVIDIA GeForce RTX 3080 Ti Laptop GPU, driver
  580.178.04.

## 2. Reported Symptoms
- With DLSS Frame Generation switched on in the game, the real frame rate drops, as it normally does with FG.
- No generated frames appear. The Steam overlay and the OptiScaler counter show only the reduced real frame rate.
- The reproduction steps start from uninstalling v0.9.72, which implies FG worked there. The report does not say so
  explicitly.

## 3. Issue Validity Assessment
**OptiScaler behaves as designed. The cause is a deliberate change in sdli1995 0.4.1. The silent failure was a
genuine diagnostics gap in OptiScaler.**

Log evidence (`OptiScaler.log`):
- The unlocker is loaded: `Mod: sdli1995 v0.4.1 ... SM86/SM75 MFG loaded successfully`.
- `sl.dlss_g` is hooked, and v0.9.80 contains `b30c2433`. This is therefore not the #58 hook-clearing defect
  (`FG-WRC-GENERATIONS-EXTERNAL-MFG.md`).
- Streamline initialises NGX through OptiScaler. Its feature path list puts the game's Streamline folder at [1] and
  the mod's own runtime (`OptiScaler\dlssg_sm86\dlssg_sm86\runtime`) last, at [7].
- Every `NVSDK_NGX_D3D12_CreateFeature` for feature 11 (frame generation) failed in the native passthrough with
  `0xBAD0000B` (`NVSDK_NGX_Result_FAIL_UnableToInitializeFeature`). That happened 3384 times in about 3460 presents:
  Streamline retries on every frame.
- The driver's own NGX log (captured by `DlssNr_NgxDiagnostics`) shows why:
  ```
  NGXLoadLibrary: Loaded library from path (.../Plugins/Shared/Streamline/Binaries/ThirdParty/Win64/nvngx_dlssg.dll)
  NGXValidateSnippetMetaData: GPU architecture : 0x170, Snippet expects at least : 0x190
  NGXValidateSnippetMetaData: error: snippet is using newer GPU arch 190 > 170
  ```
  The driver-store fallback `nvngx_dlssg.dll` fails the same check. NVIDIA's runtime requires Ada.

The mod's documentation (`dlssg_for_sm86/sdli1995/README.en.md`, Known issues; `docs/INSTALL.en.md`, "Which
generation the game's own runtime is"):
- DLSS-G 1.0.1–3.8.x is **generation A**. It has no `DLSSG.MultiFrameCount` contract, and its Streamline plugin can
  drive only 2X.
- **Since 2026-10-19** the mod does not take a generation-A runtime over. The game's `nvngx_dlssg.dll` request is
  forwarded unchanged, and the mod logs one error-level `game_runtime_generation` with
  `action: frame_generation_not_enabled`. This is the mod owner's decision. Before it, these games got 2X.
- v0.9.72 bundled sdli1995 0.4.0, from before that change. That is why the reporter saw it as a regression.

The OptiScaler gap: neither the log nor the menu said why frame generation never ran. The menu headline read
"running", and the only trace was an info-level CreateFeature failure on every frame.

The mod's log, `OptiScaler\dlssg_sm86\dlssg_sm86\logs\dlssg_sm86.log`, was requested from the reporter as
confirmation.

## 4. Root Cause
- Remnant 2 ships a generation-A DLSS-G runtime (3.7.10).
- The bundled sdli1995 0.4.1 forwards such runtimes unchanged by design.
- NVIDIA's runtime refuses RTX 20/30, so the game's DLSS-G never initialises, but the game keeps FG on and pays
  its cost.

## 5. Applied Solution
OptiScaler now detects and reports the case. FG behaviour is unchanged: there is no automatic fallback, and the MFG
mutual exclusion, the `sl.dlss_g` hooks and the mod owner's decision are left alone.

1. **Classification** (`OptiScaler/framegen/dlssg/DlssgRuntimeGeneration.h`, `3da86ef6`):
   - `Classify(major)`: below 310 is A, 310 and newer is B, an unread version is unknown.
   - `SelectGameRuntime`: the first `nvngx_dlssg.dll` in NGX search order outside the excluded roots. Paths are
     compared case-insensitively, with either separator.
2. **Detection** (`AmpereMfgLoader::NoteGameDlssgRuntime`, called at the end of `UpdateInitPaths` in
   `inputs/NVNGX_DLSS_Dx12.cpp` when `AmpereMfgUnlock` is on, `0c14078d`):
   - It runs only with the mod loaded and no Linux FSR fallback.
   - It excludes `<mod dir>\dlssg_sm86` (the mod's 310.9 runtime), plus the mod directory unless that is the game
     folder (a plain install can keep the game's own runtime there).
   - It stores `GameDlssgPath`, `GameDlssgVersion` and `GameDlssgGenerationA` in the loader status and logs the
     runtime at info level. Later NGX inits without a game runtime keep the earlier result.
   - For generation A with sdli1995 stating 0.4.1 or newer (`AmpereMfgStatusText::IsSdli1995AtLeast041`, version
     read from the binary), it logs one `LOG_WARN` that names the alternatives.
   - `State::NVNGX_DLSSG_Path` is not used here: in this log `FindFilePath` resolved it to the mod's runtime copy.
3. **Menu** (`AmpereMfgStatusText`, `menu_common.cpp`, `2a268b75`):
   - The RTX 20/30 headline reads "not running for this game: it ships DLSS-G 3.7.10 (older than 310.x), which
     sdli1995 v0.4.1 does not take over". A loader error message keeps priority.
   - An amber line under the headline offers the alternatives: update the game's `nvngx_dlssg.dll` to 310.x (for
     example with DLSS Swapper), or turn the unlock off and use XeMFG (Input Source: Game) or OptiFG.
   - The details marker gains "Game DLSS-G: <version>".
   - The block sits in the existing RTX 20/30 section, before the `externalFrameGeneration` early return.

## 6. Workarounds Given to the Reporter
- **Swap the game's DLSS-G file to 310.x** with DLSS Swapper (https://github.com/beeradmoore/dlss-swapper). The mod
  then takes the runtime over again. The game's old Streamline plugin may still limit it to 2X. Steam file
  verification or a game update can restore the old file.
- **Use XeMFG**:
  - `[XeMFG] UnlockMFG=true`, `Source=Game`;
  - with `[DLSSG] AmpereMfgUnlock=false` and `[FrameGen] External=false`, because XeMFG is mutually exclusive with
    the unlocker and with External mode.
- **Stay on v0.9.72** (sdli1995 0.4.0, 2X) for this game.

## 7. Test Verification
- `tests/dlssg_runtime_generation_unit.cpp` (8/8):
  - classification;
  - version text and path normalisation;
  - search order and exclusion of the mod's runtime;
  - the Remnant 2 path list from the #61 log;
  - source wiring of `UpdateInitPaths` and `NoteGameDlssgRuntime`.
- `tests/ampere_mfg_status_text_unit.cpp` (13/13), Tests 12–13:
  - refused only for sdli1995 0.4.1+ with the mod loaded and a generation-A runtime;
  - 0.4.0, unversioned, SilyNoMeta, 310.x and not-loaded cases are unchanged;
  - an error message keeps priority;
  - the details line, and the menu wiring order.
- Still passing:
  - `dlssg_sm86_ini_smoke`, `dlssg_sm86_v030_ini_unit`, `streamline_dlssg_hook_ownership_unit`,
    `kernel_hooks_nvngx_dlssg_unit`;
  - `ampere_mfg_*` (mode config, ceiling UI, internal control, ASI init);
  - `turing_dual_runtime_unit`, `turing_menu_recognition_unit`, `ampere_smooth_motion_unit`,
    `xemfg_menu_exclusion_unit`, `nvsmooth30_loader_unit`.
- clang-format 23 clean and UTF-8 BOM verified on every changed C++ file.
- The Windows build has not run yet (the code cannot build on Linux; CI builds it after the push). An in-game check
  (Remnant 2 or another generation-A game on RTX 20/30) is pending.

## 8. Out of Scope
- Rate-limiting the per-frame `Native CreateFeature failed` info line for feature 11.
- Any automatic switch to XeMFG or OptiFG for generation-A games. It would interact with the MFG mutual-exclusion
  rules and needs its own decision.
