# Frame Generation: WRC Generations Launch Failure and DLSS-G Hook Clearing in External MFG Mode

## 1. Overview & Environment
- **Report**: DLSS Unlocked issue #58 (https://github.com/ShyVortex/dlss-unlocked/issues/58), "wrc generations
  error, the game won't launch".
- **Reporter environment**:
  - Windows 11 (10.0.26300), NVIDIA GeForce RTX 3060 Ti (Ampere), driver 617.42.
  - DLSS Unlocked standalone NR v0.9.72.
  - The install path (`WRC Generations-InsaneRamZes`) indicates an unofficial repack.
- **Configuration**:
  - `FrameGen.External=true`, `DLSSG.AmpereMfgUnlock=true`, `AmpereMfgMode=External`, `AmpereMfgMaxFrames=3`.
  - sdli1995 `dlssg_sm86` v0.4.0 loaded.
  - `LogLevel=2`.
- **Analysis environment** (logs only): CachyOS rolling, kernel 7.3.0-4-cachyos, NVIDIA GeForce RTX 3080 Ti Laptop
  GPU, driver 580.178.04.

## 2. Reported Symptoms and Attempt History
| Build | Result |
|---|---|
| v0.9.72 (`80948410`) | Game does not launch ("error"). |
| v0.9.73 test `9f463e7`, with `17fe1221` | Still does not launch. |
| v0.9.73 test `e191b55`, with `2a0dbf5e` and `90dd1270`, after a **clean reinstall** | "The game launches, but frame generation is unavailable". |

## 3. Issue Validity Assessment
**Launch failure: not attributable to OptiScaler from the available evidence.**
- The three logs are effectively identical: same configuration, same startup, and the same final line
  (`WrappedIDXGISwapChain4 ... created`).
- The failing runs died after OptiScaler's last log line, and `LogLevel=2` records nothing after that.
- The only known difference in the working run is the clean reinstall. Leftover files from the earlier install, or
  files from the repack, are the likeliest cause, but this is unconfirmed.

**`17fe1221` (startup gate) was based on a misread log.** The `CreateSwapChainForHwnd ... Can't init FG Feature`
warning is followed by the wrapped swapchain in the v0.9.72 log. It was never fatal. See the correction in
`FG-EXTERNAL-MFG-SWAPCHAIN-STARTUP-GATE.md`.

**`2a0dbf5e` is a genuine defect, found by code analysis.**
- It added `ampereMfgActive` to the `hkdlssg_slOnPluginLoad` branch that clears `sl.dlss_g`'s `hooks`,
  `exclusive_hooks` and `external/feature/tags` ("We handle the DLSSG resources").
- That branch is the setup for OptiScaler **owning** DLSS-G (`FGInput::DLSSG` / `FGOutput::DLSSG`).
- In External mode `CheckForFGStatus` skips internal FG. The game's DLSS-G, running the modded `nvngx_dlssg.dll`,
  presents the generated frames and needs those hooks and tags. With them cleared, nothing presents the generated
  frames.
- It contradicts architecture rule §4, under which arch spoofing for the unlocker exists so the game's real DLSS-G
  can run.
- It only applies where OptiScaler hooks `sl.dlss_g` (on load, or found in memory). The maintainer's TLOU2 log at
  `LogLevel=0` shows only `sl.common` hooked, which is why it went unnoticed.
- No issue doc or test backed it.

**"Frame generation unavailable" on WRC Generations: probably expected, unconfirmed.**
- External mode only unlocks DLSS-G that the game already has. As far as known, WRC Generations ships DLSS Super
  Resolution only.
- The logs show `sl.common.dll` but no DLSS-G plugin activity. At `LogLevel=2` that cannot be proven either way.

## 4. Solution Applied
`b30c2433`, in `hooks/Streamline_Hooks.cpp` `hkdlssg_slOnPluginLoad`:
- The hook/tag clearing runs only when OptiScaler owns DLSS-G (`FGInput::DLSSG || FGOutput::DLSSG`), as before
  `2a0dbf5e`.
- Unchanged for the unlocker: architecture spoofing (rule §4), the capability hooks (`slIsFeatureSupported` etc.)
  and the vsync/HWS requirement relaxation.
- XeMFG `Source=Game` still clears, because it routes DLSS-G input into XeFG (`FGInput::DLSSG`).

For the reporter, if the game has no native DLSS-G: set `External=false` and use OptiScaler's own frame generation
on the upscaler (XeMFG `Source=OptiScaler`, or XeFG / FSR-FG).

## 5. Verification
- New `tests/streamline_dlssg_hook_ownership_unit.cpp` (4/4 PASS). It reads the production function and checks:
  - clearing is tied to ownership only, with hooks and tags under the same condition;
  - the spoof and the vsync/HWS block still include the unlocker;
  - a decision table across the unlocker, XeMFG and OptiScaler-owned DLSS-G.

  It fails against the pre-fix source.
- These suites still pass:
  - `streamline_external_mfg_hooks_unit`, `streamline_turing_spoof_unit`, `streamline_mfg_options_unit`;
  - `turing_menu_recognition_unit`, `turing_dual_runtime_unit`;
  - `dlssg_sm86_ini_smoke`, `dlssg_sm86_v030_ini_unit`;
  - `xefg_mfg_streamline_unit`, `xemfg_loader_unit`, `xemfg_menu_exclusion_unit`, `xemfg_sr_unit`;
  - `ngx_parameter_dlssg_unit`, `dlssg_evaluate_feature_unit`.
- clang-format 23 clean; UTF-8 BOM present.
- **Pending:**
  - An MSVC build.
  - An in-game check on a game with native DLSS-G where OptiScaler hooks `sl.dlss_g` (a `LogLevel=0` log shows
    `Hooking slGetPluginFunction in sl.dlssg`): the Ampere/Turing External MFG must generate frames.
  - TLOU2 should be unchanged.
  - The reporter's answers: whether the game has DLSS Frame Generation, and a `LogLevel=0` log.
