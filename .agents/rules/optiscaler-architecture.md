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

4. **Preservation of SM75-SM86 MFG Unlocker Across Upstream Merges**:
   - Upstream author (`wilsjo2`) removed / omitted the SM75-SM86 (Turing/Ampere) MFG unlocker in subsequent commits.
   - When merging changes from upstream, the SM75-SM86 component and all related work must **NEVER** be removed, overwritten, disabled, or regressed.
   - Always preserve:
     - `OptiScaler/framegen/dlssg/AmpereMfgLoader.h` and `AmpereMfgLoader.cpp`
     - Config declarations, parsing, saving, and defaults (`Config.h`, `Config.cpp`, `OptiScaler.ini`)
     - Linux 2X FG FSR Fallback (`AmpereMfgLoader::ShouldFallbackToFsrFg`, `dllmain.cpp`, `menu_common.cpp`)
     - NVAPI hooks (`hkNvAPI_D3D12_SetFlipConfig`, DRS overrides in `NvApiHooks.cpp`) and Kernel hook bypasses (`Kernel_Hooks.cpp`)
     - Menu UI section, sliders, and tonemapped status indicators (`menu_common.cpp`)
     - Packaging and build integration (`OptiScaler.vcxproj`, `OptiScaler.vcxproj.filters`, `package_release.ps1`, workflows)
     - Associated tests (`tests/dlssg_sm86_ini_smoke.cpp`, `tests/kernel_hooks_nvngx_dlssg_unit.cpp`)

5. **Permanent Workflow Branches & Integration Lifecycle**:
   - **`merge-upstream` (Permanent Branch)**:
     - Dedicated strictly to pulling and integrating upstream commits from `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`.
     - Must always maintain the SM75–SM86 mod, configs, and Linux fallbacks without regression.
   - **`dlssg-sm86` (Permanent Branch)**:
     - Dedicated to inspecting newer versions of the `dlssg_for_sm86` mod (by sdli1995), analyzing binary/INI differences, and updating OptiScaler loader/hooking integration.
   - **Standard 2-Step Workflow Sequence**:
     1. **Step 1 (Upstream Ingestion)**: Merge new commits from `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass` into `merge-upstream`. Verify integration correctness, compile tests (`dlssg_sm86_ini_smoke`), and ensure CI builds pass.
     2. **Step 2 (Mod Synchronization & Main Promotion)**:
        - Merge `merge-upstream` into `dlssg-sm86`.
        - If there are updates to `dlssg_for_sm86`, inspect differences, adapt `AmpereMfgLoader` / INI generation, test, and then merge `dlssg-sm86` into `main`.
        - If there are no updates to the mod, merge `merge-upstream` directly into `main`.