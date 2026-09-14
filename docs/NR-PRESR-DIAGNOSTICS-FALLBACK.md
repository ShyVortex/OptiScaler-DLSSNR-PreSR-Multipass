# DLSS Neural Rendering: Pre-SR Defaults, Diagnostics & Robust Buffer Fallback

## Problem Statement & Root Cause Analysis
Users reported that DLSS Neural Rendering (DLSS-NR) failed to activate when clicking "Enable Neural Rendering" in the OptiScaler GUI unless "Generate model before upscale" (`RunBeforeSR=true`) was also checked, presenting either:
1. `"Waiting for the upscaler to run."` (silent pipeline drop / infinite wait)
2. `"the NVIDIA NGX driver could not create Neural Rendering"` (NGX driver rejection at display resolution)

### Root Causes
1. **Unsynchronized Default Configuration**:
   - `OptiScaler.ini` defaulted `RunBeforeSR = auto` and `Config.h` defaulted `DlssNrRunBeforeSr = false`. When users checked "Enable Neural Rendering", it ran in Post-Upscale mode by default.
2. **Post-Upscale Buffer Allocation Silent Drop**:
   - In `DlssNr_Dx12::CreateBufferResource`, intermediate buffers were allocated with hardcoded `D3D12_HEAP_TYPE_DEFAULT` and `D3D12_HEAP_FLAG_NONE`, without querying the source texture's heap properties. Under Proton/VKD3D or swapchains with conflicting flags (e.g. `ALLOW_DEPTH_STENCIL` or typeless formats), committed resource creation failed with `E_INVALIDARG`.
   - `MakeDlssNrPass` in `DlssNr_Pipeline_Dx12.cpp` failed silently without publishing an error or setting `nr.reason`, causing the pass to be omitted from `ShaderPipeline_Dx12`. Because the pass never ran, `modelRunning` stayed `false` and the GUI was stuck on `"Waiting for the upscaler to run."`.
3. **NGX Driver Display Resolution Rejection**:
   - When post-upscale buffer creation succeeded at high display resolutions (e.g. 1440p/4K), `_nvngx.dll` driver feature creation failed because DLSS-NR model feature 18 is designed and trained for render resolutions. The resulting error was opaque and lacked a direct resolution path.

---

## Architectural Changes & Components

### 1. Pre-SR Placement Defaults & Menu Synchronization
- **`OptiScaler/Config.h`**: Defaulted `DlssNrRunBeforeSr` to `true`.
- **`OptiScaler.ini`**: Documented and mapped `RunBeforeSR = auto` to evaluate to `true` (Pre-SR).
- **`OptiScaler/dlssnr/DlssNr_Menu.cpp`**: Synchronized checkbox toggling so checking "Enable Neural Rendering" initializes `DlssNrRunBeforeSr = true` when unset while honoring explicit overrides.
- **Unit Test**: `tests/nr_placement_config_unit.cpp`.

### 2. Pipeline Setup Diagnostics & Skip Reporting
- **`OptiScaler/shaders/dlssnr/DlssNr_Dx12.h` & `.cpp`**: Added `DlssNr_Dx12::ReportPipelineSkip(const char* reason)`.
- **`OptiScaler/dlssnr/DlssNr_Pipeline_Dx12.cpp`**: Added explicit diagnostic checks and `ReportPipelineSkip` calls when buffer allocation fails, motion/depth guides are missing, compute shaders are uninitialized, or subrects are unsupported.
- **`OptiScaler/shaders/dlssnr/DlssNr_Dx12_Status.cpp`**: Updated `State::Publish` so non-empty `nr.reason` is exposed in status telemetry even before the shader dispatches.
- **`OptiScaler/upscalers/IFeature_Dx12.cpp`**: Added debug/warn logs around pipeline scheduling.
- **Unit Test**: `tests/nr_pipeline_setup_unit.cpp`.

### 3. Robust Buffer Creation & Heap Querying
- **`OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`**:
  - Normalized typeless formats using `_state->TypedGuideFormat(desc.Format)`.
  - Stripped conflicting flags (`ALLOW_DEPTH_STENCIL` and `DENY_SHADER_RESOURCE`) while ensuring `ALLOW_UNORDERED_ACCESS`.
  - Added heap querying via `source->GetHeapProperties(&heapProps, &heapFlags)`.
  - Added automatic fallback to clean `D3D12_HEAP_TYPE_DEFAULT` if custom heap properties fail committed allocation.
  - Added detailed diagnostic logging on success and failure.
- **Unit Test**: `tests/nr_buffer_resource_unit.cpp`.

### 4. Contextual Driver Diagnostics & One-Click Pre-SR Switch
- **`OptiScaler/shaders/dlssnr/DlssNr_Dx12_Models.cpp`**: When NGX driver feature creation fails in post-upscale mode, reports actionable guidance:
  `"the NVIDIA NGX driver could not create Neural Rendering at display resolution (try enabling 'Generate model before upscale' or reducing Working Scale)"`.
- **`OptiScaler/dlssnr/DlssNr_MenuPlacement.cpp`**: In `RenderStatus`, added an interactive quick-action button:
  `[ Switch to Pre-SR (Generate model before upscale) ]`, which immediately toggles `DlssNrRunBeforeSr = true` and invokes `DlssNr::RetryAfterFailure()`.
- **Unit Test**: `tests/nr_status_reporting_unit.cpp`.

---

## Upstream Integration & Preservation Protocol
When integrating commits from upstream `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`:
1. Verify that `DlssNrRunBeforeSr` remains defaulted to `true`.
2. Ensure `ReportPipelineSkip` and `CreateBufferResource` heap query/fallback logic are not overwritten by upstream commits.
3. Keep the quick-action button in `DlssNr_MenuPlacement.cpp` and contextual error strings in `DlssNr_Dx12_Models.cpp`.
4. Compile and run all 4 test units:
   ```bash
   g++ -std=c++20 -O2 tests/nr_placement_config_unit.cpp -o tests/nr_placement_config_unit && ./tests/nr_placement_config_unit
   g++ -std=c++20 -O2 tests/nr_pipeline_setup_unit.cpp -o tests/nr_pipeline_setup_unit && ./tests/nr_pipeline_setup_unit
   g++ -std=c++20 -O2 tests/nr_buffer_resource_unit.cpp -o tests/nr_buffer_resource_unit && ./tests/nr_buffer_resource_unit
   g++ -std=c++20 -O2 tests/nr_status_reporting_unit.cpp -o tests/nr_status_reporting_unit && ./tests/nr_status_reporting_unit
   ```
