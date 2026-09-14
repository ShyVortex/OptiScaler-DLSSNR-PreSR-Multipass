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
     - Config declarations, parsing, saving, and defaults (`Config.h`, `Config.cpp`, `OptiScaler.ini`)
     - Linux 2X FG FSR Fallback (`AmpereMfgLoader::ShouldFallbackToFsrFg`, `dllmain.cpp`, `menu_common.cpp`)
     - NVAPI hooks (`hkNvAPI_D3D12_SetFlipConfig`, DRS overrides in `NvApiHooks.cpp`) and Kernel hook bypasses (`Kernel_Hooks.cpp`)
     - Menu UI section, sliders, and tonemapped status indicators (`menu_common.cpp`)
     - Packaging and build integration (`OptiScaler.vcxproj`, `OptiScaler.vcxproj.filters`, `package_release.ps1`, workflows)
     - Associated tests (`tests/dlssg_sm86_ini_smoke.cpp`, `tests/kernel_hooks_nvngx_dlssg_unit.cpp`)
   - Always preserve **Ada (RTX 40) MFG Unlocker**:
     - `OptiScaler/framegen/dlssg/MfgUnlock.h` and `MfgUnlock.cpp`
     - Build flag `OptiScalerRtx40Mfg` must default to `true` in `OptiScaler.vcxproj` (defines `OPTISCALER_RTX40_MFG`)
     - Config declarations (`FGDLSSGAdaMfgUnlock`, `FGDLSSGAdaBlackwellKernels` in `Config.h`, `Config.cpp`)
     - All `#if defined(OPTISCALER_RTX40_MFG)` guarded code in `DLSSG_Dx12.cpp`, `Streamline_Hooks.cpp`, `LibraryLoad_Hooks.cpp`, `menu_common.cpp`
     - INI entry `AdaMfgUnlock` in `OptiScaler.ini` (must not be stripped during packaging)

5. **Permanent Workflow Branches & Integration Lifecycle**:
   - **`merge-upstream` (Permanent Branch)**:
     - Dedicated strictly to pulling and integrating upstream commits from `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`.
     - Must always maintain both the SM75–SM86 mod and the Ada MFG unlocker, configs, and Linux fallbacks without regression.
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