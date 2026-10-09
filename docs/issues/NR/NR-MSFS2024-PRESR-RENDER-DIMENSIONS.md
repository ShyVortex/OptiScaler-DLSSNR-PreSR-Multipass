# DLSS-NR: MSFS 2024 Pre-SR Parameter Resolution & Multi-Mip Admission Shift

## Overview

In **Microsoft Flight Simulator 2024** (`FlightSimulator2024.exe`), users reported in Issue [#49](https://github.com/ShyVortex/dlss-unlocked/issues/49) that DLSS Neural Rendering (DLSS-NR) appeared to have no visual effect from **v0.9.32 up to v0.9.50**, despite visibly functioning in **v0.9.9**. Suspicion was initially raised that this might be a regression introduced by PR [#19](https://github.com/ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass/pull/19).

This document details the complete triage, root cause analysis, architectural resolution, automated test suite, and operational workarounds.

---

## Issue Triage & Validity Assessment

### Reported Issue
- **Environment**: Windows 11, NVIDIA GeForce RTX 5090 (Blackwell), Microsoft Flight Simulator 2024.
- **Symptom**: Toggling DLSS-NR produced an immediate, visible visual transformation in DLSS Unlocked v0.9.9, but starting in v0.9.32 and continuing through v0.9.50, DLSS-NR produced no observable visual difference.
- **Initial Hypothesis**: PR #19 was suspected of introducing a regression into the DLSS-NR pipeline.

### Issue Validity Assessment
1. **Exoneration of PR #19**:
   - PR #19 (`dd25f17f`) was merged into `main` on **October 7, 2026** for release in **v0.9.50**.
   - The reported regression began in **v0.9.32**, which was released on **September 26, 2026** (11 days prior to PR #19).
   - Therefore, PR #19 did not introduce the behavioral change.

2. **Genuine Defect Confirmation**:
   - The issue is a **valid defect** resulting from an architectural pipeline interaction between multi-mip texture admission changes introduced in commit `08fa575e` (PR #13, September 26, 2026) and NGX parameter query assumptions in the Pre-SR pipeline:
     - **v0.9.9 Behavior (Post-SR)**: In v0.9.9, `CanRunBeforeUpscale_Dx12` strictly rejected textures with `desc.MipLevels > 1`. MSFS 2024 passes a color texture with multiple mip levels (`MipLevels = 2`). Because Pre-SR rejected the texture, OptiScaler automatically fell back to **Post-SR** (post-upscale) execution at display resolution (`2560x1440`). In Post-SR mode, DLSS-NR denoised the final reconstructed frame after DLSS-SR, producing an immediate, obvious sharpening and denoising visual effect.
     - **v0.9.32 Admission Shift (Pre-SR)**: Commit `08fa575e` introduced `PreSrColorCanUseScratch`, allowing multi-mip color textures into Pre-SR as long as the scratch texture format matched. Consequently, starting in v0.9.32, MSFS 2024 was admitted into the Pre-SR path (`RunBeforeSR = true`).
     - **Subrect Parameter Absence**: In Pre-SR, OptiScaler queried `NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width` and `NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height`. MSFS 2024 specifies render dimensions via `NVSDK_NGX_Parameter_Width` and `NVSDK_NGX_Parameter_Height` (e.g., `1706x960` for Quality mode at 1440p) without setting the optional subrect dimensions.
     - **Pipeline Failure**: Because subrect dimensions were missing, `RenderSubrectWidth` and `RenderSubrectHeight` defaulted to `0`. `PreSrColorExtent` fell back to reading the full backing texture dimensions (`2560x1440`). Consequently, `cropColor` failed to isolate the active rendering region, guide bounds were mismatched, and the subsequent DLSS Super Resolution pass smoothed and reconstructed the Pre-SR output, leaving the user with no discernible visual effect.

---

## Root Cause Analysis

### Parameter Query Asymmetry

When games invoke NGX evaluate (`NVSDK_NGX_D3D12_EvaluateFeature`), the DLSS specification permits two methods for communicating the active rendering resolution:
1. **Dynamic Resolution / Subrect API**:
   - `NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width`
   - `NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height`
2. **Standard Render Dimension API**:
   - `NVSDK_NGX_Parameter_Width`
   - `NVSDK_NGX_Parameter_Height`

In `OptiScaler/dlssnr/DlssNr_Pipeline_Dx12.cpp`:
```cpp
// Prior code:
unsigned int width = 0, height = 0;
parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
```
When a game sets only `Width` and `Height`:
- `width` and `height` evaluated to `0`.
- In `DlssNr::PreSrColorExtent(desc, width, height)`:
  ```cpp
  if (renderWidth == 0 || renderHeight == 0)
      return Extent{desc.Width, desc.Height};
  ```
- The pipeline assumed the active viewport was the entire allocated surface (`2560x1440`), rather than the actual rendering sub-region (`1706x960`).
- Denoising was performed on uninitialized padding or improperly scaled coordinates, which DLSS Super Resolution subsequently discarded or smoothed over during temporal reconstruction.

---

## Solution Applied

### 1. Robust Dimension Resolution Helper
Introduced `ResolveNrRenderDimensions` in `OptiScaler/dlssnr/DlssNr_Pipeline_Dx12.cpp`:
```cpp
void ResolveNrRenderDimensions(NVSDK_NGX_Parameter* parameters, unsigned int& width, unsigned int& height)
{
    width = 0;
    height = 0;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
    if (width == 0 || height == 0)
    {
        parameters->Get(NVSDK_NGX_Parameter_Width, &width);
        parameters->Get(NVSDK_NGX_Parameter_Height, &height);
    }
}
```
This helper is now applied in:
- `DlssNr::CanRunBeforeUpscale_Dx12`: for gate validation.
- `DlssNr::MakeDlssNrPass`: for setting `frame.RenderSubrectWidth` and `frame.RenderSubrectHeight`.

### 2. Vulkan Pipeline Parity
Updated `OptiScaler/dlssnr/DlssNrPipeline_Vk.h`:
- `HasSupportedSubrects`: Falls back to `NVSDK_NGX_Parameter_Width/Height` when subrect dimensions are `0`.
- `FrameInfo`: Populates `RenderSubrectWidth/Height` from `NVSDK_NGX_Parameter_Width/Height` when subrect queries return `0`.

### 3. Denoise-First & Deferred-SR Feature Parity
Updated `OptiScaler/shaders/dlssnr/DlssNr_Dx12_DenoiseFirst.cpp` and `OptiScaler/shaders/dlssnr/DlssNr_Dx12_DeferredSr.cpp`:
```cpp
const auto renderW =
    UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, UInt(source, NVSDK_NGX_Parameter_Width));
const auto renderH = UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
                          UInt(source, NVSDK_NGX_Parameter_Height));
const auto active = DlssNr::PreSrColorExtent(inDesc, renderW, renderH);
```

---

## Automated Verification

The resolution is verified by an automated unit test suite:
- **Test File**: `tests/nr_render_dimension_resolution_unit.cpp`
- **Scenarios Tested**:
  1. `DLSS_Render_Subrect_Dimensions` takes explicit precedence when populated by dynamic resolution engines.
  2. Fallback to `Width`/`Height` when `DLSS_Render_Subrect_Dimensions` is missing.
  3. Fallback to `Width`/`Height` when `DLSS_Render_Subrect_Dimensions` returns `0`.
  4. Proper 0x0 fallback to full backing surface when no dimensions are provided.
  5. MSFS 2024 active extent isolation (`1706x960` active render in `2560x1440` allocation with `cropColor = true`).
  6. Guide extent matching against active render extent.
  7. DLAA 1:1 render scenario (`2560x1440` in `2560x1440` allocation with `cropColor = false`).

### Execution Output:
```
=== Running DLSS-NR Render Dimension Resolution Unit Tests ===
[PASS] Test 1: Subrect dimensions take precedence when present (1920x1080)
[PASS] Test 2: Fallback to Width/Height when subrect is absent (1706x960)
[PASS] Test 3: Fallback when subrect dimensions are 0 (1706x960)
[PASS] Test 4: Default to 0x0 when no dimensions are provided
[PASS] Test 5: MSFS 2024 active extent correctly crops 1706x960 in 2560x1440 allocation
[PASS] Test 6: Guides correctly resolve to active render extent (1706x960)
[PASS] Test 7: DLAA scenario correctly identifies uncropped native 2560x1440
All DLSS-NR render dimension resolution unit tests passed successfully!
```

---

## User Workaround & Operational Guidance

Users who prefer the direct post-upscale denoising behavior of v0.9.9 in MSFS 2024 can configure OptiScaler to run DLSS-NR in **Post-SR** mode:
1. In the OptiScaler in-game overlay menu (Insert / Home), navigate to **Neural Rendering (DLSS-NR)** and uncheck **"Generate model before upscale"**.
2. Or in `OptiScaler.ini`, under the `[DLSSNR]` section, set:
   ```ini
   [DLSSNR]
   RunBeforeSR = false
   ```
This immediately restores Post-SR execution at display resolution, bypassing the Pre-SR crop and DLSS SR reconstruction passes.
