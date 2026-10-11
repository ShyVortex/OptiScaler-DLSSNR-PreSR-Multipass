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
  - After initially accepting PR #23 containing the SM75/SM86 MFG unlocker, upstream author `wilsjo2` excluded both the SM75-SM86 mod component and the Ada (RTX 40) MFG unlocker in later commits/branches (around v0.8.0–v0.8.3) due to NVIDIA DMCA concerns.
  - This repository (`ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass`) explicitly maintains and supports both MFG unlockers: the Turing (SM75) and Ampere (SM86) MFG unlocker with the Linux 2X FG FSR Fallback pipeline, and the Ada (RTX 40) MFG unlocker (always compiled via `OptiScalerRtx40Mfg=true`).

### Permanent Multi-Branch Architecture
To safely manage upstream merges and mod evolution, three permanent branches are established:
1. **`merge-upstream` (Permanent Upstream Ingestion Branch)**:
   - Used to fetch and merge incoming commits/tags from `upstream` (`wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`).
   - Acts as the initial integration buffer where merge conflicts are resolved and verified, ensuring both SM75–SM86 files and Ada MFG unlocker files, configs, and Linux fallbacks are preserved.
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

## Implementation Planning & Issue Triage Standards
Every implementation plan created for the project must document:
1. **Reported Issue**: The verbatim report and symptoms reported by the user or identified in the field.
2. **Issue Validity Assessment**: Verification confirming that the reported behavior is a genuine issue in OptiScaler / mod integration logic rather than user misconfiguration or unsupported settings.
3. **Solution Applied / Proposed**: The precise architectural/code solution implemented to resolve the defect cleanly.
4. **Issue Documentation in `docs/issues/`**: All investigated or resolved defects must be documented in `docs/issues/` categorized into domain subfolders:
   - `docs/issues/SR/` — Super Resolution (DLSS, FSR, XeSS)
   - `docs/issues/FG/` — Frame Generation (DLSS-G, FSR-FG, XeFG, Smooth Motion, external proxies)
   - `docs/issues/RR/` — Ray Reconstruction (DLSS-D)
   - `docs/issues/NR/` — Neural Rendering (DLSS-NR)

## Intel Xe Multi-Frame Generation (XeMFG & XeMFG-SR) Architecture

### XeMFG Native Streamline Integration (`XeMfgUnlock`)
- **Target**: Games supporting Streamline DLSS Frame Generation.
- **Provider**: Intel XeSS-FG (`libxess_fg.dll` / `igxess_fg.dll`).
- **Core Loader**: `OptiScaler/framegen/xefg/XeMfgLoader.h/cpp` applying 5 transactional binary patches (`U1`–`U5`) to unlock up to 6X Multi-Frame Generation across all GPU vendors.
- **Native Thunk Presentation Pacing**: `OptiScaler/framegen/xefg/XeFGPacing.h` hooking presentation deadlines and scheduler thunks to prevent micro-stutter and burst bunching.
- **Config**: `[XeMFG]` `UnlockMFG`, `MaxInterpolatedFrames` (1..5), `ExtraPacing`.

### XeMFG-SR Injector for DLSS Super Resolution Titles (`XeMfgSrEnable`)
- **Target**: Games supporting DLSS Super Resolution (SR) but lacking native DLSS Frame Generation or Streamline integration.
- **Pipeline Routing**: Siphons color buffers, motion vectors, depth, and exposure from DLSS-SR evaluate passes (`NVSDK_NGX_D3D12_EvaluateFeature`) via `FGInput::Upscaler` into `FGOutput::XeFG`.
- **Loader Dual Activation**: `XeMfgLoader::SrEnabledForSession()` and `AnyEnabledForSession()` activate patches and dynamically resolve ceiling to `XeMfgSrGeneratedFrames`.
- **Pipeline Auto-Configuration**: Post-attach in `dllmain.cpp` automatically sets volatile overrides: `FGEnabled = true`, `FGInput = FGInput::Upscaler`, `FGOutput = FGOutput::XeFG`, `FGNvngxReplacement = None`, and `FGXeFGInterpolationCount = srFrames`.
- **In-Game Menu Overlay**: Dedicated `Intel Xe Multi-Frame Generation for DLSS-SR (XeMFG-SR)` subsection in `menu_common.cpp` with interactive one-click setup, dynamic multiplier slider (2X to 6X / 1 to 5 interpolated frames) with live in-memory byte rewriting (`SetMaxGeneratedFrames`), diagnostic badges, presentation telemetry, and corner debug markers.
- **Mutual Exclusion**: Strict universal mutual exclusion in `Config.cpp` and `menu_common.cpp` against primary `XeMfgUnlock`, `AdaMfgUnlock`, `AmpereMfgUnlock`, `SmoothMotion`, and `ExternalFrameGeneration`.
- **Config**: `[XeMFG-SR]` `Enable`, `GeneratedFrames` (1..5).

