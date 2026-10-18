# Frame Generation: Direct NVNGX Parameter Dimension Corruption & Turing Spoofing Crash Resolution (GitHub Issue #54)

## 1. Overview & Environment

- **Target / User Environment (Reported in Issue #54)**:
  - **Operating System**: Windows 11 Home 64-bit (Build 10.0.26100.7309)
  - **Hardware**: NVIDIA GeForce RTX 2060 Laptop GPU (Turing TU106 / SM75, 6144 MiB VRAM, Device ID `0x1F11`)
  - **Driver**: NVIDIA Display Driver 617.14
  - **Game**: *Forza Horizon 5* (`forzahorizon6.exe`, Direct NVNGX integration, DirectX 12, no Streamline)
  - **Mod Version**: `NR-v0.9.51 OptiScaler-DLSSNR-PreSR-Multipass v0.9.51`
  - **Configuration**: `FrameGen.External: true`, `DLSSG.AmpereMfgUnlock: true`, `AmpereMfgRouter: auto`, `Dx12Upscaler: auto` (native DLSS SR)
- **Development & Verification Environment**:
  - **Operating System**: CachyOS Linux (Kernel `7.2.9-2-cachyos`, Arch-based rolling)
  - **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86, GA103, 16384 MiB VRAM)
  - **Driver**: NVIDIA Display Driver 580.178.04 / CUDA 13.0
- **Affected Subsystems**:
  - Direct NVNGX Parameter Passing (`OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp`, `OptiScaler/inputs/FG/DlssgParameterSanitizer.h`)
  - Native Upscaler Initialization (`OptiScaler/upscalers/dlss/DLSSFeature_Dx12.cpp`, `OptiScaler/upscalers/dlssd/DLSSDFeature_Dx12.cpp`, `OptiScaler/upscalers/ShaderPipeline_Dx12.h`)
  - Scoped Dimension Restoration (`OptiScaler/upscalers/ScopedNgxDimensionRestoration.h`)
  - Turing Architecture Spoofing Safety (`OptiScaler/framegen/dlssg/AmpereMfgLoader.h`, `OptiScaler/framegen/dlssg/AmpereMfgLoader.cpp`, `OptiScaler/nvapi/NvApiHooks.cpp`)

---

## 2. Reported Issue & Symptoms

As reported in GitHub issue #54 ([`ShyVortex/dlss-unlocked#54`](https://github.com/ShyVortex/dlss-unlocked/issues/54)):
- **Symptom**: In *Forza Horizon 5*, the game crashed immediately upon loading into gameplay when using **DLSS Super Resolution (Upscaling) + Frame Generation**.
- **Crucial Diagnostic Clue**: If the user selected **DLAA + Frame Generation**, the game ran smoothly and did **NOT** crash.
- **Log Analysis**:
  The user attached `OptiScaler.log` and `OptiScaler.ini`. The log terminated abruptly right after `NVSDK_NGX_D3D12_EvaluateFeature` was called for Feature 11 (`FrameGeneration`) following `NVSDK_NGX_D3D12_EvaluateFeature` for Feature 1 (`SuperSampling`):
  ```
  [2026-10-18 09:12:04.112] [info] Passthrough to native DLSS CreateFeature for feature 1
  [2026-10-18 09:12:04.115] [info] Native CreateFeature success, HandleId: 1
  [2026-10-18 09:12:04.120] [info] Created modded DLSSG feature with HandleId: 1000000
  [2026-10-18 09:12:04.150] [debug] DLSS-NR route: handle 1, NGX feature 1, upscaler 1, RR 0
  [2026-10-18 09:12:04.152] [debug] EvaluateFeature - Handle: 1000000, CmdList: 000001DC5290B180
  <CRASH / Driver Hang 0x887A0006 DXGI_ERROR_DEVICE_HUNG>
  ```

---

## 3. Issue Validity Assessment & Root Cause Autopsy

**Status: Confirmed Valid (Severe Architectural Parameter Corruption & Architecture Leak Identified).**

A deep architectural analysis and reverse engineering of the NVNGX dispatch pipeline and the `nvngx_dlssg.dll` binary revealed two cooperating failure mechanisms:

### Root Cause 1: Direct NVNGX Shared Parameter Dictionary Contamination
1. **Direct NVNGX vs Streamline Architecture**:
   In games utilizing NVIDIA Streamline, Streamline creates separate constant buffers and option payloads for `sl.dlss` (Super Resolution) and `sl.dlss_g` (Frame Generation).
   In **direct NVNGX games** (such as *Forza Horizon 5*, which has no Streamline DLLs), the game engine allocates a single `NVSDK_NGX_Parameter` dictionary via `NVSDK_NGX_D3D12_GetCapabilityParameters` and reuses this single dictionary across all feature creation and evaluation calls.
