# OptiScaler-DLSSNR-PreSR-Multipass — Integration Context

## Project Overview
OptiScaler is a Windows x64 DLL proxy (dxgi.dll / d3d12.dll / version.dll etc.) that intercepts graphics calls to provide upscaling (DLSS, FSR, XeSS) and frame generation (DLSSG, FSR-FG, XeFG, OptiFG) support to games. It ships as a Visual Studio 2022 (v143) C++20 solution.

## Key Architecture for Ampere MFG Unlocker Integration

### Existing Ada MFG Unlocker Pattern
The Ada (RTX 40) MFG unlocker is the reference pattern. It lives in:
- **Config**: `Config.h` L807-808 (`FGDLSSGAdaMfgUnlock`, `FGDLSSGAdaBlackwellKernels`)
- **Config parse**: `Config.cpp` L72-73 reads `[DLSSG]` / `AdaMfgUnlock` and `AdaBlackwellKernels`
- **Config save**: `Config.cpp` L978-979
- **INI**: `OptiScaler.ini` L280-286 under `[DLSSG]`
- **Logic**: `framegen/dlssg/MfgUnlock.h/cpp` — patches nvngx_dlssg.dll in-memory
- **Menu**: `menu/menu_common.cpp` L3076-3090 — checkbox + status display
- **Hooks**: `hooks/LibraryLoad_Hooks.cpp` L122 and `hooks/Streamline_Hooks.cpp` L1152,1220 — calls `TryApply()`
- **Consumers**: `hooks/Streamline_Hooks.cpp` and `framegen/dlssg/DLSSG_Dx12.cpp` use `UnlockedMax()` and `Pending()`

### dlssg_for_sm86 Mod Analysis
The mod by sdli1995 is a **standalone DLL proxy** (`version.dll` + `dlssg_sm86.ini`) for Ampere (SM86, RTX 30 series) that:
1. Embeds an entire DLSSG 310.1 runtime with SM86 backend and models
2. Intercepts DLSSG requests, redirects them to the embedded implementation
3. Supports Multi-Frame Generation up to 4X (MaxGeneratedFrames=3 → 4X)
4. Is a ~10MB binary with bundled runtime — NOT a simple memory patch like the Ada unlocker

### Critical Difference: Ada vs Ampere Approach
- **Ada unlocker**: Pure memory patching of the loaded `nvngx_dlssg.dll` (rewrites byte patterns, retargets kernels). Zero external files. ~400 lines of C++.
- **Ampere mod**: Ships its own complete DLSSG runtime with SM86 CUDA kernels. A 10MB standalone DLL that acts as a proxy. Sideloaded via `LoadLibraryW` from `OptiScaler/dlssg_sm86/dlssg_sm86.dll`.

### Ampere SM86 Integration Architecture
1. **Config Layer**:
   - `Config.h`: `FGDLSSGAmpereMfgUnlock` (bool), `FGDLSSGAmpereMfgMaxFrames` (int 0–3), `FGDLSSGAmpereMfgKernelImage` (string Auto/PTX/Cubin).
   - `Config.cpp`: Loads under `[DLSSG]` with bounds check `[0, 3]`; saves values back to `OptiScaler.ini`.
   - `OptiScaler.ini`: Documented entries under `[DLSSG]`.
2. **AmpereMfgLoader Module** (`framegen/dlssg/AmpereMfgLoader.h/cpp`):
   - `Status` tracks `Enabled`, `DllFound`, `IniWritten`, `DllLoaded`, and `ErrorMessage`.
   - `GenerateIniContent()` creates dynamic `dlssg_sm86.ini` matching upstream format.
   - `TrySetup()` guards against non-Ampere GPUs (`NV_GPU_ARCHITECTURE_GA100` / `0x170`), Ada mutual exclusion, locates DLL in `dlssg_sm86/`, writes INI beside DLL, and calls `LoadLibrary`.
3. **Menu UI** (`menu/menu_common.cpp`):
   - Collapsible header `"RTX 30 (Ampere SM86) MFG Unlock"`.
   - Toggle checkbox with mutual exclusion (greyed out if Ada is active, and vice versa).
   - Live status display (`DLL: found | INI: written | Loaded: yes/no` or red error).
   - `MaxGeneratedFrames##sm86` slider (0–3 with descriptive labels).
   - `KernelImage##sm86` combo dropdown (`Auto`, `PTX`, `Cubin`).
   - Positioned before `if (state.externalFrameGeneration)` early return so controls remain visible in External FG mode.
4. **Startup & Runtime Wiring** (`dllmain.cpp`, `MfgUnlock.cpp`):
   - `dllmain.cpp` invokes `AmpereMfgLoader::TrySetup()` during startup.
   - Automatically switches OptiScaler to External FG mode (`state.externalFrameGeneration = true` with volatile overrides) so game controls the multiplier and OptiScaler DLSSG hooks do not interfere.
   - `MfgUnlock.cpp` guards both `TryApply()` and `Pending()` against `FGDLSSGAmpereMfgUnlock`.
