---
trigger: always_on
description: Architecture, frame generation, and safety rules for OptiScaler
---

# OptiScaler Architecture & Safety Rules

1. **MFG Mutual Exclusion**:
   - Multi-Frame Generation unlockers (e.g., Ada MFG unlock vs Ampere SM86 MFG) must remain strictly mutually exclusive across:
     - Configuration loading (`Config.cpp`)
     - Configuration saving (`Config.cpp`)
     - Menu UI toggles (`menu_common.cpp`)
     - Runtime execution and hooking (`MfgUnlock.cpp`, `AmpereMfgLoader.cpp`, `dllmain.cpp`)

2. **External Frame Generation Mode**:
   - Sideloaded external FG proxies (such as `dlssg_sm86.dll`) must set `State::Instance().externalFrameGeneration = true`.
   - Apply volatile runtime overrides (`FGInput::NoFG`, `FGOutput::NoFG`, `FGNvngxReplacement::None`) so OptiScaler's internal FG pipeline does not conflict with the game or external proxy.

3. **Menu Subsection Placement**:
   - External FG mod controls and status indicators in `menu/menu_common.cpp` must be placed **before** the `if (state.externalFrameGeneration)` early return, ensuring users can always observe status and toggle settings even when External FG mode is active.

4. **Preservation of MFG Unlockers Across Upstream Merges**:
   - Upstream author (`wilsjo2`) removed / omitted both the SM75-SM86 (Turing/Ampere) MFG unlocker and the Ada (RTX 40) MFG unlocker in subsequent commits.
   - When merging changes from upstream, **both** MFG unlocker components and all related work must **NEVER** be removed, overwritten, disabled, or regressed.
    - Always preserve **SM75-SM86 MFG Unlocker**:
      - `OptiScaler/framegen/dlssg/AmpereMfgLoader.h` and `AmpereMfgLoader.cpp`
      - Config declarations, parsing, saving, and defaults (`Config.h`, `Config.cpp`, `OptiScaler.ini`):
        - `AmpereMfgMaxFrames` up to 5 (6X Multi-Frame Generation on 310.9 runtime)
        - `AmpereMfgOptimized` (19–32% faster kernel execution pipeline)
        - `AmpereMfgPreset` (UI recomposition preset: Auto / A / B)
      - Linux 2X FG FSR Fallback (`AmpereMfgLoader::ShouldFallbackToFsrFg`, `dllmain.cpp`, `menu_common.cpp`)
      - NVAPI hooks (`hkNvAPI_D3D12_SetFlipConfig`, DRS overrides in `NvApiHooks.cpp`) and Kernel hook bypasses (`Kernel_Hooks.cpp`)
      - **Turing & Ampere Native DLSSG Interface Recognition (`menu_common.cpp`)**:
        - `supportsDlssg` must recognize Turing (`0x160`, RTX 20 / GTX 16 / TITAN RTX) and Ampere (`0x170`, RTX 30) along with `ampereActive`.
        - Never forcibly reset `FGOutput` to `NoFG` on Turing hardware.
        - Preserve `"None (Real DLSSG / RTX 20/30 w/ SM75-86 mod)"` menu option.
      - **Streamline Architecture Spoofing for External MFG (`Streamline_Hooks.cpp`)**:
        - `hkdlssg_slOnPluginLoad`: `shouldSpoofArch` must include `ampereMfgActive` so `sl.dlss_g.dll` is spoofed during plugin load even when `activeFgInput == NoFG`.
      - **Dual Runtime Architecture & Turing Router Safety (`AmpereMfgLoader.h`, `AmpereMfgLoader.cpp`)**:
        - Prioritize `310.1` runtime folder (`dlssg_sm86/310.1/dlssg_sm86.dll`) on Turing cards for native SM75 kernels (`DLSSG_SM75_SLOTS`).
        - When running on 310.9 runtime, `ResolveRouter` must output `Router=Auto` on Turing (never `Router=SM75`) to avoid runtime backend abort.
        - Generate 0.3.0 companion INI format (`[General] Enabled=1`, `[FrameGeneration] Optimized=1`, `MaxGeneratedFrames=5`, `[Compatibility] Preset=Auto`).
      - Packaging and build integration (`OptiScaler.vcxproj`, `OptiScaler.vcxproj.filters`, `package_release.ps1`, workflows):
        - `package_release.ps1` must stage both root 310.9 DLL, `310.1/dlssg_sm86.dll`, and modular 0.4.0 components (`backend/`, `kernels/`, `runtime/`) while strictly pruning `sr/` (~180 MB) and `xefg/` (5.4 MB) to prevent release bloat and conflict with native XeMFG.
        - Mandatory post-staging modular integrity assertions in packaging script and workflow pre-checks (`version.dll`, `sm86_backend.dll`, `fg_kernels.dll`, `nvngx_dlssg.dll`).
        - Dual-compatibility INI formatting supporting both 0.3.x (`[General]`, `[FrameGeneration]`) and 0.4.0 (`[DLSSG]`, `Engine=DLSS`, `Preset=C` FG Lite).
      - Associated tests:
        - `tests/dlssg_sm86_ini_smoke.cpp`
        - `tests/kernel_hooks_nvngx_dlssg_unit.cpp`
        - `tests/turing_menu_recognition_unit.cpp`
        - `tests/streamline_turing_spoof_unit.cpp`
        - `tests/turing_dual_runtime_unit.cpp`
        - `tests/dlssg_sm86_v030_ini_unit.cpp`
   - Always preserve **Ada (RTX 40) MFG Unlocker**:
     - `OptiScaler/framegen/dlssg/MfgUnlock.h` and `MfgUnlock.cpp`
     - Build flag `OptiScalerRtx40Mfg` must default to `true` in `OptiScaler.vcxproj` (defines `OPTISCALER_RTX40_MFG`)
     - Config declarations (`FGDLSSGAdaMfgUnlock`, `FGDLSSGAdaBlackwellKernels` in `Config.h`, `Config.cpp`)
     - All `#if defined(OPTISCALER_RTX40_MFG)` guarded code in `DLSSG_Dx12.cpp`, `Streamline_Hooks.cpp`, `LibraryLoad_Hooks.cpp`, `NVNGX_Parameter.cpp`, `NVNGX_DLSS_Dx12.cpp`, `menu_common.cpp`
     - INI entry `AdaMfgUnlock` in `OptiScaler.ini` (must not be stripped during packaging)
     - **Architecture Gate Rewriter (`MfgUnlock::PatchArchGates`)**:
       - Accept only complete, unique known advertise/validate signature families; do not restore broad architecture-comparison fallback patching.
       - Plan gates and optional kernel changes together, check protections/cache operations, retain the module, and roll back on failure. Incomplete rollback must remain an explicit unsafe state.
       - `UnlockedMax()` reports 5 only after the complete requested patch succeeds. Optional kernel rewriting remains separately configured; unknown layouts do not imply success.
     - **NGX Parameter Advertising (`NVNGX_Parameter.cpp`)**:
       - Use `MfgUnlock::EffectiveMax()` for reported and cached capabilities, while strictly maintaining Ampere mutual exclusion. Configuration enablement alone does not establish an unlocked capability.
       - Preserve larger legitimate native capabilities. Failed patch attempts must not trust a potentially altered native report.
     - **Streamline Option Clamping Guard & Pacing (`Streamline_Hooks.cpp`)**:
       - Queue UI intent; apply it only through game-owned options calls. Do not replay cached game structures from the overlay.
       - Copy only fields defined by each options ABI; legacy padding must not become new fields on promotion. Preserve unknown-version storage without slicing.
       - Clamp overrides at use to verified capabilities. Learn capabilities from one game-owned GetState query, without consuming presented-frame counters through extra queries.
       - Commit mode and Reflex pacing only after runtime acceptance. Acknowledge only the matching intent generation and only fields that were actually applied; ignored requests remain pending.
     - **NGX Evaluate & Reflex Synchronization (`NVNGX_DLSS_Dx12.cpp`)**:
       - Streamline-managed evaluation must preserve runtime-supplied counts rather than independently applying pending UI intent. Only direct-NGX ownership may apply the override here.
       - Sync Reflex and detected interpolation count only after evaluation success. Clean patch failures clamp to one generated frame; incomplete rollback refuses evaluation.
     - **Associated Unit Tests**:
       - `tests/ngx_parameter_dlssg_unit.cpp`
       - `tests/mfg_unlock/run.ps1` (production-backed transaction and module-lifetime tests)
       - `tests/streamline_mfg_options_unit.cpp`
       - `tests/dlssg_evaluate_feature_unit.cpp`

