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
       - Instruction-level patching of `cmp eax/reg, 0x1b0` -> `0x190` across all executable sections of `sl.dlss_g.dll` and `nvngx_dlssg.dll`.
       - Decoupled `UnlockedMax()` so arch-gate success returns 5 even if kernel rewrite is skipped or returns 0 on newer Streamline 2.14 / v310.9+ DLLs.
     - **NGX Parameter Advertising (`NVNGX_Parameter.cpp`)**:
       - `NVSDK_NGX_Parameter_Set_FG` must advertise `DLSSG.MultiFrameCountMax = 5` when `MfgUnlock::EnabledForSession()` is true, while strictly maintaining Ampere mutual exclusion.
       - Populate `State::Instance().dlssgMfgMax = 5` so the ImGui menu ratio override combo box is unlocked for 2X-6X across all titles.
     - **Streamline Option Clamping Guard & Pacing (`Streamline_Hooks.cpp`)**:
       - `hkslDLSSGSetOptions` and `hkslDLSSGGetState` must default `state.dlssgMfgMax` to 5 when Ada MFG is active.
       - Volatile clamping of `FGDLSSGOverrideInterpolationCount` to `state.dlssgMfgMax` must be guarded so 3X/4X overrides are not clamped to 1.
       - Version 4 `sl::DLSSGState` initialized in `hkslDLSSGGetState` and `numFramesToGenerateMax` elevated to 5.
       - Reflex pacing synchronized via `ReflexHooks::setDlssgFrameCount(overrideCount)` for any override count.
     - **NGX Evaluate & Reflex Synchronization (`NVNGX_DLSS_Dx12.cpp`)**:
       - When `feature == NVSDK_NGX_Feature_FrameGeneration`, `FGDLSSGOverrideInterpolationCount` must be written to `InParameters->Set("DLSSG.MultiFrameCount", frameCount)` before passing to `NVNGXProxy::D3D12_EvaluateFeature`.
       - Sync `ReflexHooks::setDlssgFrameCount(frameCount)` and `State::Instance().dlssgDetectedInterpolationCount`.
     - **Associated Unit Tests**:
       - `tests/ngx_parameter_dlssg_unit.cpp`
       - `tests/mfg_unlock/run.ps1` (production-backed transaction and module-lifetime tests)
       - `tests/streamline_mfg_options_unit.cpp`
       - `tests/dlssg_evaluate_feature_unit.cpp`

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