2. **`DLSSFeature::ProcessInitParams` In-Place Mutation**:
   When OptiScaler initializes native DLSS Super Resolution (`DLSSFeature::Init`), it invokes `ProcessInitParams(InParameters)`. In `ProcessInitParams`, OptiScaler computes the downscaled render dimensions:
   ```cpp
   // OptiScaler/upscalers/dlss/DLSSFeature.cpp:
   InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());   // e.g. 1280
   InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight()); // e.g. 720
   ```
   Because `InParameters` is a shared dictionary belonging to the game, `Width` was permanently overwritten with `1280` and `Height` with `720`.
3. **Binary Inspection of `nvngx_dlssg.dll`**:
   Disassembly and string analysis of `dlssg_for_sm86/sdli1995/dlssg_sm86/runtime/nvngx_dlssg.dll` revealed:
   - `"Width"`: 18 occurrences
   - `"Height"`: 18 occurrences
   - `"OutWidth"`: 0 occurrences
   - `"OutHeight"`: 0 occurrences
   - `"DLSSG.Backbuffer"`: 7 occurrences
   `nvngx_dlssg.dll` has **zero knowledge** of `OutWidth` or `OutHeight`. It relies exclusively on `Width` and `Height` matching the backbuffer extent.
4. **The Dimension Mismatch Crash**:
   When the game called `NVSDK_NGX_D3D12_EvaluateFeature` for Feature 11 (`FrameGeneration`), it passed `DLSSG.Backbuffer` with dimensions $1920 \times 1080$, while `InParameters` had `Width = 1280` and `Height = 720`.
   - `EndpointCore::ValidateApplicationResources` inside `nvngx_dlssg.dll` detected that the resource extent ($1920 \times 1080$) did not match the declared parameter extent ($1280 \times 720$).
   - When proceeding to optical flow calculation and interpolation compute dispatches, CUDA kernels attempted to access out-of-bounds coordinates or mismatched tile buffers, triggering an unrecoverable GPU driver timeout `0x887A0006` (`DXGI_ERROR_DEVICE_HUNG`).
5. **Why DLAA Worked**:
   When the user selected DLAA (Deep Learning Anti-Aliasing), render resolution equals display resolution ($1920 \times 1080$). Thus, `RenderWidth() == 1920` and `RenderHeight() == 1080`. `Width` in `InParameters` matched `DLSSG.Backbuffer` exactly, preventing the out-of-bounds dispatch and explaining why DLAA did not crash.