5. **Permanent Workflow Branches & Integration Lifecycle**:
   - **`merge-upstream` (Permanent Branch)**:
     - Dedicated strictly to pulling and integrating upstream commits from `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`.
     - Must always maintain the SM75–SM86 mod, the Ada MFG unlocker, the NVSmooth30 unlocker, configs, and Linux fallbacks without regression.
   - **`dlssg-sm86` (Permanent Branch)**:
     - Dedicated to inspecting newer versions of the `dlssg_for_sm86` mod (by sdli1995), analyzing binary/INI differences, and updating OptiScaler loader/hooking integration.
   - **Standard 2-Step Workflow Sequence**:
     1. **Step 1 (Upstream Ingestion)**: Merge new commits from `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass` into `merge-upstream`. Verify integration correctness, compile tests (`dlssg_sm86_ini_smoke`), and ensure CI builds pass.
     2. **Step 2 (Mod Synchronization & Main Promotion)**:
        - Merge `merge-upstream` into `dlssg-sm86`.
        - If there are updates to `dlssg_for_sm86`, inspect differences, adapt `AmpereMfgLoader` / INI generation, test, and then merge `dlssg-sm86` into `main`.
        - If there are no updates to the mod, merge `merge-upstream` directly into `main`.

6. **Preservation of DLSS Neural Rendering (DLSS-NR) Pre-SR Defaults, Diagnostics & Buffer Fallbacks**:
   - Upstream's default configuration and pipeline omission logic caused DLSS-NR to fail silently or get stuck on `"Waiting for the upscaler to run."` unless "Generate model before upscale" was manually selected, or to fail with unhelpful driver errors at high display resolutions.
   - When merging changes from upstream `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`, the following components and fixes must **NEVER** be reverted, removed, or overwritten:
     - **Pre-SR Placement Defaults & Menu Sync**:
       - `Config.h`: `DlssNrRunBeforeSr` must default to `true`.
       - `OptiScaler.ini`: `RunBeforeSR = auto` must resolve to `true` (Pre-SR).
       - `OptiScaler/dlssnr/DlssNr_Menu.cpp`: Checking "Enable Neural Rendering" must auto-select Pre-SR when unset while honoring explicit user overrides.
       - Associated test: `tests/nr_placement_config_unit.cpp`.
     - **Pipeline Setup Diagnostics & Skip Reporting**:
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12.h` & `.cpp`: `DlssNr_Dx12::ReportPipelineSkip(const char* reason)`.
       - `OptiScaler/dlssnr/DlssNr_Pipeline_Dx12.cpp`: Explicit checks and error reporting for missing guides (depth/motion), uninitialized compute shaders, buffer allocation failure, and unsupported subrects.
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12_Status.cpp`: `State::Publish` must expose non-empty `nr.reason` even when `modelRunning` is false, showing the actual error and Retry button in the GUI instead of an infinite wait message.
       - `OptiScaler/upscalers/IFeature_Dx12.cpp`: Diagnostic logging around pipeline scheduling.
       - Associated test: `tests/nr_pipeline_setup_unit.cpp`.
     - **Robust Buffer Creation & Heap Query/Fallback**:
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`: `DlssNr_Dx12::CreateBufferResource` must normalize typeless formats (`TypedGuideFormat`), strip `ALLOW_DEPTH_STENCIL` and `DENY_SHADER_RESOURCE`, query `source->GetHeapProperties`, and gracefully fallback to `D3D12_HEAP_TYPE_DEFAULT` with diagnostic logging.
       - Associated test: `tests/nr_buffer_resource_unit.cpp`.
     - **Contextual Driver Resolution Diagnostics & Quick Action Pre-SR Switch**:
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12_Models.cpp`: Contextual error message when post-upscale creation fails at display resolution: `"the NVIDIA NGX driver could not create Neural Rendering at display resolution (try enabling 'Generate model before upscale' or reducing Working Scale)"`.
       - `OptiScaler/dlssnr/DlssNr_MenuPlacement.cpp`: One-click button `"Switch to Pre-SR (Generate model before upscale)"` under the failure text to instantly switch and retry.
       - Associated test: `tests/nr_status_reporting_unit.cpp`.
     - **Format Bouncing Stability & Cutscene Camera-Cut Surface Caching (FF7 Rebirth Stutter Issue #33)**:
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12_ModelState.h`: `CachedSurfaces altSurfaces` in `ModelStateDx12` caching staging textures across both `modelFormat` and `nativeFormat`.
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12_Resources.cpp`: `ReleaseSurfacesIfFormatChanged` must **never** call `model.RetryAfterFailure()` or reset `modelRunning = false` on format transitions. It must execute zero-allocation O(1) pointer swaps using `altSurfaces` and set `nr.reset = true` without tearing down or rebuilding the NGX feature context. `altSurfaces` must be cleanly retired in `ReleaseResources()`.
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12_Models.cpp`: `PrepareRunModels` must strictly limit `altSurfaces` invalidation to genuine resolution or placement changes (`resolutionChanged || placementChanged`).
       - Documentation: `docs/issues/NR/NR-FF7-REBIRTH.md`.
       - Associated test: `tests/nr_format_bouncing_unit.cpp`.
     - **DLSS 5 Lightweight Model ("Crazy Cuckoo Small FP8") Support & Preset Integration**:
       - `Config.h`: `DlssNrPreset` (defaults to `0` / Auto) and per-pass `NrPassOverrides::preset`.
       - `OptiScaler/dlssnr/DlssNr_MenuControls.cpp`: In-game ImGui `Model preset` combo selector:
         - Pass 1: Auto (`0`, maps to Performance / Small FP8 in 310.9), Quality (`1`, "Honest Tench", full-precision teacher model), Performance (`2`, "Crazy Cuckoo Small FP8", lightweight FP8 Tensor Core model with up to +40% faster inference).
         - Pass 2..10: Inherited combo (Auto / inherit Pass 1, Quality, Performance).
       - `OptiScaler/dlssnr/PassProfiles.h`: Generalized `extra.preset` inheritance across all passes without legacy clamping to `pass <= 2`.
       - `OptiScaler/Config.cpp`: Generalized `Pass{}Preset` loading and saving across multi-pass layers.
       - `OptiScaler/shaders/dlssnr/DlssNr_Dx12_Models.cpp`: `TuningMatchesFeature` using `operator==` on `ModelSettings` to detect preset changes and trigger guarded context recreation.
       - `OptiScaler.ini`: Documented `Preset`, `Pass2Preset`, `Pass3Preset` with reverse-engineered model weights and performance characteristics.
       - Documentation: `docs/issues/NR/NR-LIGHTWEIGHT-MODEL-PRESET-SUPPORT.md`.
       - Associated test: `tests/nr_model_preset_unit.cpp`.

7. **Preservation of NVSmooth30 (RTX 30 Smooth Motion Unlocker) Integration Across Upstream Merges**:
   - Upstream author (`wilsjo2`) does not maintain the NVSmooth30 proxy integration.
   - When merging changes from upstream `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`, NVSmooth30 components and all related work must **NEVER** be removed, overwritten, disabled, or regressed:
     - **Core Loader & Lifecycle**:
       - `OptiScaler/framegen/smoothmotion/NVSmooth30Loader.h` and `NVSmooth30Loader.cpp`.
       - Automatic DRS profile synchronization via `NvApiHooks::ApplySmoothMotionDrs(true)` upon setup.
       - Candidate path resolution strictly prioritizing `basePath / L"OptiScaler" / L"nvsmooth30.dll"`.
       - Post-attach initialization hook in `OptiScaler/dllmain.cpp` (`NVSmooth30Loader::TrySetup()`).
     - **Configuration Backend**:
       - `Config.h`: `SmoothMotionNVSmooth30` setting.
       - `Config.cpp`: Parsing and saving `EnableNVSmooth30` under `[SmoothMotion]`.
     - **In-Game Menu UI (`OptiScaler/menu/menu_common.cpp`)**:
       - Enable interactive Smooth Motion controls on NVIDIA Ampere (RTX 30, `0x170` / `GA100`) alongside Ada and Blackwell.
       - Ampere NVSmooth30 subsection with "Enable NVSmooth30 Unlocker (RTX 30)" checkbox and dynamic status display.
       - Status indicator reflecting `NVSmooth30Loader::GetStatus()`.
     - **Packaging & CI Workflow Integration**:
       - `package_release.ps1`: `[switch]$IncludeNVSmooth30` parameter, staging binary strictly to `OptiScaler/nvsmooth30.dll`, staging license, and dynamic cloning/building from `https://github.com/ItsAdeline/NVSmooth30.git` if local source/binary is absent.
       - `.github/workflows/package_release.yml`: `include_nvsmooth30` workflow_dispatch input, `Ensure NVSmooth30 files are present` step (cloning and building with CMake if missing), and parameter hashtable splatting.
     - **Associated Automated Unit Tests**:
       - `tests/nvsmooth30_loader_unit.cpp`
       - `tests/ampere_smooth_motion_unit.cpp`

8. **Preservation of Unified Intel Xe Multi-Frame Generation (XeMFG) with Source Selection Across Upstream Merges**:
   - Upstream author (`wilsjo2`) does not maintain the Intel XeMFG unlocker or native multi-frame generation routing for XeFG (`libxess_fg.dll`).
   - When merging changes from upstream `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`, Intel XeMFG components and all related work must **NEVER** be removed, overwritten, disabled, or regressed:
     - **Core Loader & Memory Patching Pipeline (`XeMfgLoader.h`, `XeMfgLoader.cpp`)**:
       - 5 transactional binary patches in `libxess_fg.dll` (`U1/frame-count-fallback`, `U2/model-downgrade`, `U3/default-ceiling`, `U4/override-clamp`, `U5/reported-maximum`) unlocking up to 6X Multi-Frame Generation across all GPU vendors.
       - Dynamic session gating: `XeMfgLoader::EnabledForSession()` evaluating dynamically (`XeMfgUnlock && !externalFrameGeneration && !Ada && !Ampere`), and `XeMfgLoader::IsSourceOptiScaler()`.
       - Dynamic in-memory ceiling adaptation: `XeMfgLoader::SetMaxGeneratedFrames` rewrites `U3`, `U4`, and `U5` byte targets live with `VirtualProtect` and `FlushInstructionCache`. Never make the ceiling immutable for the session or lock out the menu slider.
       - Post-attach initialization hook in `OptiScaler/dllmain.cpp` (`XeMfgLoader::TryApply()`).
     - **Mandatory Native Thunk Pacing Engine (`XeFGPacing.h`)**:
       - `XeFGPacing::Install` must run unconditionally whenever XeMFG is patched (`outStatus.Patched == true`).
       - Never gate native thunk pacing behind optional DXGI `ExtraPacing` or config switches; without `XeFGPacing`, `libxess_fg.dll` dumps intermediate generated frames in < 1 ms unmetered bursts and collapses `frameRenderTime` to 0.0f, ruining visual smoothness.
       - Decoupled frame render time (`XeFGPacing::RenderTimeMs()`) must feed valid delta time to Intel in `XeFG_Dx12::Dispatch()`.
       - **End-to-End Burst Pacing, Wait Tracking & Anti-Clump Protection**:
         - Support 2X Frame Generation (`multiplier < 2` guard), pacing the single generated frame to the exact 50% halfway point ($T_{\text{base}} + T_{\text{interval}}$).
         - `SchedForwarder` wrapping `g_schedNative` with QPC timing to capture both intermediate and native Frame $\text{count}$ waits into `g_burstBlockQpc`.
         - In `NoteFrame`, calculate pure engine render time (`pureNs = totalDeltaNs - blockNs`) before median filtering, isolating unblocked engine render time (~16–17 ms) and eliminating the 51.4 ms cycle lock (`Set FT: 50`).
         - In `TsDetour` (RVA `0x3430`), set deadline for Frame 1 to `nowNs` (zero wait) on fresh bursts, as the previous Base Frame already occupied the display during CPU submission and GPU interpolation.
         - Space Base Frame presents (`0x220A52` / `0x220DAD`) by 1 step from Frame $\text{count}$ (`LastFrameCallerRva = 0x220467`) and accumulate sleep into `g_burstBlockQpc`, guaranteeing Frame $\text{count}$ is never overwritten in 0.3 ms.
         - Anti-clump protection in `ApplyAntiClumpProtection` clamps consecutive presents to the minimum floor ($\ge 1.5\text{ ms}$) without stalling the main thread for whole frame intervals.
         - Fall back to `g_periodNs` in `NoteFrame` when inter-frame intervals collapse under 1 ms to prevent zeroed frame render time reporting.
     - **Multiplier Ceiling Uncap (`XeFG_Dx12.cpp`)**:
       - `_maxInterpolationCount` must be initialized to `std::max(_maxInterpolationCount, 5)` in `CreateSwapchain` / `CreateSwapchain1` when `XeMfgLoader::EnabledForSession()`.
       - `SetInterpolatedFrameCount` and `Dispatch` must compute `effectiveMax` via `std::max(_maxInterpolationCount, XeMfgLoader::EffectiveMax(_maxInterpolationCount))` rather than stock ceiling 1, dynamically raising `_maxInterpolationCount` and synchronizing `XeMfgLoader::SetMaxGeneratedFrames(count)` live.
     - **Configuration Backend, Defaults & Backward Compatibility (`Config.h`, `Config.cpp`, `OptiScaler.ini`)**:
       - Unified `[XeMFG]` configuration block:
         - `UnlockMFG` (defaults to `false` when unset / `auto`).
         - `Source` (`Game` vs `OptiScaler`, defaults to `Game`).
         - `MaxInterpolatedFrames` (defaults to 3 / 4X FG, clamped 1 to 5).
         - `ExtraPacing` (defaults to `false`, controlling only DXGI swapchain VBlank lock in `FG_Hooks.cpp`).
         - `ForceBorderless` (defaults to `false` when unset / `auto`, mapping to `FGXeFGForceBorderless`).
       - 100% backward-compatible migration in `Config.cpp`: legacy `[XeMFG-SR] Enable=true` or `[XeMFG] EnableSR=true` transparently migrates to `UnlockMFG=true`, `Source=OptiScaler`, and legacy `GeneratedFrames` migrates to `MaxInterpolatedFrames`.
       - Strict universal mutual exclusion in `Config.cpp` and `menu_common.cpp`: Ada MFG, Ampere SM86 MFG, Smooth Motion, and External FG strictly override and suppress XeMFG.
     - **Automatic Pipeline Auto-Configuration (`OptiScaler/dllmain.cpp`)**:
       - When `XeMfgLoader::EnabledForSession()` is active without external FG:
         - If `XeMfgLoader::IsSourceOptiScaler()`: sets `FGEnabled = true`, `FGInput = FGInput::Upscaler`, `FGOutput = FGOutput::XeFG`, `FGNvngxReplacement = None`, `FGXeFGInterpolationCount = XeMfgMaxFrames`, and invokes `fakenvapi::init(true)` early.
         - Else (`Source == Game`): sets `FGEnabled = true`, `FGInput = FGInput::DLSSG`, `FGOutput = FGOutput::XeFG`, `FGNvngxReplacement = None`.
       - When Streamline receives `DLSSGMode::eOff`, cleanly set `XeFGProxy::SetEnabled(false)` on the swapchain context so Intel does not expect motion vectors or depth buffers while FG is disabled, preventing `-12` mismatch storms. Re-enable upon transitioning back to `eOn`.
     - **Reflex Hook SwapChain & Feature State Gating (`OptiScaler/hooks/Reflex_Hooks.cpp`)**:
       - Reflex hooks (`hkNvAPI_D3D_SetSleepMode`, `hkNvAPI_D3D_Sleep`, etc.) must **never** hijack game calls to `nvapi_calls` unless XeFG is fully initialized and actively presenting (`state.activeFgOutput == FGOutput::XeFG && state.currentFGSwapchain != nullptr && state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused()`).
       - When XeFG is inactive or swapchain is unbound, calls must smoothly fall through to native driver Reflex (`o_NvAPI_D3D_...`).
     - **Thread-Safe `LowLatencyCtx` Lazy-Initialization & Null Guards (`OptiScaler/nvapi/fakenvapi/`)**:
       - `LowLatencyCtx::get()` in `low_latency.h` must be thread-safe with `std::scoped_lock` on static mutex `s_mutex` and self-initializing, guaranteeing it never returns `nullptr`.
       - All 13 dispatch functions in `nvapi_calls.cpp` must null-guard `LowLatencyCtx::get()`.
     - **DXGI 1.2+ Window-Sized SwapChain Resolution (`DxgiSwapchainSizing.h`, `DxgiFactory_Hooks.cpp`, `DxgiFactory_WrappedCalls.cpp`)**:
       - `ResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC1& desc, HWND window)` queries `GetClientRect` when `Width == 0 && Height == 0` on a local descriptor copy *before* the `< 100` overlay check, preserving caller `pDesc` and wrapping normally.
     - **Helldivers 2 Quirk & User Borderless Toggle (`Quirks.h`, `Config.cpp`, `OptiScaler.ini`, `menu_common.cpp`)**:
       - `QUIRK_ENTRY("helldivers2.exe", GameQuirk::ForceBorderlessWhenUsingXeFG)` in `Quirks.h` ensures *Helldivers 2* creates flip-model borderless swapchains without globally forcing borderless on other games.
       - Elevated `Force Borderless Window (restart)##xemfg` directly inside unified XeMFG UI and `[XeMFG] ForceBorderless` config.
     - **SwapChain Init Parameters & Non-Intel Hardware Validation Safety (`XeFG_Dx12.cpp`)**:
       - In `CreateSwapchain` and `CreateSwapchain1`, `params.maxInterpolatedFrames` passed to `XeFGProxy::D3D12InitFromSwapChainDesc` **must** be set to `1` on non-Intel GPUs to satisfy validation gate at `0x1801ffb44`.
       - Unlocked session ceiling (`_maxInterpolationCount = 5`) is maintained in memory, and immediately after `D3D12GetSwapChainPtr`, `SetInterpolatedFrameCount(targetCount)` dynamically binds the target multiplier on frame 1 without manual interaction.
     - **Live Multiplier Adaptation Without Destructive Deactivations (`XeFG_Dx12.cpp`, `menu_common.cpp`)**:
       - `_framesToInterpolate` initialized on swapchain creation.
       - `state.WAR_xefgRequestFGToggle` guarded with `!XeMfgLoader::EnabledForSession()`, and `state.fgChanged = true;` never set in multiplier slider callback. Live adjustments via `SetNumInterpolatedFrames` must never call `Deactivate()`, `SetEnabled: false`, or pause presentation for 10 frames.
     - **FrameRenderTime Sanity Floor Fallback (`XeFG_Dx12.cpp`)**:
       - In `XeFG_Dx12::Dispatch`, if `frameRenderTime < 1.0` or `< 0.25 * _ftDelta[fIndex]`, fallback to `_ftDelta[fIndex]` to stabilize optical flow motion vectors and eliminate judder.
     - **In-Game Menu UI & Section Suppression (`OptiScaler/menu/menu_common.cpp`)**:
       - Consolidated `Intel Xe Multi-Frame Generation (XeMFG)` collapsing header:
         - Enable checkbox with mutual exclusion tooltips.
         - `Input Source` combo selector (`Game (Native DLSS-G / Streamline)` vs `OptiScaler (OptiFG / Upscaler Injection)`).
         - Dynamic telemetry badges (`DLL: found | Patches: 5/5 | Pacing: active`), present status, and active in-game multiplier indicator.
         - Dynamic slider `Max Interpolated Frames##xemfg` (1 to 5 / 2X to 6X FG).
         - Corner debug markers, Only Interpolated Frames, Display VBlank Sync, and Force Borderless Window.
       - When `XeMfgLoader::EnabledForSession()` is active, redundant `Frame Generation (XeFG)` and `Frame Generation (OptiFG)` sections are completely hidden, eliminating clutter and verbose warnings.
     - **HDR RGBA16F Format Diagnostic Warning (`XeFG_Dx12.cpp`)**:
       - Backbuffer format check for `DXGI_FORMAT_R16G16B16A16_FLOAT` / `TYPELESS`, setting `state.xefgHdrFormatUnsupported = true` and logging diagnostic instructions to switch to 10-bit HDR (`R10G10B10A2`) or 8-bit SDR.
     - **Resource Dimension Mismatch Protection & Subrect Decoupling (`XeFG_Dx12.cpp`)**:
       - In `GetResourceData(type, index)`, compare `resourceSize` against backbuffer dimensions `_targetWidth` and `_targetHeight` for `HudlessColor` and `UIColor`. Mismatched auxiliary planes are decoupled/omitted with `LOG_WARN_ONCE`, avoiding error `-12` (`INVALID_ARGUMENT`) in Intel's optical flow runner.
     - **Model Reset Anti-Thrashing Debounce (`XeFG_Dx12.h`, `XeFG_Dx12.cpp`)**:
       - `_lastResetFrame` tracks history resets, debouncing redundant consecutive resets within 3 frames unless swapchain dimensions changed, preventing optical flow collapse and micro-stutter.
     - **Documentation & Associated Automated Unit Tests**:
       - Documentation: `docs/issues/FG/FG-XEMFG-HARDENING-AND-DIAGNOSTICS.md`
       - `tests/xefg_hardening_diagnostics_unit.cpp`
       - `tests/xemfg_loader_unit.cpp` (Tests 1–12)
       - `tests/xemfg_menu_exclusion_unit.cpp` (Tests 1–5)
       - `tests/xefg_diagnostic_ring_unit.cpp` (Tests 1–6)
       - `tests/xefg_main_pacing_alignment_unit.cpp` (Tests 1–6)
       - `tests/xefg_pacing_engine_unit.cpp` (Tests 1–7)
       - `tests/xemfg_multiplier_and_burst_pacing_unit.cpp` (Tests 1–16)
       - `tests/xemfg_sr_crash_fix_unit.cpp` (Tests 1–5)
       - `tests/xemfg_sr_parity_unit.cpp` (Tests 1–6)
       - `tests/xemfg_sr_unit.cpp` (Tests 1–11)


