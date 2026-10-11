# Frame Generation: Helldivers 2 XeMFG-SR Inactive on 0x0 Window-Sized SwapChains

## 1. Overview & Environment

- **Operating System**: Linux (CachyOS x86_64, Kernel 6.13, Proton Experimental / Wine / VKD3D-Proton)
- **Target Game**: *Helldivers 2* (build 1.003.x, Arrowhead Game Studios / Sony Interactive Entertainment)
- **GPU Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (16GB VRAM, GA103 / Ampere SM86)
- **Display Resolution & Upscaler**: 2560x1600, DLSS Super Resolution (Quality mode, DLSS SDK 3.7.20)
- **Target Runtimes**: `dxgi.dll` (OptiScaler v0.7.x), `libxess_fg.dll` (Intel XeFG v1.3.1)
- **Feature**: Intel Xe Multi-Frame Generation Injector for DLSS-SR (`XeMFG-SR`)
- **Reported Symptom**: After resolving the initial `LowLatencyCtx` crash, the game ran stably, but XeMFG interpolation failed to activate and modifying the frame multiplier in the in-game overlay had no effect on presented framerate or frame delivery.

---

## 2. Problem Statement & Log Trace

After fixing the `LowLatencyCtx` uninitialized memory dereference, *Helldivers 2* booted smoothly with DLSS-SR and XeMFG-SR configured. However, no interpolated frames were generated, and the in-game multiplier slider produced no change.

Inspection of `/mnt/DataCachy/SteamLibrary/steamapps/common/Helldivers 2/bin/OptiScaler.log` revealed the following warning during engine initialization:
```text
[12:50:20.340835] [W] DxgiFactoryHooks::CreateSwapChainForHwnd Overlay call! Width: 0, Height: 0
```

### 2.1 Operational Pipeline Disconnect
Because of that single log line:
1. OptiScaler misclassified the game's primary presentation swapchain as a 3rd-party overlay (such as RivaTuner Statistics Server or the Steam Overlay).
2. It forwarded the creation call directly to the native graphics driver via `o_CreateSwapChainForHwnd` without wrapping it.
3. `FGHooks::CreateSwapChainForHwnd` was completely bypassed.
4. `State::Instance().currentFGSwapchain` was never assigned (remained `nullptr`).
5. `State::Instance().currentFG` (`XeFG_Dx12`) was never instantiated (remained `nullptr`).
6. `UpscalerInputsDx12::UpscaleStart` and `UpscaleEnd` queried `currentFG`, found `nullptr`, and skipped recording resource barriers and copying depth, motion vectors, and color buffers.
7. Frame generation presentation detours were never attached, leaving interpolation dead and the multiplier control disconnected from any active pipeline context.

---

## 3. Root Cause Analysis

### 3.1 The DXGI 1.2+ `CreateSwapChainForHwnd` Contract
According to Microsoft DirectX Graphics Infrastructure (DXGI) documentation for `IDXGIFactory2::CreateSwapChainForHwnd`:
> *If you set `Width` or `Height` in `DXGI_SWAP_CHAIN_DESC1` to 0, the runtime obtains the size from the output window and assigns this size to the swap-chain description.*

Engines targeting modern DirectX 12 commonly pass `Width = 0` and `Height = 0` during window creation to allow DXGI to automatically match the target `HWND` client rectangle. *Helldivers 2* follows this standard pattern.

### 3.2 Premature Overlay Heuristic Check in OptiScaler
In `OptiScaler/hooks/DxgiFactory_Hooks.cpp` and `OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp`, OptiScaler uses a threshold heuristic to ignore small utility swapchains created by overlays:
```cpp
if (pDesc->Height < 100 || pDesc->Width < 100)
{
    LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);
    return o_CreateSwapChainForHwnd(...);
}
```
Because the game provided `pDesc->Width == 0 && pDesc->Height == 0`, the condition `0 < 100` evaluated to `true`, triggering the overlay bypass before the window dimensions were ever queried.

### 3.3 Missing Sizing Overload for `DXGI_SWAP_CHAIN_DESC1`
Upstream author `wilsjo2` previously introduced `ResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC& desc)` in `OptiScaler/hooks/DxgiSwapchainSizing.h` for legacy DXGI 1.0 `CreateSwapChain`, but never provided the corresponding overload for `DXGI_SWAP_CHAIN_DESC1` taking an explicit `HWND` for DXGI 1.2+ `CreateSwapChainForHwnd`.

---

## 4. Architectural Solution Applied

### 4.1 DXGI 1.2+ Sizing Overload in `DxgiSwapchainSizing.h`
Added an inline overload accepting `DXGI_SWAP_CHAIN_DESC1` and target `HWND`:
```cpp
inline bool ResolveWindowSizedSwapchain(DXGI_SWAP_CHAIN_DESC1& desc, HWND window)
{
    if (desc.Width != 0 || desc.Height != 0 || window == nullptr)
        return false;

    RECT rect {};
    if (!GetClientRect(window, &rect))
        return false;

    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width < 100 || height < 100)
        return false;

    desc.Width = (UINT) width;
    desc.Height = (UINT) height;
    return true;
}
```

### 4.2 Local Descriptor Resolution Prior to Overlay Gate
In both `DxgiFactory_Hooks.cpp` and `DxgiFactory_WrappedCalls.cpp`:
1. The incoming descriptor is copied into a local structure (`localDesc = *pDesc;`).
2. `ResolveWindowSizedSwapchain(localDesc, hWnd)` is invoked *before* the overlay check.
3. The `< 100` check evaluates `localDesc` rather than the raw `pDesc`.
4. If `localDesc` was sized to window, an informational log entry is emitted:
   ```text
   CreateSwapChainForHwnd: 0x0 size-to-window descriptor, Hwnd: {:X}, Count: {}, wrapping normally
   ```
5. The original caller pointer `pDesc` is preserved and never mutated in-place, preventing side-effects on the game engine.
6. The swapchain successfully enters `FGHooks::CreateSwapChainForHwnd`, instantiating `currentFG` and activating XeMFG-SR interpolation.

---

## 5. Verification Matrix

| Test Suite / Component | Verification Scope | Status |
| :--- | :--- | :--- |
| `tests/dxgi_swapchain_sizing_unit.cpp` | 0x0 game window resolution (1920x1080), caller preservation, ultrawide (3440x1440), tiny helper rejection (< 100px), 100px boundary threshold, null/invalid HWND, explicit size immutability, partial zero rejection, DXGI 1.0 parity, and `DxgiFactory_Hooks` gating simulation | **PASS** (9/9) |
| `tests/dxgi_window_size_smoke.cpp` | Extended with `DXGI_SWAP_CHAIN_DESC1` hidden window classification, descriptor preservation, threshold checks, and WARP `CreateSwapChainForHwnd` validation | **PASS** |
| `clang-format` | Zero format violations across modified headers, hook implementations, and test suites | **PASS** |
| UTF-8 BOM Preservation | Verified byte markers (`\xef\xbb\xbf`) on `DxgiFactory_Hooks.cpp` | **PASS** |