### Root Cause 2: Turing Architecture Spoofing Leak to Direct NGX Models
In the `sdli1995` v0.3.0–0.4.0 companion configuration:
- Setting `SpoofArchToGame` defaults to `1` when omitted in `dlssg_sm86.ini`.
- When `SpoofArchToGame=1` is active on Turing GPUs (`0x160` / SM75, such as the RTX 2060 Laptop in Issue #54), `_nvngx.dll` queries `NvAPI_GPU_GetArchInfo` and receives `0x190` (Ada Lovelace).
- In direct NVNGX games, native `nvngx_dlss.dll` attempts to load Ada Lovelace model weights and FP8 cubin kernels that contain instructions unsupported by SM75 hardware, triggering illegal instruction crashes (documented by sdli1995 as the Forza Horizon `FHC01` crash).

---

## 4. Applied Solution & Architectural Implementation

We resolved both failure vectors across four modular components without introducing runtime overhead or breaking backward compatibility:

### 1. Scoped Parameter Restoration (`ScopedNgxDimensionRestoration.h`)
To prevent native upscaler initialization and dispatch from leaking internal render dimensions into shared parameter maps:
- Created [`OptiScaler/upscalers/ScopedNgxDimensionRestoration.h`](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/upscalers/ScopedNgxDimensionRestoration.h):
  ```cpp
  class ScopedNgxDimensionRestoration
  {
    private:
      NVSDK_NGX_Parameter* _parameters = nullptr;
      unsigned int _width = 0, _height = 0, _outWidth = 0, _outHeight = 0;
      bool _hasWidth = false, _hasHeight = false, _hasOutWidth = false, _hasOutHeight = false;
    public:
      explicit ScopedNgxDimensionRestoration(NVSDK_NGX_Parameter* inParams);
      ~ScopedNgxDimensionRestoration();
  };
  ```
- Integrated `ScopedNgxDimensionRestoration restoreDimensions(InParameters);` into:
  - `DLSSFeature_Dx12.cpp`, `DLSSDFeature_Dx12.cpp`
  - `DLSSFeature_Dx11.cpp`, `DLSSDFeature_Dx11.cpp`
  - `DLSSFeature_Vk.cpp`, `DLSSDFeature_Vk.cpp`
- Enhanced `RestoreUpscalerResources_Dx12` in `OptiScaler/upscalers/ShaderPipeline_Dx12.h` to capture and restore `Width`, `Height`, `OutWidth`, and `OutHeight` across `IFeature_Dx12::Evaluate` dispatches.

### 2. DLSS-G Create & Evaluate Dimension Sanitization (`DlssgParameterSanitizer.h` & `NVNGX_DLSS_Dx12.cpp`)
To guarantee that `nvngx_dlssg.dll` always observes the genuine display/backbuffer extent even if a game or third-party hook corrupted `Width`/`Height`:
- Enhanced [`OptiScaler/inputs/FG/DlssgParameterSanitizer.h`](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/inputs/FG/DlssgParameterSanitizer.h) with dimension forcing (`_forcedWidth`, `_forcedHeight`, `IsWidthKey`, `IsHeightKey`, and `ResolveDlssgSanitization`).
- When `Width < targetWidth` or `Height < targetHeight`, `DlssgParameterSanitizer` transparently overrides `Get("Width", ...)` and `Get("Height", ...)` across all integer and floating-point types (`unsigned int`, `int`, `float`, `double`, `unsigned long long`), returning the verified display/backbuffer extent.
- In [`OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp`](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp):
  - In `NVSDK_NGX_D3D12_CreateFeature`: for `InFeatureID == NVSDK_NGX_Feature_FrameGeneration`, resolves fallback display extent from `state.currentFeature->DisplayWidth()` or `state.screenWidth` and wraps `effectiveParameters` with `DlssgParameterSanitizer`.
  - In `NVSDK_NGX_D3D12_EvaluateFeature`: for `feature == NVSDK_NGX_Feature_FrameGeneration`, extracts exact backbuffer dimensions from `DLSSG.Backbuffer` descriptor (`backbuffer->GetDesc()`) and applies sanitization.

### 3. Turing Architecture Spoofing Safety (`AmpereMfgLoader.h` & `NvApiHooks.cpp`)
To prevent direct NGX Super Resolution models on Turing hardware from loading Ada cubins:
- In [`OptiScaler/framegen/dlssg/AmpereMfgLoader.h`](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/framegen/dlssg/AmpereMfgLoader.h):
  - Updated `FormatIniContent030` to accept `uint32_t realArch`.
  - When `spoofArch == "Auto"` (or unset) on Turing hardware (`realArch == NV_GPU_ARCHITECTURE_TU100` / `0x160`): strictly outputs `SpoofArchToGame=0` while preserving explicit user overrides (`"1"` / `"0"`) as escape hatches.
- In [`OptiScaler/framegen/dlssg/AmpereMfgLoader.cpp`](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/framegen/dlssg/AmpereMfgLoader.cpp): passed `gpu.nvidiaArchInfo.architecture_id` into `FormatIniContent030`.
- In [`OptiScaler/nvapi/NvApiHooks.cpp`](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/OptiScaler/nvapi/NvApiHooks.cpp):
  - Added `allowNvngxSpoof = (realArch != NV_GPU_ARCHITECTURE_TU100)` guard in `hkNvAPI_GPU_GetArchInfo`, ensuring `_nvngx.dll` callers on Turing hardware always receive the genuine physical architecture (`0x160`).

---

## 5. Verification & Automated Unit Testing

All components were verified using dedicated unit tests in [`tests/mfg_parameter_sanitization_unit.cpp`](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/tests/mfg_parameter_sanitization_unit.cpp):

```
=== Running DLSS-G Parameter Sanitization Unit Tests ===
  [PASS] Test_ShouldFilterDlssgOutputReal_NullParameters
  [PASS] Test_ShouldFilterDlssgOutputReal_KeyNotPresent
  [PASS] Test_ShouldFilterDlssgOutputReal_NonNullResource
  [PASS] Test_ShouldFilterDlssgOutputReal_NullResource
  [PASS] Test_DlssgParameterSanitizer_FilteredGet
  [PASS] Test_DlssgParameterSanitizer_UnfilteredGet
  [PASS] Test_DlssgParameterSanitizer_SetForwarding
  [PASS] Test_Simulation_Streamline_MultiPass_MFG
  [PASS] Test_ResolveDlssgSanitization_UpscalerDimensionMismatch
  [PASS] Test_ResolveDlssgSanitization_DirectNgxBackbufferFallback
  [PASS] Test_ResolveDlssgSanitization_DlaaNoMismatch
  [PASS] Test_ScopedNgxDimensionRestoration_Isolation
  [PASS] Test_AmpereMfgLoader_TuringSpoofArchResolution
=== All DLSS-G Parameter Sanitization Unit Tests PASSED! ===
```

Additional full-suite test results:
- `tests/dlssg_sm86_v030_ini_unit.cpp`: 16/16 tests PASS.
- `tests/ampere_mfg_mode_config_unit.cpp`: 13/13 tests PASS.
- `tests/turing_menu_recognition_unit.cpp`: 7/7 tests PASS.
- `tests/turing_dual_runtime_unit.cpp`: 7/7 tests PASS.
- `tests/streamline_turing_spoof_unit.cpp`: 6/6 tests PASS.
- `tests/ngx_parameter_dlssg_unit.cpp`: 8/8 tests PASS.
- C++ & MSVC compliance: All modified `.cpp` and `.h` files verified with mandatory UTF-8 BOM (`\xef\xbb\xbf`) and clean `clang-format --dry-run --Werror`.
