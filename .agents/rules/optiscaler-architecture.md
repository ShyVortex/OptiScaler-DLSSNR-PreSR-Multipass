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
        - `package_release.ps1` must stage both root 310.9 DLL and `310.1/dlssg_sm86.dll`.
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

8. **Preservation of Intel Xe Multi-Frame Generation (XeMFG) Integration Across Upstream Merges**:
   - Upstream author (`wilsjo2`) does not maintain the Intel XeMFG unlocker or native multi-frame generation routing for XeFG (`libxess_fg.dll`).
   - When merging changes from upstream `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`, Intel XeMFG components and all related work must **NEVER** be removed, overwritten, disabled, or regressed:
     - **Core Loader & Memory Patching Pipeline**:
       - `OptiScaler/framegen/xefg/XeMfgLoader.h` and `XeMfgLoader.cpp`.
       - 5 transactional binary patches in `libxess_fg.dll` (`U1/frame-count-fallback`, `U2/model-downgrade`, `U3/default-ceiling`, `U4/override-clamp`, `U5/reported-maximum`) unlocking up to 6X Multi-Frame Generation across all GPU vendors.
       - Presentation pacing verification (`0x224cf0`, `0x21ee30`, `0x224b30`).
       - Post-attach initialization hook in `OptiScaler/dllmain.cpp` (`XeMfgLoader::TryApply()`).
       - **Mandatory Native Thunk Pacing Engine (`XeFGPacing.h`)**:
         - `XeFGPacing::Install` must run unconditionally whenever XeMFG is patched (`outStatus.Patched == true`).
         - Never gate native thunk pacing behind optional DXGI `ExtraPacing` or config switches; without `XeFGPacing`, `libxess_fg.dll` dumps intermediate generated frames in < 1 ms unmetered bursts and collapses `frameRenderTime` to 0.0f, ruining visual smoothness.
         - Decoupled frame render time (`XeFGPacing::RenderTimeMs()`) must feed valid delta time to Intel in `XeFG_Dx12::Dispatch()`.
          - **End-to-End Burst Pacing & Anti-Clump Protection**:
            - Support 2X Frame Generation (`multiplier < 2` guard), pacing the single generated frame to the exact 50% halfway point ($T_{\text{base}} + T_{\text{interval}}$).
            - Unconditionally pace the final generated frame (`isLast == true`, caller `LastFrameCallerRva = 0x220467`), eliminating sub-millisecond frame dumps (0.28–0.47 ms).
            - **Strict Prohibition on Base Frame Thread Blocking (`NoteBasePresent()`)**:
              - In `libxess_fg.dll`, within each `xefgSwapChainPresent` call, callers `0x220A52` and `0x220DAD` represent the base application frame presenting synchronously on the game engine's main render thread.
              - The base application frame must **NEVER** be blocked with `WaitUntil`, sleep, or spin-wait. It must present immediately via `NoteBasePresent()` with **0 wait** and return control directly to the engine so the game renders at full native framerate (30–60+ FPS). Pacing must strictly apply to generated frames (`0x2202ED`, `0x220467`).
              - In `NoteBasePresent()`, `PushPeriod` must measure pure engine submission time by subtracting `burstBlockQpc` from `deltaNs`, completely preventing presentation delays from triggering the self-referential recursive feedback loop that collapsed frame rate to 9.2 FPS (108.55 ms).
              - `TsDetour` (RVA `0x3430`) must set uniform deadlines for generated frames: $\text{deadline}(i) = \text{base} + (i - 1) \times \text{step}$, resetting on every burst to prevent inter-cycle drift.
              - Anti-clump protection in `ApplyAntiClumpProtection` clamps consecutive presents to the minimum floor ($\ge 1.5\text{ ms}$) without stalling the main thread for whole frame intervals.
            - Fall back to `g_periodNs` in `NoteFrame` when inter-frame intervals collapse under 1 ms to prevent zeroed frame render time reporting.
        - **Multiplier Ceiling Uncap (`XeFG_Dx12.cpp`)**:
          - `_maxInterpolationCount` must be initialized to `std::max(_maxInterpolationCount, 5)` in `CreateSwapchain` / `CreateSwapchain1` when `XeMfgLoader::AnyEnabledForSession()`.
          - `SetInterpolatedFrameCount` and `Dispatch` must compute `effectiveMax` via `std::max(_maxInterpolationCount, XeMfgLoader::EffectiveMax(_maxInterpolationCount))` rather than stock ceiling 1, dynamically raising `_maxInterpolationCount` and synchronizing `XeMfgLoader::SetMaxGeneratedFrames(count)` live.
       - **Dynamic In-Memory Ceiling Adaptation**:
         - `XeMfgLoader::SetMaxGeneratedFrames` must support live in-memory byte rewriting of `U3`, `U4`, and `U5` with `VirtualProtect` and `FlushInstructionCache`. Never make the ceiling immutable for the session or lock out the menu slider.
         - `EnabledForSession()` must evaluate dynamically; never latch it with `static const bool`.
     - **Configuration Backend & Defaults**:
       - `Config.h`: `XeMfgUnlock` (defaults to `false` when unset / `auto`), `XeMfgMaxFrames` (defaults to 3 / 4X FG), `XeMfgExtraPacing` (defaults to `false`, controlling only the DXGI swapchain VBlank lock in `FG_Hooks.cpp`).
       - `Config.cpp`: Parsing and saving `UnlockMFG`, `MaxInterpolatedFrames`, and `ExtraPacing` under `[XeMFG]`.
       - `OptiScaler.ini`: `[XeMFG]` section with `UnlockMFG=auto` (resolves to `false`), `MaxInterpolatedFrames=3`, `ExtraPacing=false`.
     - **Pipeline Auto-Configuration & Swapchain Lifecycle (`OptiScaler/dllmain.cpp`, `Streamline_Hooks.cpp`)**:
       - When `XeMfgUnlock` is enabled and `!externalFrameGeneration`:
         `FGEnabled = true`, `FGInput = FGInput::DLSSG`, `FGOutput = FGOutput::XeFG`, and `FGNvngxReplacement = None`.
       - When Streamline receives `DLSSGMode::eOff`, cleanly set `XeFGProxy::SetEnabled(false)` on the swapchain context so Intel does not expect motion vectors or depth buffers while FG is disabled, preventing `-12` mismatch storms. Re-enable upon transitioning back to `eOn`.
     - **Streamline & NGX Capability Unlocking**:
       - `OptiScaler/proxies/NVNGX_Proxy.h`: Interception of NGX Feature 11 (`FrameGeneration`) across DX12, DX11, and Vulkan to report `Supported` with `MinHWArchitecture = 0`.
       - `OptiScaler/hooks/Streamline_Hooks.cpp`: Interposer hook attachment (`hkslIsFeatureSupported`, `hkslIsFeatureLoaded`, etc.), architecture spoofing (`shouldSpoofArch`), synthetic DLSSG state advertising (`hkslDLSSGGetState` with `EffectiveMax`), dynamic MFG support (`bIsDynamicMFGSupported = sl::Boolean::eTrue`), and multiplier routing (`hkslDLSSGSetOptions` -> `XeFG`).
       - `OptiScaler/NVNGX_Parameter.cpp`: Advertising `FrameGeneration.Available = 1`, `DLSSG.Available = 1`, and `DLSSG.MultiFrameCountMax = XeMfgLoader::EffectiveMax(1)`.
     - **In-Game Menu UI (`OptiScaler/menu/menu_common.cpp`)**:
       - Dedicated `Intel Xe Multi-Frame Generation (XeMFG)` subsection with toggle, multiplier slider, burst pacing checkbox, diagnostic badges (`Patches: active (5/5)`), and live multiplier synchronization (`In-Game Multiplier: %dX (Active)`).
       - Live slider adjustments must call `XeMfgLoader::SetMaxGeneratedFrames(maxFrames)` directly.
       - Strict mutual exclusion enforcement between Ada MFG, Ampere SM86 MFG, and XeMFG across config and menu toggles.
     - **Associated Automated Unit Tests**:
       - `tests/xemfg_loader_unit.cpp`
       - `tests/xefg_mfg_streamline_unit.cpp`
       - `tests/xefg_pacing_engine_unit.cpp`

