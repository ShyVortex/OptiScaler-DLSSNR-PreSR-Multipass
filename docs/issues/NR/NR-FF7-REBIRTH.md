# DLSS-NR: FF7 Rebirth Cutscene Camera-Cut Stutter & Format Bouncing Stability

## Overview

In **FINAL FANTASY VII REBIRTH** (and other Unreal Engine 4/5 titles with dynamic post-processing pipelines), cutscenes alternate color buffer formats between standard gameplay and camera cuts:
- **Gameplay / Standard Frames**: `DXGI_FORMAT_R11G11B10_FLOAT` (Format 26)
- **Cutscene Camera Changes**: `DXGI_FORMAT_R16G16B16A16_FLOAT` (Format 10)

This format bouncing originally caused severe stutter (issue [#33](https://github.com/ShyVortex/dlss-unlocked/issues/33)) due to synchronous feature teardown and recreation. It was resolved in v0.9.6 via commit `dacfaeeeb6f3d0a321d3ddc8d5877dfe76f35bd1`, but briefly regressed in v0.9.25 during subsequent pipeline refactoring in commit `2e42397790790d9dabded1a9140f8f3a6e7f487e`.

This document records the exact root cause, architectural fix, and mandatory invariants required to ensure this issue never reoccurs.

---

## Root Cause Analysis

When a cutscene switches camera perspectives or render targets, the game modifies the format of the render target passed into the upscaler / DLSS-NR pipeline:
```
[23:54:50.723717] DLSS-NR rebuilding surfaces: model format 26 -> 10, frame format 10
[23:54:51.486787] DLSS-NR: feature created at 1708x960 through direct compatibility runtime
[23:54:51.487273] DLSS-NR pre-SR input dispatch failed in pipeline
[23:54:51.513806] DLSS-NR rebuilding surfaces: model format 10 -> 26, frame format 26
[23:54:52.227308] DLSS-NR: feature created at 1708x960 through direct compatibility runtime
```

### Why the Hitch Occurred:
1. **Model Feature Teardown**: In naive or regressed implementations, `ReleaseSurfacesIfFormatChanged` invoked `model.RetryAfterFailure()` and reset `modelRunning = false` whenever the color format changed.
2. **Synchronous NGX Feature Recreation**: Calling `RetryAfterFailure()` forces OptiScaler (or the direct NGX compatibility runtime) to destroy the existing DLSS-NR feature context (`CreateFeature(18)` / `NVSDK_NGX_D3D12_CreateFeature`). Reallocating deep learning model weight buffers and internal NGX states takes **700ms to 800ms** of synchronous GPU/CPU thread stall.
3. **Repeated Destruction**: Next frame or next camera cut, the format bounced back (10 $\rightarrow$ 26), discarding the newly built feature and stalling for another 700ms+.
4. **Intermediate Texture Allocations**: Discarding scratch surfaces required calling `CreateScratch()` (D3D12 `CreateCommittedResource`) on every transition, causing heap fragmentation and CPU overhead.

---

## Architectural Fix & Invariants

The fix relies on three foundational rules implemented across `ModelStateDx12`, `DlssNr_Dx12_Resources.cpp`, and `DlssNr_Dx12_Models.cpp`:

### 1. Dual-Format Surface Cache (`altSurfaces`)
`ModelStateDx12` maintains a dedicated `CachedSurfaces altSurfaces` struct alongside the active staging textures:
```cpp
struct CachedSurfaces
{
    DXGI_FORMAT modelFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT nativeFormat = DXGI_FORMAT_UNKNOWN;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int workWidth = 0;
    unsigned int workHeight = 0;
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* passScratch = nullptr;
    ID3D12Resource* passClamp = nullptr;
    ID3D12Resource* hdrCopy = nullptr;
    ID3D12Resource* activeColor = nullptr;
    ID3D12Resource* colorSmall = nullptr;
    ID3D12Resource* outputNative = nullptr;
} altSurfaces;
```

### 2. O(1) Instant Pointer Swap Without Model Invalidation
In `DlssNr_Dx12::State::ReleaseSurfacesIfFormatChanged`:
- **Do NOT destroy models**: Never call `model.RetryAfterFailure()` or set `modelRunning = false`. The DLSS-NR model context depends strictly on dimensions (`workWidth`, `workHeight`) and pass tuning, **not** on the intermediate surface pixel format.
- **Cache Hit (O(1) Swap)**: If `altSurfaces` matches the requested `modelFormat`, `nativeFormat`, and dimensions, execute a pointer swap (`std::swap`) between active textures and `altSurfaces`. Zero memory allocations take place.
- **Cache Miss (Stash)**: If no matching cache exists, park existing `altSurfaces`, stash the current surfaces into `altSurfaces`, and set current pointers to `nullptr` so `PrepareRunModels` allocates the new format surfaces once.
- **Temporal Reset**: Set `nr.reset = true` to inform the temporal model to discard previous history across the camera cut.

### 3. Resolution & Teardown Invalidation
- When true resolution or placement changes occur (`resolutionChanged || placementChanged` in `PrepareRunModels`), both the active pool and `altSurfaces` must be parked and cleared (`altSurfaces = {}`).
- On complete teardown (`ReleaseResources()`), all resources in `altSurfaces` must be retired via `ParkNrResource`.

---

## Automated Verification

The behavior is strictly enforced and verified by unit test:
```bash
tests/nr_format_bouncing_unit.cpp
```
The test verifies:
1. Zero allocations during format switches after initial warmup.
2. Zero model retries (`models[0].retries == 0`) across 100 ping-pong cycles between formats 26 and 10.
3. Uninterrupted model execution (`modelRunning == true`).
4. Correct dual-format invalidation on genuine resolution changes.
5. Mixed-format handling for spatial NR modes (`modelFormat != nativeFormat`).

---

## Preservation Mandate

This mechanism must **never** be removed, commented out, or bypassed during upstream synchronization with `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass` or subsequent refactoring. Any changes to `ReleaseSurfacesIfFormatChanged` must preserve the dual-format cache and maintain `modelRunning == true`.
