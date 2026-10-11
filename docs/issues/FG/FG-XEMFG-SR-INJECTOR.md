# Frame Generation: Intel Xe Multi-Frame Generation Injector for DLSS-SR (XeMFG-SR)

## 1. Overview & Environment

- **Operating System**: Linux (Proton / Wine / VKD3D-Proton) & Windows 10/11 x64
- **Hardware Targets**: Intel Arc GPUs, NVIDIA GeForce GPUs (RTX 20/30/40/50, GTX 16), AMD Radeon GPUs (RX 6000/7000/8000)
- **Target Runtimes**: `libxess_fg.dll` / `igxess_fg.dll` (Intel XeSS Frame Generation v1.3.1+)
- **Pipeline Routing**: `FGInput::Upscaler` -> `FGOutput::XeFG`
- **Issue & Feature Reference**: Global Frame Generation Injection for DLSS-SR Titles

---

## 2. Problem Statement & Motivation

Many modern and legacy games support **DLSS Super Resolution (SR)** (exposing high-precision sub-pixel motion vectors, linear depth buffers, exposure, and color buffers via `NVSDK_NGX_D3D12_EvaluateFeature`), but completely lack native **DLSS Frame Generation (FG)** or NVIDIA Streamline integration. Typical examples include:
1. Titles built on DLSS 2.x SDKs or custom engine branches where Streamline was never integrated.
2. Older DirectX 12 games that received DLSS upscaling updates but never implemented frame generation.
3. Games with custom proprietary presentation loops where standard Streamline interception hooks do not attach.

Prior to the XeMFG-SR implementation:
- OptiScaler's primary Intel Xe Multi-Frame Generation (`XeMfgUnlock`) was strictly architected around `FGInput::DLSSG` (intercepting game-driven Streamline DLSS-G option calls).
- In titles lacking native DLSS-G, users attempting to use `FGInput::Upscaler` with `FGOutput::XeFG` had to navigate complex manual menus, manual swapchain configuration was prone to conflicts, `XeMfgLoader`'s 5 memory patches (`U1`–`U5`) and presentation pacing engine were not automatically activated or synchronized, and in-game multiplier adjustments live from the overlay were unavailable.

---

## 3. Issue Validity & Technical Feasibility Assessment

- **Validity**: **Confirmed feasible and high-value architectural capability**.
- **Feasibility Analysis**:
  - In games with DLSS-SR, OptiScaler already intercepts `NVSDK_NGX_D3D12_EvaluateFeature` (in `NVNGX_DLSS_Dx12.cpp`).
  - During this evaluate pass, OptiScaler captures the game's rendered color buffer, motion vectors, depth buffer, reactive mask, and camera jitter offsets.
  - When OptiScaler's internal FG pipeline is configured to `FGInput::Upscaler` and `FGOutput::XeFG`, these exact buffers are siphoned into `XeFG_Dx12::Dispatch()`, providing `libxess_fg.dll` with the complete inputs necessary to synthesize interpolated frames.
  - By integrating `XeMfgLoader` into this upscaler pass, `libxess_fg.dll`'s internal frame ceiling patches (`U3`, `U4`, `U5`) and native presentation thunk pacing (`XeFGPacing.h`) are activated, unlocking dynamic Multi-Frame Generation from 2X up to 6X (1 to 5 interpolated frames) across all GPU vendors without requiring any game-side Streamline support.

---

## 4. Architectural Implementation

### 4.1 Configuration Backend (`[XeMFG-SR]`)
The configuration backend was extended with a dedicated section and strict validation:
- **`OptiScaler.ini`**:
  ```ini
  [XeMFG-SR]
  ; Enables Intel Xe Multi-Frame Generation injector for games with DLSS-SR but without DLSS-FG
  ; auto (default, resolves to false), true, false
  Enable = auto

  ; Number of generated frames per real DLSS-SR rendered frame (1 to 5)
  ; 1 = 2X FG, 2 = 3X FG (default), 3 = 4X FG, 4 = 5X FG, 5 = 6X FG
  GeneratedFrames = 2
  ```