5. **Build & Packaging** (`OptiScaler.vcxproj`, `OptiScaler.vcxproj.filters`, `package_release.ps1`):
   - Added `AmpereMfgLoader.h` and `AmpereMfgLoader.cpp` to VS project and filters.
   - `package_release.ps1` supports `-IncludeAmpereMfg` and `-AcceptAmpereMfgLicenses`, bundles `dlssg_sm86.dll` and notices into `OptiScaler/dlssg_sm86/`, and prevents packaging if `AmpereMfgUnlock=true`.

### GPU Architecture IDs (from nvapi)
- `NV_GPU_ARCHITECTURE_TU100` — Turing (RTX 20)
- Ampere (RTX 30) — `NV_GPU_ARCHITECTURE_GA100` (0x170)
- `NV_GPU_ARCHITECTURE_AD100` — Ada (RTX 40)
- Blackwell (RTX 50) — 0x1B0

### State & Config Pattern
- `CustomOptional<T>` for config values with `set_from_config()`, `value_or_default()`, `value_for_config()`, `set_volatile_value()`
- `State::Instance()` singleton for runtime state
- Menu UI in `menu/menu_common.cpp` using ImGui
- External FG mode (`externalFrameGeneration`) disables OptiScaler's own FG but allows external unlockers

## File Locations Summary
| Purpose | File |
|---------|------|
| Config declaration | `OptiScaler/Config.h` |
| Config load/save | `OptiScaler/Config.cpp` |
| Runtime state | `OptiScaler/State.h` |
| Ada MFG unlock logic | `OptiScaler/framegen/dlssg/MfgUnlock.h/cpp` |
| Ampere MFG loader | `OptiScaler/framegen/dlssg/AmpereMfgLoader.h/cpp` |
| DLSSG frame gen | `OptiScaler/framegen/dlssg/DLSSG_Dx12.h/cpp` |
| Overlay menu | `OptiScaler/menu/menu_common.cpp` |
| Library load hooks | `OptiScaler/hooks/LibraryLoad_Hooks.cpp` |
| Streamline hooks | `OptiScaler/hooks/Streamline_Hooks.cpp` |
| DLL entry point | `OptiScaler/dllmain.cpp` |
| INI config | `OptiScaler.ini` |
| VS project | `OptiScaler/OptiScaler.vcxproj` |
| VS project filters | `OptiScaler/OptiScaler.vcxproj.filters` |
| GPU identification | `OptiScaler/misc/IdentifyGpu.h/cpp` |
| Package release | `package_release.ps1` |
| Ampere mod upstream | `dlssg_for_sm86/` |

## Upstream Fork & Merge Strategy
- **Upstream Repository**: `https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass.git` (remote: `upstream`)
- **Divergence Context**:
  - After initially accepting PR #23 containing the SM75/SM86 MFG unlocker, upstream author `wilsjo2` excluded the SM75-SM86 mod component in later commits/branches (around v0.8.0–v0.8.3) due to NVIDIA DMCA concerns.
  - This repository (`ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass`) explicitly maintains and supports the Turing (SM75) and Ampere (SM86) MFG unlocker along with the Linux 2X FG FSR Fallback pipeline.

### Permanent Multi-Branch Architecture
To safely manage upstream merges and mod evolution, three permanent branches are established:
1. **`merge-upstream` (Permanent Upstream Ingestion Branch)**:
   - Used to fetch and merge incoming commits/tags from `upstream` (`wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`).
   - Acts as the initial integration buffer where merge conflicts are resolved and verified, ensuring SM75–SM86 files, configs, and Linux fallbacks are preserved.
2. **`dlssg-sm86` (Permanent Mod Tracking & Inspection Branch)**:
   - Used to inspect newer releases/commits of `sdli1995/dlssg_for_sm86`.
   - Used to diff proxy binaries, analyze configuration/INI adjustments, and update OptiScaler's `AmpereMfgLoader` / packaging integration.
3. **`main` (Production Branch)**:
   - Verified, deployable production branch where CI builds and release packaging take place.

### Standard Integration Workflow
1. **Step 1: Ingest Upstream into `merge-upstream`**:
   - Check out `merge-upstream` and merge the desired commit/tag from `upstream`.
   - Resolve any conflicts while strictly keeping all SM75–SM86 headers, source files, UI elements, and configs.
   - Verify code compiles and unit tests pass (`g++ -std=c++20 tests/dlssg_sm86_ini_smoke.cpp`).
2. **Step 2: Sync with `dlssg-sm86` and Promote to `main`**:
   - Merge `merge-upstream` into `dlssg-sm86`.
   - **Case A (Mod Updates Available)**: If a new `dlssg_for_sm86` release exists, inspect differences in `dlssg-sm86`, apply necessary updates to OptiScaler's loader/hooks, verify with tests, and merge `dlssg-sm86` into `main`.
   - **Case B (No Mod Updates)**: If no mod updates are required, directly merge `merge-upstream` into `main`.


