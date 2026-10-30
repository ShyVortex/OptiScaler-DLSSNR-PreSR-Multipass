# Super Resolution: DLSS Not Detected When the Game Exe Is Outside Binaries\Win64 (Phantom Blade Zero)

## 1. Overview & Environment
- **Report**: [DLSS Unlocked #62](https://github.com/ShyVortex/DLSS-Unlocked/issues/62), "Latest version of
  dlss-unlocked does not detect dlss upscaler in Phantom Blade Zero".
- **Reporter environment** (from the issue and its `OptiScaler.log`):
  - Windows 11 (10.0.26200), NVIDIA GeForce RTX 4070 Ti (Ada, arch `0x190`), driver 621.01.
  - DLSS Unlocked `dlss-unlocked-setup-NR-v0.9.80` (installer), OptiScaler v0.9.80-final (`9df776d2`).
  - Phantom Blade Zero (Steam), Unreal Engine 5.7. The exe is `PBZ\platform\steam\PBZ.exe`, with OptiScaler installed
    beside it as `version.dll`. The engine plugins are under `PBZ\Engine\Plugins\Runtime\Nvidia\...`; Streamline 2.14.1
    loads from `...\Nvidia\StreamlineCore\Binaries\ThirdParty\Win64`.
- **Configuration**:
  - `Dx12Upscaler=xess`, `AdaMfgUnlock=true`, `LogLevel=2`, everything else auto.
  - `[Libraries] NvngxDlssPath=auto`.
- **Analysis environment** (logs only): CachyOS, kernel 7.3.0-4-cachyos, NVIDIA GeForce RTX 3080 Ti Laptop GPU, driver
  580.178.04.

## 2. Reported Symptoms
- DLSS isn't offered as an upscaler in the OptiScaler overlay ("no dlss upscalers found").
- The overlay shows "nvngx_dlss.dll not found, DLSS disabled!".
- The game's DLSS option is served by XeSS instead.

## 3. Issue Validity Assessment
**A genuine OptiScaler defect: the DLL search did not cover this Unreal Engine layout. It is not a regression.**
`Util::FindFilePath` and the startup DLSS check are unchanged since v0.9.72, so earlier versions behave the same
in this game.

Log evidence (`OptiScaler.log`):
- At startup: `Check for DLSS files` → `nvngx_dlss.dll not found, disabling DLSS`. `dllmain.cpp` then sets
  `DLSSEnabled=false` (volatile) for the session, and `FeatureProvider_Dx12` never offers DLSS.
- The game calls NGX directly through Unreal's DLSS plugin. Streamline loads only `sl.common`, `sl.pcl` and
  `sl.reflex`.
- OptiScaler serves the game's feature with the configured upscaler: `TryCreateOptiFeature Creating XeSS upscaler
  feature`.
- The game's NGX init, which carries its plugin path list, arrives after OptiScaler's own NGX init and returns at
  "NVNGX already inited". By then the startup decision has already been made.

Not related:
- `Disabling DLSS-G since it is not supported on current hardware` is Streamline's stock min-spec check. The game
  does not request DLSS-G.
- The `Microsoft Basic Render Driver` device is a probe; the game renders on the RTX 4070 Ti.
- `AdaMfgUnlock=true` plays no part.

Audit: no earlier `docs/issues/` entry covers DLSS DLL detection. `FindFilePath` history: `64efe037` (skip dev DLLs
under `OptiScaler\streamline`), `a795e090` (KCD2 `Win64MasterMasterSteamPGO`), `b580fc45` (absolute `MainDllPath`).

## 4. Root Cause
`Util::FindFilePath(startDir, file)` (`OptiScaler/Util.cpp`) is called first from the OptiScaler folder, then from
the exe folder. It tried:
1. `startDir` itself;
2. a breadth-first search below `startDir`;
3. an Unreal fallback that runs only when `startDir\..\..\{Win64|WinGDK|Win64MasterMasterSteamPGO}` exists, then
   searches from the project root.

The standard layout `<Root>\<Project>\Binaries\Win64\` matches step 3. Phantom Blade Zero's exe sits in
`PBZ\platform\steam\`, so step 3 looks for `PBZ\platform\Win64`, which doesn't exist. `PBZ\Engine` was never
searched.

## 5. Applied Solution
Commit `27bcb0ee`:
- New step 4 in `Util::FindFilePath`. It runs only if steps 1-3 found nothing **and** the step 3 Unreal fallback did
  not run, so a standard layout, whose root was already searched, is never searched twice.
- Step 4 walks up at most 3 folders from `startDir` (skipping `startDir` itself). The nearest folder that contains
  an `Engine` folder is taken as the game root, and the same filtered breadth-first search runs from there. The
  existing filter that skips `OptiScaler\streamline` dev DLLs is kept.
- The lookup is a pure helper, `GameRootSearch::FindEngineRoot` in `OptiScaler/misc/GameRootSearch.h`, which takes a
  directory predicate so it can be tested without a filesystem.
- What stays the same:
  - `[Libraries] NvngxDlssPath` still takes priority;
  - the OptiScaler folder is still searched before the exe folder, so the bundled sm86 `nvngx_dlssg.dll` keeps its
    priority;
  - `nvngx_dlssd.dll` and `nvngx_dlssg.dll` use the same search and benefit from step 4 too.
- `OptiScaler.ini`: the `NvngxDlssPath` comment now says it takes the folder that contains the DLL, and what `auto`
  searches.

Not changed: the startup DLSS gate, NGX path-list handling (`UpdateInitPaths`), and Streamline.

## 6. Workarounds Given to the Reporter
- Set `[Libraries] NvngxDlssPath` to the **folder** under `PBZ\Engine\Plugins\...` that contains `nvngx_dlss.dll`.
  The override accepts a directory only (`dllmain.cpp`).
- Or copy `nvngx_dlss.dll` next to `PBZ.exe`; the exe folder is searched directly.

## 7. Test Verification
- `tests/game_root_search_unit.cpp` (8/8 PASS, `g++ -std=c++20`):
  1. The PBZ exe folder finds the `PBZ` root.
  2. The OptiScaler folder (3 levels up) finds the same root.
  3. No `Engine` folder: no root.
  4. `Engine` 4 levels up is beyond the limit.
  5. Stops at the filesystem root.
  6. The nearest `Engine` ancestor wins.
  7. `startDir` is never its own root.
  8. Source check of `Util.cpp`: step 4 runs after steps 2 and 3, only when the Unreal fallback did not run, and
     through the filtered BFS.
- `tests/util_delayed_destroy_unit.cpp` still passes.
- clang-format 23 and UTF-8 BOM checks pass.
- **Pending**: the MSVC build, and the reporter's in-game check with `NvngxDlssPath=auto`. Expected: the log shows
  `nvngx_dlss.dll found at ...\PBZ\Engine\...` and `Enabling DLSS`, and DLSS is selectable and runs.

## 8. Out of Scope
- Using the path list from the game's own NGX init to find the DLL (option B in the analysis). That needs the startup
  decision to be revisited after OptiScaler's own early NGX init, and is only worth it if a layout appears that step 4
  doesn't cover.