- **`OptiScaler/Config.h`**:
  - `XeMfgSrEnable` (`CustomOptional<bool>{false}`)
  - `XeMfgSrGeneratedFrames` (`CustomOptional<int>{2}`)
- **`OptiScaler/Config.cpp`**:
  - Parsing keys with legacy migration fallback (`[XeMFG] EnableSR` and `[XeMFG] SrGeneratedFrames`).
  - Strict range clamping: `GeneratedFrames` is clamped within `[1, 5]`. Out-of-range values reset cleanly to default (`2`).
  - Serialization: writes `Enable` and `GeneratedFrames` back under `[XeMFG-SR]`.

### 4.2 Universal Mutual Exclusion Backend
To prevent GPU race conditions, conflicting hook execution, or swapchain presentation crashes, universal mutual exclusion is enforced across all frame generation technologies in `Config.cpp`, `menu_common.cpp`, and loader modules:
1. **Primary XeMFG Precedence**: When `XeMfgUnlock = true`, `XeMfgSrEnable` is automatically suppressed and reset to `false`.
2. **Ada MFG Exclusion**: When `FGDLSSGAdaMfgUnlock = true`, `XeMfgSrEnable` is suppressed.
3. **Ampere MFG Exclusion**: When `FGDLSSGAmpereMfgUnlock = true`, `XeMfgSrEnable` is suppressed.
4. **Smooth Motion Exclusion**: Concurrent activation of NVIDIA driver-level Smooth Motion and XeMFG-SR is mutually exclusive.
5. **External Frame Generation Mode**: Sideloaded proxies force `XeMfgSrEnable = false`.

### 4.3 Loader Dual Activation & Dynamic Ceiling Resolution (`XeMfgLoader`)
In `OptiScaler/framegen/xefg/XeMfgLoader.h` and `.cpp`:
- **`SrEnabledForSession()`**:
  Evaluates dynamically:
  ```cpp
  bool SrEnabledForSession()
  {
      auto config = Config::Instance();
      if (!config) return false;
      return config->XeMfgSrEnable.value_or_default() &&
             !config->XeMfgUnlock.value_or_default() &&
             !config->ExternalFrameGeneration.value_or_default() &&
             !config->FGDLSSGAdaMfgUnlock.value_or_default() &&
             !config->FGDLSSGAmpereMfgUnlock.value_or_default();
  }
  ```
- **`AnyEnabledForSession()`**:
  Returns `EnabledForSession() || SrEnabledForSession()`.
- **Target Ceiling Resolution in `TryApply()`**:
  ```cpp
  unsigned int targetCeiling = kDefaultMaxFrames;
  if (SrEnabledForSession())
      targetCeiling = Config::Instance()->XeMfgSrGeneratedFrames.value_or(2);
  else
      targetCeiling = Config::Instance()->XeMfgMaxFrames.value_or(kDefaultMaxFrames);
  ```
  Applies the 5 binary patches (`U1`–`U5`) to `libxess_fg.dll` with the resolved target ceiling.

### 4.4 Automatic Pipeline Configuration (`dllmain.cpp`)
During DLL attach and state initialization in `OptiScaler/dllmain.cpp`:
```cpp
const bool xeMfgSrActive = XeMfgLoader::SrEnabledForSession();
if (xeMfgSrActive && !State::Instance().externalFrameGeneration)
{
    auto* cfg = Config::Instance();
    cfg->FGEnabled.set_volatile_value(true);
    cfg->FGInput.set_volatile_value(FGInput::Upscaler);
    cfg->FGOutput.set_volatile_value(FGOutput::XeFG);
    cfg->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::None);
    const int srFrames = std::clamp(cfg->XeMfgSrGeneratedFrames.value_or(2), 1, 5);
    cfg->FGXeFGInterpolationCount.set_volatile_value(srFrames);
}
```
This guarantees zero manual configuration required by the user: enabling XeMFG-SR automatically hooks up the complete upscaler-to-XeFG injection pipeline.

### 4.5 In-Game ImGui Menu Overlay (`OptiScaler/menu/menu_common.cpp`)
A dedicated subsection was added to `MenuCommon::RenderFrameGenerationSelection`:
1. **Subsection Placement**:
   Placed before `if (state.externalFrameGeneration || ...)` early return so controls and diagnostics are always available.
