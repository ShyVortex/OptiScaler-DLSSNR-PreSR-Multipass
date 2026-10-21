# DLSS-NR: MSFS 2024 Pre-SR Parameter Resolution, Multi-Mip Guide Support & Evaluate-Time Fallback

## Overview & Verified Environment

In **Microsoft Flight Simulator 2024** (`FlightSimulator2024.exe`), users reported in GitHub Issue [#49](https://github.com/ShyVortex/dlss-unlocked/issues/49) that DLSS Neural Rendering (DLSS-NR) appeared to have no observable visual effect in-game from **v0.9.32 up to v0.9.50**, despite visibly functioning in **v0.9.9** and OptiScaler logging an active ~4.25 ms GPU workload.
An initial patch in commit `0fe693dc` (v0.9.51) attempted to resolve render dimensions by reading `NVSDK_NGX_Parameter_Width` and `NVSDK_NGX_Parameter_Height` from `InParameters`. However, upon testing a clean install of v0.9.51, users reproduced the defect identically. Furthermore, when users tested the "Finished Picture" (Post-SR) fallback option, OptiScaler rejected the buffers with `"DLSS-NR finished picture: The game's depth or movement data is not supported"`.

This document details the exhaustive root cause analysis, the discovery of the synthetic mock test blindspot, the NGX SDK evaluate-time parameter contract, the multi-mip guide support implementation, and the automated test verification.

### Verified Environment Details
- **Reported Platform**: Windows 11 Build 26100.3200 (x64)
- **Reported GPU**: NVIDIA GeForce RTX 5090 (Blackwell Architecture, SM100) / RTX 4090 (Ada Lovelace, SM89)
- **Reported Driver**: NVIDIA GeForce Game Ready Driver 617.42 / 617.07
- **Verification Environment**: Linux 7.2.11-1-cachyos (x86_64), GCC 15.2 / Clang 21, NVIDIA RTX 3080 Laptop GPU (Driver 580.178.04)
- **Target Executable**: `FlightSimulator2024.exe` (DirectX 12 Agility SDK, DLSS SDK 3.7+ / 310.x runtime)

---

## Issue Triage & Validity Assessment

### Reported Symptoms
1. **Zero Visual Effect in Pre-SR (Default)**: Toggling DLSS-NR on and off in MSFS 2024 produces no visible change in graphics fidelity, despite OptiScaler indicating active status and a ~4.25 ms GPU execution time.
2. **Failure of v0.9.51 Patch (`0fe693dc`)**: The previous commit added `ResolveNrRenderDimensions` to check `NVSDK_NGX_Parameter_Width/Height` on `parameters`. In real gameplay on v0.9.51, the issue persisted without improvement.
3. **Post-SR "Finished Picture" Rejection**: Attempting to switch to "Finished Picture" (post-SR) resulted in OptiScaler logging:
   ```
   DLSS-NR finished picture: The game's depth or movement data is not supported.
   ```

### Validity Assessment: Confirmed Architectural Defect & Synthetic Mock Blindspot
The issue is a **confirmed valid architectural defect** caused by three cascading factors:
1. **NGX Parameter Asymmetry at `EvaluateFeature` vs `CreateFeature`**:
   In the NVIDIA NGX DLSS SDK specification, `NVSDK_NGX_Parameter_Width` and `NVSDK_NGX_Parameter_Height` are parameters defined exclusively at `CreateFeature` (representing maximum feature capacity). Games do not pass `Width` or `Height` in `InParameters` when invoking `NVSDK_NGX_D3D12_EvaluateFeature`.
2. **Evaluation Timing in `IFeature_Dx12::Evaluate`**:
   In `IFeature_Dx12::Evaluate`, `CanRunBeforeUpscale_Dx12(InParameters)` was invoked at line 160 **before** `ProcessEvaluateParams(InParameters)` had ever executed. Consequently, `InParameters` contained neither subrect dimensions nor `Width/Height` at the moment DLSS-NR setup ran.
3. **Synthetic Mock Blindspot in Commit `0fe693dc`**:
   The unit test in `tests/nr_render_dimension_resolution_unit.cpp` passed only because the test harness artificially injected `params.Set(NVSDK_NGX_Parameter_Width, 1706)`. In real gameplay, `InParameters` is completely empty of dimension metadata at line 160, causing `ResolveNrRenderDimensions` to return `0x0`.
4. **The Pre-SR Misalignment Cascade**:
   With `width = 0, height = 0`, `PreSrColorExtent(desc, 0, 0)` returned the full surface allocation (`2560x1440`). `cropColor` evaluated to `false`. DLSS-NR allocated and ran its neural model on the uncropped `2560x1440` buffer, whereas the game engine only drew active geometry into the top-left `1706x960` viewport. The subsequent DLSS-SR pass upscaled the active `1706x960` region using its own temporal history, completely smoothing over and discarding the misaligned NR processing.
5. **Post-SR Multi-Mip Rejection**:
   In `LateContext::Clone`, an arbitrary assertion `want.MipLevels != 1` caused OptiScaler to reject MSFS 2024's multi-mip depth and motion vectors with `"The game's depth or movement data is not supported"`.

---

## Comprehensive Root Cause Analysis

### 1. The NVIDIA NGX Evaluation Parameter Specification
Under the NVIDIA DLSS SDK API contract:
- `NVSDK_NGX_Parameter_Width` and `NVSDK_NGX_Parameter_Height`: Set at `NVSDK_NGX_D3D12_CreateFeature` to declare the maximum feature execution extent.
- `NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width/Height`: Optional parameters provided during `NVSDK_NGX_D3D12_EvaluateFeature`, set *only* by games utilizing dynamic resolution scaling (DRS).
- In fixed-resolution titles or engines with internal custom viewports (like MSFS 2024 at DLSS Quality, 1440p), the engine passes only resources (`Color`, `Output`, `Depth`, `MotionVectors`) to `EvaluateFeature`.
- Relying on `parameters->Get(NVSDK_NGX_Parameter_Width)` during `EvaluateFeature` without checking the active upscaler instance guarantees `width = 0, height = 0`.

### 2. Call-Graph Order in `IFeature_Dx12::Evaluate`
Before the fix, the call graph inside `IFeature_Dx12::Evaluate` proceeded as follows:
```
IFeature_Dx12::Evaluate(commandList, InParameters)
├── line 160: CanRunBeforeUpscale_Dx12(InParameters)  <-- InParameters has NO Width/Height!
│   └── ResolveNrRenderDimensions(InParameters)       <-- Returns (0, 0)
│       └── PreSrColorExtent(desc, 0, 0)              <-- Returns full allocation (2560x1440)
│           └── cropColor = false                     <-- Model runs on full buffer, misaligned!
└── line 180: EvaluateInternal(commandList, InParameters)
    └── DLSSFeature::ProcessEvaluateParams(InParameters) <-- Finally populates Width/Height (TOO LATE!)
```

### 3. The Multi-Mip Guide Limitation in `LateContext`
In `DlssNr_Dx12_Late.cpp` and `DlssNr_Late.inl`, the clone helper asserted:
```cpp
const auto want = source->GetDesc();
if (want.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || want.SampleDesc.Count != 1 ||
    want.DepthOrArraySize != 1 || want.MipLevels != 1)
    return false;
```
MSFS 2024 creates depth and motion vector resources with multiple mip levels (e.g. `MipLevels = 4`). When users attempted to test the "Finished Picture" fallback, `Clone` rejected the resources. Furthermore, simply removing the check would cause D3D12 `CopyResource` to crash when destination and source subresource counts mismatched.

---

## Technical Solution Applied

### 1. Active Feature Render Dimension Fallback (`DlssNr_Pipeline_Dx12.cpp`, `DlssNrPipeline_Vk.h`)
`ResolveNrRenderDimensions` was updated to inspect `State::Instance().currentFeature` whenever parameters lack dimension metadata:
```cpp
void DlssNr::ResolveNrRenderDimensions(NVSDK_NGX_Parameter* parameters, unsigned int& width, unsigned int& height)
{
    width = 0;
    height = 0;
    if (parameters != nullptr)
    {
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
        if (width == 0 || height == 0)
        {
            parameters->Get(NVSDK_NGX_Parameter_Width, &width);
            parameters->Get(NVSDK_NGX_Parameter_Height, &height);
        }
    }
    if ((width == 0 || height == 0) && State::Instance().currentFeature != nullptr)
    {
        width = State::Instance().currentFeature->RenderWidth();
        height = State::Instance().currentFeature->RenderHeight();
    }
}
```

### 2. Early Resolution & Population in `IFeature_Dx12::Evaluate` (`IFeature_Dx12.cpp`, `ShaderPipeline_Dx12.h`)
At the immediate entry of `IFeature_Dx12::Evaluate`, before any pipeline decision or query runs, we resolve render resolution and ensure `NVSDK_NGX_Parameter_Width` and `Height` are present on `InParameters`:
```cpp
unsigned int evalRenderW = 0, evalRenderH = 0;
GetRenderResolution(InParameters, &evalRenderW, &evalRenderH);
if (evalRenderW == 0 || evalRenderH == 0)
{
    evalRenderW = RenderWidth();
    evalRenderH = RenderHeight();
}

unsigned int checkW = 0, checkH = 0;
if (InParameters->Get(NVSDK_NGX_Parameter_Width, &checkW) != NVSDK_NGX_Result_Success || checkW == 0)
    InParameters->Set(NVSDK_NGX_Parameter_Width, evalRenderW);
if (InParameters->Get(NVSDK_NGX_Parameter_Height, &checkH) != NVSDK_NGX_Result_Success || checkH == 0)
    InParameters->Set(NVSDK_NGX_Parameter_Height, evalRenderH);
```
`RestoreUpscalerResources_Dx12` was updated with `HasWidth()`, `HasHeight()`, `HasOutWidth()`, and `HasOutHeight()` query accessors to guarantee parameter clean-up on scope exit without modifying caller state.

### 3. Multi-Mip Guide Support & Subresource 0 Region Copy (`DlssNr_Dx12_Late.cpp`, `DlssNr_Late.inl`, `DlssNr_Dx12_Resources.cpp`)
1. Removed `want.MipLevels != 1` rejection in `LateContext::Clone`.
2. Normalized guide clone creation in `CreateGuideClone` to `desc.MipLevels = 1`.
3. In `LateContext::Capture` and `ReadableGuide`, when `src.MipLevels != dst.MipLevels`, perform a clean `CopyTextureRegion` on subresource 0:
```cpp
if (pair.first->GetDesc().MipLevels == pair.second->GetDesc().MipLevels)
{
    cmd->CopyResource(pair.second, pair.first);
}
else
{
    D3D12_TEXTURE_COPY_LOCATION dstLoc {};
    dstLoc.pResource = pair.second;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = pair.first;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    const auto d = pair.second->GetDesc();
    D3D12_BOX box { 0, 0, 0, static_cast<UINT>(d.Width), static_cast<UINT>(d.Height), 1 };
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
}
```
4. In `LateContext::Capture`, resolve `frame.RenderSubrectWidth/Height` using `DlssNr::ResolveNrRenderDimensions(params, ...)`.

---

## Automated Verification Matrix

The test suite in `tests/nr_render_dimension_resolution_unit.cpp` was expanded to 12 comprehensive scenarios:

| Test ID | Test Scenario | Expected Outcome | Result |
|---|---|---|---|
| **Test 1** | Explicit subrect parameters present | Precedence given to subrect (`1920x1080`) | **PASS** |
| **Test 2** | Subrect absent, `Width/Height` present | Fallback to `Width/Height` (`1706x960`) | **PASS** |
| **Test 3** | Subrect explicitly 0, `Width/Height` present | Fallback to `Width/Height` (`1706x960`) | **PASS** |
| **Test 4** | No dimensions and no active feature | Default to `0x0` | **PASS** |
| **Test 5** | Pre-SR active extent isolation | `1706x960` active render in `2560x1440` allocation (`cropColor = true`) | **PASS** |
| **Test 6** | Guide extent resolution | Guides correctly bound to `1706x960` | **PASS** |
| **Test 7** | Native DLAA 1:1 render scenario | `2560x1440` uncropped (`cropColor = false`) | **PASS** |
| **Test 8** | **Real-world MSFS 2024 evaluate: empty parameters** | Fallback to `currentFeature->RenderWidth()/Height()` (`1706x960`) | **PASS** |
| **Test 9** | **Real-world Pre-SR color crop with empty parameters** | `cropColor = true`, `PreSrColorCanUseScratch = true`, `1706x960` extent | **PASS** |
| **Test 10** | **Multi-mip guide cloning (`MipLevels = 4`)** | Normalized to `MipLevels = 1`, `CopyTextureRegion` subresource 0 | **PASS** |
| **Test 11** | **LateContext subrect resolution with empty parameters** | Resolves `RenderSubrectWidth = 1706`, `RenderSubrectHeight = 960` | **PASS** |
| **Test 12** | **Low-res motion guide region with empty parameters** | Motion guide correctly bounded to `1706x960` instead of `2560x1440` | **PASS** |

### Test Suite Execution Output
```
=== Running DLSS-NR Render Dimension Resolution Unit Tests ===
[PASS] Test 1: Subrect dimensions take precedence when present (1920x1080)
[PASS] Test 2: Fallback to Width/Height when subrect is absent (1706x960)
[PASS] Test 3: Fallback when subrect dimensions are 0 (1706x960)
[PASS] Test 4: Default to 0x0 when no dimensions are provided and no active feature
[PASS] Test 5: MSFS 2024 active extent correctly crops 1706x960 in 2560x1440 allocation
[PASS] Test 6: Guides correctly resolve to active render extent (1706x960)
[PASS] Test 7: DLAA scenario correctly identifies uncropped native 2560x1440
[PASS] Test 8: Empty evaluate parameters successfully fallback to active feature (1706x960)
[PASS] Test 9: Real-world MSFS 2024 color correctly crops to 1706x960 with empty parameters
[PASS] Test 10: Multi-mip guide cloning normalized to MipLevels=1 with subresource 0 region copy
[PASS] Test 11: LateContext subrect dimensions resolve to 1706x960 on empty parameters
[PASS] Test 12: Low-res motion guide region correctly bounds to resolved 1706x960
All DLSS-NR render dimension resolution unit tests passed successfully!
```

---

## Anti-Regression Invariants

1. **Never Assume `InParameters` Contains Dimensions at `EvaluateFeature`**:
   Any DLSS-NR pass or query function that requires render dimensions MUST fall back to `State::Instance().currentFeature->RenderWidth()/RenderHeight()` if `parameters` lacks both subrect and `Width/Height` entries.
2. **Never Reject Multi-Mip Guide Resources**:
   DirectX 12 depth and motion vector buffers supplied by modern engines frequently contain full mip chains (`MipLevels > 1`). Cloned guides must be normalized to `MipLevels = 1`, and copied using `CopyTextureRegion` on subresource 0 whenever subresource counts mismatch.
3. **Mandatory Verification Against Real-World Empty Parameter States**:
   Unit tests for NGX evaluation passes must never rely solely on mock parameters pre-populated with synthetic `Width` or `Height` values. All tests must assert correct behavior when `InParameters` is completely empty of dimension metadata.