9. **Preservation of Intel Xe Multi-Frame Generation for DLSS-SR (XeMFG-SR) Injector Across Upstream Merges**:
   - Upstream author (`wilsjo2`) does not maintain the XeMFG-SR injector pipeline for games with DLSS Super Resolution lacking native DLSS-FG or Streamline.
   - When merging changes from upstream `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`, XeMFG-SR components and all related work must **NEVER** be removed, overwritten, disabled, or regressed:
     - **Core Loader & Session Gating (`XeMfgLoader.h`, `XeMfgLoader.cpp`)**:
       - `XeMfgLoader::SrEnabledForSession()` evaluating dynamically: `XeMfgSrEnable && !XeMfgUnlock && !externalFrameGeneration && !Ada && !Ampere`.
       - `XeMfgLoader::AnyEnabledForSession()` uniting primary XeMFG and XeMFG-SR session states.
       - Dynamic target ceiling resolution in `XeMfgLoader::TryApply()` resolving to `XeMfgSrGeneratedFrames` (clamped 1 to 5) when in SR mode.
       - Dynamic in-memory ceiling adaptation: `XeMfgLoader::SetMaxGeneratedFrames` rewrites `U3`, `U4`, and `U5` byte targets live in `libxess_fg.dll`.
     - **Configuration Backend & Defaults**:
       - `Config.h`: `XeMfgSrEnable` (defaults to `false` when unset / `auto`), `XeMfgSrGeneratedFrames` (defaults to 2 / 3X FG, clamped 1 to 5).
       - `Config.cpp`: Parsing, legacy key migration (`[XeMFG] EnableSR`, `SrGeneratedFrames`), value clamping, and saving under `[XeMFG-SR]`.
       - `OptiScaler.ini`: Dedicated `[XeMFG-SR]` section with descriptive comments.
       - Strict universal mutual exclusion in `Config.cpp`: primary `XeMfgUnlock`, `AdaMfgUnlock`, `AmpereMfgUnlock`, and `ExternalFrameGeneration` strictly override and suppress `XeMfgSrEnable`.
     - **Automatic Pipeline Auto-Configuration (`OptiScaler/dllmain.cpp`)**:
       - When `XeMfgLoader::SrEnabledForSession()` is active without external FG:
         `FGEnabled = true`, `FGInput = FGInput::Upscaler`, `FGOutput = FGOutput::XeFG`, `FGNvngxReplacement = None`, and `FGXeFGInterpolationCount = srFrames`.
       - Post-attach initialization must invoke `fakenvapi::init(true)` early to ensure the low latency context is allocated before Reflex calls arrive.
     - **Reflex Hook SwapChain & Feature State Gating (`OptiScaler/hooks/Reflex_Hooks.cpp`)**:
       - Reflex hooks (`hkNvAPI_D3D_SetSleepMode`, `hkNvAPI_D3D_Sleep`, `hkNvAPI_D3D_GetLatency`, `hkNvAPI_D3D_SetLatencyMarker`, `hkNvAPI_D3D12_SetAsyncFrameMarker`, `setFPSLimit`) must **never** hijack game calls to `nvapi_calls` unless XeFG is fully initialized and actively presenting (`state.activeFgOutput == FGOutput::XeFG && state.currentFGSwapchain != nullptr && state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused()`).
       - When XeFG is inactive or when the swapchain has not yet been bound to XeFG, calls must smoothly fall through to native driver Reflex (`o_NvAPI_D3D_...`), preventing null dereferences on uninitialized contexts and ensuring games with native Reflex / DLSS-SR continue operating cleanly.
     - **Thread-Safe `LowLatencyCtx` Lazy-Initialization & Null Guards (`OptiScaler/nvapi/fakenvapi/`)**:
       - `LowLatencyCtx::get()` in `low_latency.h` must be thread-safe with `std::scoped_lock` on static mutex `s_mutex` and self-initializing (`if (!lowlatency_ctx) init();`), guaranteeing it never returns `nullptr`.
       - All 13 dispatch functions in `nvapi_calls.cpp` must null-guard `LowLatencyCtx::get()`, returning `ERROR()` on null context instead of dereferencing `nullptr + 8`.
     - **In-Game Menu Overlay UI (`OptiScaler/menu/menu_common.cpp`)**:
       - Dedicated `Intel Xe Multi-Frame Generation for DLSS-SR (XeMFG-SR)` collapsible subsection rendered before the `if (state.externalFrameGeneration || ...)` early return.
       - Interactive checkbox `"Enable XeMFG-SR Injector (Multi-Frame Generation; restart)##xemfg_sr"` auto-configuring the pipeline, initiating `fakenvapi::init(true)`, `XeFGProxy::InitXeFG()`, applying `XeMfgLoader::TryApply()`, and enforcing bidirectional mutual exclusion with other FG unlockers and Smooth Motion.
       - Dynamic slider `"Generated Frames##xemfg_sr"` (1 to 5 / 2X to 6X FG) dynamically updating `XeMfgLoader::SetMaxGeneratedFrames` and `currentFG->SetInterpolatedFrameCount`.
       - Status badge (`DLL: found | Patches: 5/5 | Pacing: active`), presentation telemetry from `xefgSwapChainGetLastPresentStatus`, and live multiplier indicator (`In-Game Multiplier: %dX (Active)`).
       - Diagnostic prompt when swapchain is unbound (`state.currentFGSwapchain == nullptr`): `"Note: Save Settings and restart the game to initialize the XeFG SwapChain."`.
       - Diagnostic checkboxes: `Debug Markers (Corners)`, `Only Interpolated Frames`, and `Display VBlank Sync (Extra Pacing)`.
     - **DXGI 1.2+ Window-Sized SwapChain Resolution (`DxgiSwapchainSizing.h`, `DxgiFactory_Hooks.cpp`, `DxgiFactory_WrappedCalls.cpp`)**:
       - `ResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC1& desc, HWND window)` must query `GetClientRect(window, &rect)` when `Width == 0 && Height == 0` and size the descriptor to the window client rect.
       - In `CreateSwapChainForHwnd` and `DLSSGCreateSwapChainForHwnd`, `ResolveWindowSizedSwapchain(localDesc, hWnd)` must execute on a local descriptor copy *before* the `< 100` overlay check.
       - Window-sized swapchains (e.g. *Helldivers 2* passing 0x0) must wrap normally via `FGHooks::CreateSwapChainForHwnd` and log resolution at `INFO` level, never being rejected as overlays.
       - The caller's `pDesc` must remain unmutated (preserving the 0x0 contract).
     - **Helldivers 2 Quirk & User Borderless Toggle (`Quirks.h`, `Config.cpp`, `OptiScaler.ini`, `menu_common.cpp`)**:
       - `QUIRK_ENTRY("helldivers2.exe", GameQuirk::ForceBorderlessWhenUsingXeFG)` in `Quirks.h` ensures *Helldivers 2* creates flip-model borderless swapchains under Linux and Windows without globally forcing borderless on other games.
       - `[XeMFG-SR] ForceBorderless = auto` in `OptiScaler.ini` and `Config.cpp` syncs with `FGXeFGForceBorderless`, and `"Force Borderless Window (Exclusive Fullscreen Workaround; restart)##xemfg_sr"` provides interactive user control in the GUI.
     - **Live Multiplier Adaptation Without Destructive Deactivations (`XeFG_Dx12.cpp`, `menu_common.cpp`)**:
       - `_framesToInterpolate` must be initialized from `Config::Instance()->FGXeFGInterpolationCount.value_or_default()` in `CreateSwapchain` and `CreateSwapchain1`, ensuring immediate multiplier alignment.
       - `state.WAR_xefgRequestFGToggle` must be guarded with `!XeMfgLoader::AnyEnabledForSession()`, and `state.fgChanged = true;` must never be set in the `Generated Frames##xemfg_sr` slider callback. Live adjustments via `SetNumInterpolatedFrames` must never call `Deactivate()`, `SetEnabled: false`, or pause presentation for 10 frames.
     - **FrameRenderTime Sanity Floor Fallback (`XeFG_Dx12.cpp`)**:
       - In `XeFG_Dx12::Dispatch`, if `frameRenderTime < 1.0` or `frameRenderTime < 0.25 * _ftDelta[fIndex]` when `_ftDelta[fIndex] > 0.0`, fallback to `_ftDelta[fIndex]`. This prevents feeding collapsed sub-millisecond deltas (from scheduler blocking subtraction in `XeFGPacing.h`) to Intel XeSS-FG, stabilizing optical flow motion vectors and eliminating judder.
     - **OptiFG Menu Auto-Management & Advanced Settings Suppression (`menu_common.cpp`)**:
       - When `xeMfgSrActive` is true, `Frame Generation (OptiFG)` must display the matching informational message `"Managed exclusively by the Intel Xe Multi-Frame Generation for DLSS-SR (XeMFG-SR) section above."` and completely hide manual OptiFG parameters and `Advanced OptiFG Settings`.
     - **Associated Automated Unit Tests**:
       - `tests/xemfg_sr_unit.cpp` (Tests 1–11)
       - `tests/xemfg_menu_exclusion_unit.cpp`
       - `tests/xemfg_sr_crash_fix_unit.cpp`
       - `tests/dxgi_swapchain_sizing_unit.cpp`
       - `tests/dxgi_window_size_smoke.cpp`
       - `tests/xefg_pacing_engine_unit.cpp`