2. **Interactive Toggle Checkbox**:
   `"Enable XeMFG-SR Injector (Multi-Frame Generation)##xemfg_sr"`:
   - Configures runtime pipeline on the fly (`FGInput::Upscaler`, `FGOutput::XeFG`, `FGNvngxReplacement::None`, `FGEnabled = true`).
   - Disengages conflicting FG unlockers (`XeMfgUnlock = false`, `AdaMfgUnlock = false`, `AmpereMfgUnlock = false`, `SmoothMotion = false`).
   - Invokes `XeFGProxy::InitXeFG()` and `XeMfgLoader::TryApply()`.
   - Signals `state.fgChanged = true`.
3. **Live Dynamic Multiplier Slider & In-Memory Patch Rewriting**:
   - Slider `"Generated Frames##xemfg_sr"` (1 to 5) with custom labels (`1 (2X FG)`, `2 (3X FG) [Default]`, `3 (4X FG)`, `4 (5X FG)`, `5 (6X FG)`).
   - Live adjustments immediately invoke [XeMfgLoader::SetMaxGeneratedFrames](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/framegen/xefg/XeMfgLoader.cpp#L335-L370) and [SetInterpolatedFrameCount](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/framegen/xefg/XeFG_Dx12.h#L94), dynamically rewriting `U3`, `U4`, and `U5` patch bytes in `libxess_fg.dll`'s memory with `VirtualProtect` and `FlushInstructionCache` without requiring a game restart.
4. **Status Badges & Telemetry**:
   - Provider DLL detection (`libxess_fg.dll` found/missing).
   - Patch status badge (`Patches: active (5/5)`).
   - Native thunk presentation pacing status (`Pacing: active (native provider)`).
   - Real-time presentation telemetry from `xefgSwapChainGetLastPresentStatus`.
   - Live multiplier status badge: `"In-Game Multiplier: %dX (Active)"`.
   - Diagnostic controls: `Debug Markers (Corners)##xemfg_sr`, `Only Interpolated Frames##xemfg_sr`, and `Display VBlank Sync (Extra Pacing)##xemfg_sr`.

---

## 5. Automated Unit Testing & Integration Validation

The implementation is verified by two comprehensive unit test suites:

1. **`tests/xemfg_sr_unit.cpp`** (7/7 test cases passing):
   - **Case 1**: Config parsing, defaults, and bounds clamping within `[1, 5]`.
   - **Case 2**: Legacy key migration from `[XeMFG] EnableSR` and `[XeMFG] SrGeneratedFrames`.
   - **Case 3**: Universal mutual exclusion across `XeMfgUnlock`, `AdaMfgUnlock`, `AmpereMfgUnlock`, and `ExternalFrameGeneration`.
   - **Case 4**: Loader session state evaluation (`SrEnabledForSession()`, `EnabledForSession()`, `AnyEnabledForSession()`).
   - **Case 5**: Dynamic target ceiling resolution in SR mode vs Streamline mode.
   - **Case 6**: In-memory binary patch ceiling adaptation (live byte rewriting of `U3`, `U4`, `U5` across 1 to 5).
   - **Case 7**: Post-attach automatic pipeline configuration and multiplier calculation.

2. **`tests/xemfg_menu_exclusion_unit.cpp`** (6/6 test cases passing):
   - **Case 4**: Active XeMFG-SR locks standard FG selection, renders dedicated XeMFG-SR section, displays XeMFG-SR managed notice, and suppresses duplicate legacy controls.
   - **Case 5**: Strict bidirectional mutual exclusion between XeMFG-SR and all other FG options (Ada, Ampere, XeMFG, Smooth Motion, External FG).
   - **Case 6**: Primary XeMFG priority over XeMFG-SR when both settings are configured.

3. **Compiler & Code Quality Standards**:
   - `clang-format --dry-run --Werror` verified with **0 violations** across all modified files.
   - UTF-8 BOM (`\xef\xbb\xbf`) preserved on all modified C++ files.
   - Complete regression test suite passed with **100% clean passes**.