- **Helldivers 2 Defect Resolution (Crash & SwapChain Binding)**:
  - Fixed immediate crash (`0xc0000005` at `nullptr + 8`) caused by uninitialized `LowLatencyCtx::get()`, missing null guards in `nvapi_calls`, and unguarded Reflex hook hijacking when XeMFG-SR was enabled mid-game.
  - Fixed inactive interpolation on 0x0 swapchains: *Helldivers 2* called `CreateSwapChainForHwnd` with `Width=0, Height=0`, triggering OptiScaler's premature `< 100` overlay check and bypassing swapchain wrapping (`FGHooks::CreateSwapChainForHwnd`).
  - Added `ResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC1&, HWND)` in `DxgiSwapchainSizing.h` querying `GetClientRect`, and updated `DxgiFactory_Hooks.cpp` and `DxgiFactory_WrappedCalls.cpp` to resolve `localDesc` before overlay checks, preserving caller `pDesc` and allowing normal FG wrapping.
- **Reflex Hook SwapChain Gating**: In `Reflex_Hooks.cpp`, `IsXeFGActive()` strictly verifies `activeFgOutput == FGOutput::XeFG && currentFGSwapchain != nullptr && currentFG != nullptr && currentFG->IsActive() && !currentFG->IsPaused()`. If inactive or if the swapchain is not yet bound to XeFG, Reflex calls fall through smoothly to native driver Reflex (`o_NvAPI_D3D_...`).
- **LowLatencyCtx Lazy-Init & Null Guards**: `LowLatencyCtx::get()` in `low_latency.h` is self-initializing and thread-safe via `std::scoped_lock`. All dispatch functions in `nvapi_calls.cpp` null-guard `LowLatencyCtx::get()`. Early `fakenvapi::init(true)` calls are placed in `dllmain.cpp` and `menu_common.cpp`.
- **Menu Guidance & SwapChain Diagnostics**: `menu_common.cpp` marks XeMFG-SR checkbox as requiring restart (`(restart)`), and displays `"Note: Save Settings and restart the game to initialize the XeFG SwapChain."` whenever `currentFGSwapchain == nullptr`.
- **Fullscreen Incompatibility & Borderless Quirk/Toggle**: Intel XeSS-FG unconditionally disables interpolation in DXGI exclusive fullscreen. Added `QUIRK_ENTRY("helldivers2.exe", GameQuirk::ForceBorderlessWhenUsingXeFG)` in `Quirks.h` and user-controlled toggle `ForceBorderless = auto` under `[XeMFG-SR]` (`OptiScaler.ini`, `Config.cpp`, and GUI).
- **Live Multiplier Adaptation & Pacing Stability**: Initialized `_framesToInterpolate` on swapchain creation, guarded `WAR_xefgRequestFGToggle` in `XeFG_Dx12::Dispatch`, and removed `state.fgChanged = true;` from the slider callback, eliminating destructive deactivations and 10-frame freezes. Added a sanity floor fallback for `frameRenderTime` in `XeFG_Dx12::Dispatch` to prevent motion vector collapse and eliminate micro-stutters.
- **OptiFG Menu Auto-Management**: In `menu_common.cpp`, `Frame Generation (OptiFG)` automatically displays the managed notice and hides manual controls and `Advanced OptiFG Settings` when XeMFG-SR is active.
- **Wall-Clock Native Thunk Pacing Overhaul & Anti-Clump Protection**:
  - Overhauled `OptiScaler/framegen/xefg/XeFGPacing.h` with wall-clock QPC timing and anchored interval scheduling:
    - Supported 2X Frame Generation (`multiplier < 2` guard), pacing the single generated frame to the exact 50% halfway point ($T_{\text{base}} + T_{\text{interval}}$).
    - Unconditionally paced the final generated frame (`isLast == true`, caller `LastFrameCallerRva = 0x220467`), eliminating sub-millisecond dumps (0.28–0.47 ms).
    - Intercepted base application frame presents (`0x220A52` and `0x220DAD`) via `NoteBasePresent()` in `Detour` (`0x25C0`) to anchor subsequent generated frame deadlines ($T_{\text{base}} + \text{index} \times T_{\text{interval}}$) and update cycle periods via `PushPeriod()`.
    - Enforced an anti-clump floor ($\ge g\_lastPresentedQpc + \min(T_{\text{interval}} \times 0.4,\, 2.0\text{ ms})$) preventing consecutive presents from firing closer than the floor threshold.
    - Neutralized cumulative deadline drift in `TsDetour` (RVA `0x3430`) by eliminating mutating global deadline accumulation.
    - Added render time sanity floor in `NoteFrame` so `RenderTimeMs()` falls back to `g_periodNs` instead of underflowing to 0.0 ms.
- **Unit Tests**: Validated by `tests/xemfg_sr_crash_fix_unit.cpp` (5/5 passing), `tests/dxgi_swapchain_sizing_unit.cpp` (9/9 passing), `tests/xemfg_sr_unit.cpp` (11/11 passing), and `tests/xefg_pacing_engine_unit.cpp` (7/7 passing).

