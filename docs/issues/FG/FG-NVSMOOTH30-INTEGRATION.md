# Frame Generation: NVSmooth30 Integration & SwapChain Present Conflict

## 1. Overview & Environment

- **Operating System**: Windows 11 build 26100 / 26200
- **Hardware**: NVIDIA GeForce RTX 3060 (Ampere SM86, `0x170`, GA106)
- **Driver**: NVIDIA Display Driver 616.92+ (Branch R570+)
- **Games**: Cyberpunk 2077 (Steam), Control Resonant (Steam)
- **Mod Configuration**: OptiScaler v0.9.25 / v0.9.30 with NVSmooth30 v0.3.0
- **Report Reference**: [DLSS Unlocked Issue #42](https://github.com/ShyVortex/dlss-unlocked/issues/42)

---

## 2. Reported Issue & Symptoms

A user reported interconnected defects when attempting to use the NVSmooth30 unlocker for driver-level Smooth Motion on Ampere (RTX 30 series):

1. **Menu UI Desynchronization ("Disabled in config" on checked checkbox)** (v0.9.25):
   - In the OptiScaler in-game overlay menu, under the Smooth Motion section on an RTX 30 GPU, the checkbox `Enable NVSmooth30 Unlocker (RTX 30)` appeared checked (`[X]`).
   - However, the status label printed immediately beneath the checkbox reported `"Disabled in config"`.
   - Users were unable to determine whether the unlocker was truly active, disabled, or failing to load.

2. **Presentation Crash / Black Screen (`0x80004002` / `E_NOINTERFACE` on Present)** (v0.9.25):
   - When explicitly setting `EnableNVSmooth30 = true` in `OptiScaler.ini` (or activating Smooth Motion), launching games such as *Cyberpunk 2077* or *Control* either crashed immediately upon entering 3D rendering or presented an unresponsive black screen.
   - Diagnostic logs revealed an unhandled `0x80004002` (`E_NOINTERFACE`) HRESULT returned during swapchain `Present` / `Present1` dispatch.

3. **Silent Non-Activation on Mid-Game Toggle (Framerate Lock at Native)** (v0.9.30):
   - When testing `v0.9.30`, the game launched once with default settings (`SmoothMotion = false`).
   - Upon opening the OptiScaler menu and enabling Smooth Motion and NVSmooth30, the menu updated to display `[Smooth Motion Active (RTX 30)]` and `NVSmooth30 Status: Active`.
   - However, zero interpolated frames were generated: native rendering framerate and display framerate were identical (78.3 FPS native == 78.5 FPS average).

4. **Immediate Startup Crash on Subsequent Launch (`0xC0000005` Access Violation)** (v0.9.30):
   - When the user exited the game (saving `SmoothMotion = true` in `OptiScaler.ini`) and launched *Control Resonant* a second time, the game crashed immediately upon startup.

---

## 3. Issue Validity Assessment

### Part 1: Menu UI Desynchronization
- **Validity**: **100% Genuine OptiScaler Defect.**
- **Verification**:
  - In `OptiScaler/Config.h`, `CustomOptional<bool> SmoothMotionNVSmooth30 { false };` initialized with default value `false`.
  - In `OptiScaler/framegen/smoothmotion/NVSmooth30Loader.cpp`, startup `TrySetup()` checked `cfg->SmoothMotionNVSmooth30.value_or(false)`. Since the setting was unset by default, this evaluated to `false`, leaving `s_status.Enabled = false`.
  - In `OptiScaler/menu/menu_common.cpp` (lines 3653 & 3696), the UI evaluated `config->SmoothMotionNVSmooth30.value_or(true)`. Because the setting was unset, it evaluated to `true`, causing ImGui to render the checkbox as checked (`[X]`).
  - Line 3718 then evaluated `else if (!nvSmoothStatus.Enabled) { ImGui::TextDisabled("Disabled in config"); }`.
  - The UI thus rendered a checked checkbox directly over a label saying "Disabled in config". Furthermore, when Smooth Motion itself was turned off (`FGDLSSGSmoothMotion = false`), the label displayed "Disabled in config" rather than clarifying that Smooth Motion was simply inactive.

### Part 2: Presentation Crash (`0x80004002` / `E_NOINTERFACE`)
- **Validity**: **100% Genuine OptiScaler Defect** arising from two distinct architectural oversights:
  - **Lack of Mutual Exclusion between DLSS-G / MFG and Driver-Level Smooth Motion**:
    In both attached logs (`cyberpunk2077OptiScaler.log` and `controlOptiScaler.log`), the user had both `AmpereMfgUnlock = true` (external DLSS-G proxy `dlssg_sm86.dll`) and `SmoothMotion = true` / `EnableNVSmooth30 = true` active simultaneously.
    DLSS-G Multi-Frame Generation and NVIDIA driver-level Smooth Motion (`NvPresent64.dll`) are mutually exclusive frame interpolation technologies. Running both concurrently caused both proxies to simultaneously hook the DXGI SwapChain vtable (slots 8 and 22). When `dlssg_sm86.dll` presented generated frames, `NvPresent64.dll` attempted to re-interpolate them, resulting in hook recursion, swapchain interface collision, and pipeline failure.
  - **Incomplete COM Interface Delegation in `WrappedIDXGISwapChain4::QueryInterface`**:
    When OptiScaler wraps the DXGI swapchain in `WrappedIDXGISwapChain4`, its `QueryInterface` implementation strictly handled only standard public interfaces (`IDXGISwapChain[1-4]`, `IUnknown`, `IDXGIObject`, `IDXGIDeviceSubObject`).
    For all other interfaces—including `IDXGISwapChainMedia` and private NVIDIA driver presentation interfaces queried by `NvPresent64.dll` (such as `INvPresentSwapChain`)—`WrappedIDXGISwapChain4::QueryInterface` returned `E_NOINTERFACE` without forwarding the query to `_real->QueryInterface(riid, ppvObject)`.
    When `NvPresent64.dll` queried the swapchain for its private driver interface and received `E_NOINTERFACE`, presentation aborted with `0x80004002`.

### Part 3: Silent Non-Activation on Mid-Game Toggle
- **Validity**: **100% Genuine OptiScaler Architectural Defect.**
- **Verification**:
  - NVIDIA Smooth Motion operates via driver-level presentation interception (`NvPresent64.dll`).
  - `NvPresent64.dll` hooks DXGI Factory creation and attaches its internal presentation wrapper object (at offset `+0x18` of the swapchain) **strictly during swapchain creation** (`CreateSwapChainForHwnd`).
  - If Smooth Motion is toggled in the ImGui menu while the game is already rendering, the swapchain was already created without `NvPresent64.dll` wrapping it.
  - `NvPresent64.dll` cannot retroactively hook a live, pre-existing swapchain.
  - OptiScaler's menu evaluated `nvSmoothStatus.DllLoaded == true` and falsely reported `[Smooth Motion Active (RTX 30)]`, giving the false impression that frame generation was active when it was completely dormant.
  - The menu help text incorrectly claimed `"Can be toggled dynamically on the fly"`, which is invalid for driver-level swapchain interception.

### Part 4: Immediate Startup Crash on Second Launch (`0xC0000005`)
- **Validity**: **100% Genuine OptiScaler Defect** caused by multi-threaded hook races and D3D11 vs D3D12 vtable stomping:
  - **Asynchronous Worker Thread Race**:
    In `dllmain.cpp`, `getGpuInfo` was spawned as a detached background thread via `CreateThread`. When `SmoothMotion = true` was saved in `OptiScaler.ini`, `getGpuInfo` called `NVSmooth30Loader::TrySetup()`. In turn, `nvsmooth30.dll`'s `DllMain` spawned a third thread running `bootstrap`.
    While the game's main thread was initializing Streamline (`sl.interposer.dll`), `CreateDXGIFactory`, and `D3D12CreateDevice`, `bootstrap` on the third thread created a 16x16 dummy window (`NVSmooth30DummyWindow`), called `D3D11CreateDeviceAndSwapChain`, and executed `patch_vtable` on the DXGI swapchain vtable using `VirtualProtect`.
  - **Streamline Interposer Hook Invalidation**:
    Streamline intercepted the 16x16 dummy swapchain on the background thread. When `bootstrap` immediately destroyed the window and reset the swapchain, Streamline was left holding dead internal handles.
  - **D3D11 vs D3D12 Function Pointer Collision in `dispatch_present`**:
    `nvsmooth30.dll`'s `install_hooks()` captured `g_present1` (slot 22) from the dummy D3D11 swapchain. In native D3D12 games, when `WrappedIDXGISwapChain4::Present1` called `pSwapChain->Present1`, the patched slot 22 jumped into `nvsmooth30.dll`'s `hook_present1`, which dispatched `g_present1(swapchain, ...)` with the D3D12 swapchain as `this`.
    Executing `CDXGISwapChain::Present1` (D3D11) with a `CDXGISwapChainD3D12` object caused an immediate invalid memory dereference and access violation (`0xC0000005`).
  - **Invalid `SetMaximumFrameLatency` Call**:
    `nvsmooth30`'s `select_primary()` called `swap2->SetMaximumFrameLatency(1)` on D3D12 swapchains without the `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` flag, returning `DXGI_ERROR_INVALID_CALL`.

---

## 4. Technical Solution & Architectural Invariants

### 1. Configuration & UI Default Harmonization
- Align the default value of `SmoothMotionNVSmooth30` across all files to `true` (so when an Ampere user opts into Smooth Motion, NVSmooth30 automatically unlocks SM86 without requiring manual INI edits).
- In `NVSmooth30Loader::TrySetup()`, check `cfg->FGDLSSGSmoothMotion.value_or_default()`: NVSmooth30 must only load if Smooth Motion is actually requested.
- In `menu_common.cpp`, disambiguate status messages:
  - If DLSS-G / MFG is active: display `"Conflict: DLSS-G / MFG active"` (with orange warning color).
  - If Smooth Motion is disabled: display `"Inactive (Smooth Motion disabled)"`.
  - If NVSmooth30 is explicitly unchecked: display `"Disabled in config"`.
  - If binary is missing: display `"OptiScaler/nvsmooth30.dll not found"`.
  - If loaded: display `"Active (OptiScaler/nvsmooth30.dll)"`.

### 2. Strict Mutual Exclusion: DLSS-G / MFG vs Smooth Motion
Per OptiScaler architecture rules (`.agents/rules/optiscaler-architecture.md`):
- Multi-Frame Generation unlockers (`AmpereMfgUnlock`, `AdaMfgUnlock`, external FG proxies) and driver-level Smooth Motion must remain strictly mutually exclusive across:
  - **Configuration Loading**: If `FGDLSSGAmpereMfgUnlock` or `FGDLSSGAdaMfgUnlock` is active, suppress or disable `FGDLSSGSmoothMotion` and `SmoothMotionNVSmooth30`.
  - **Runtime Execution**: In `NVSmooth30Loader::TrySetup()`, abort setup if `State::Instance().externalFrameGeneration`, `AmpereMfgUnlock`, or `AdaMfgUnlock` is active, setting `ErrorMessage = "Smooth Motion cannot be used while Frame Generation / MFG is active."`.
  - **In-Game Menu**: When MFG is active, disable the Smooth Motion checkbox via `ImGui::BeginDisabled()` with an explanatory tooltip (`"Disabled because DLSS-G / MFG Frame Generation is active. Disable MFG first to use Smooth Motion."`).

### 3. COM Interface Delegation in `WrappedIDXGISwapChain4` and `WrappedIDXGIFactory7`
In `OptiScaler/wrapped/wrapped_swapchain.cpp`:
```cpp
// Delegate any unhandled or private driver interfaces to the real swapchain
if (_real != nullptr)
    return _real->QueryInterface(riid, ppvObject);

*ppvObject = nullptr;
return E_NOINTERFACE;
```
Similarly in `OptiScaler/wrapped/wrapped_factory.cpp`:
```cpp
if (_real != nullptr)
    return _real->QueryInterface(riid, ppvObject);

*ppvObject = nullptr;
return E_NOINTERFACE;
```
This guarantees that extended Windows interfaces (`IDXGISwapChainMedia`) and proprietary NVIDIA driver presentation interfaces queried by `NvPresent64.dll` succeed without throwing `E_NOINTERFACE`.

### 4. Early Synchronous Startup Execution & Environment Sanitization
- Invoke `NVSmooth30Loader::TrySetup()` synchronously during early startup before D3D12 device and SwapChain creation (`hkD3D12CreateDevice`, `hkCreateSwapChainForHwnd`).
- Before loading `nvsmooth30.dll`, sanitize runtime environment variables:
  - `SetEnvironmentVariableW(L"SM86_ENABLE_D3D11_BRIDGE", L"0");` (disables D3D11 bridge for native D3D12 games).
  - `SetEnvironmentVariableW(L"SM86_LOW_LATENCY", L"0");` (prevents invalid `SetMaximumFrameLatency(1)` calls on D3D12 swapchains).
  - `SetEnvironmentVariableW(L"SM86_ENABLE_OSD", L"0");` (prevents GDI/D3D11 OSD crashes).
  - `SetEnvironmentVariableW(L"SM86_SKIP_DXGI_HOOKS", L"1");` (instructs proxy to skip dummy swapchain vtable hooking).

### 5. Neutralize Dummy 16x16 SwapChain & DXGI VTable Stomping in `DxgiFactory_Hooks.cpp`
- In `OptiScaler/hooks/DxgiFactory_Hooks.cpp`, detect the 16x16 dummy window creation (`localDesc.BufferDesc.Width <= 16 && localDesc.BufferDesc.Height <= 16`) from `NVSmooth30DummyWindow` and reject or isolate it so `nvsmooth30.dll`'s `install_hooks()` gracefully skips `patch_vtable`.
- This prevents `nvsmooth30` from stomping on OptiScaler's or Streamline's DXGI Present vtable slots, preventing the D3D11-to-D3D12 `Present1` crash and Streamline interposer destruction, while leaving `NvPresent64.dll`'s native driver presentation hooks fully active and functional.

### 6. Accurate SwapChain Attachment Verification & Menu Restart Requirement
- In `NVSmooth30Loader::Status`, track `SwapchainAttached`: verify whether `NvPresent64.dll` is genuinely attached to the active swapchain (by probing `_real`'s vtable or wrapper candidate offset `_real + 0x18`).
- In `menu_common.cpp`:
  - Change label to `"NVIDIA Smooth Motion (Driver-level FG; restart)##driver_sm"`.
  - When toggled mid-game while `SwapchainAttached` is false, display:
    `"[Pending Restart: Active on next game launch]"` in amber text.
  - Display `"[Smooth Motion Active (RTX 30)]"` in green text **only** when `NvPresent64.dll` is genuinely attached to the swapchain.
  - Update tooltip text to remove `"Can be toggled dynamically on the fly"`, explaining that driver-level presentation interception requires a game restart to bind to the swapchain.

---

## 5. Automated Verification

- `tests/nvsmooth30_loader_unit.cpp`:
  - Verify mutual exclusion gating rejecting setup when DLSS-G / Ampere MFG / Ada MFG is active.
  - Verify configuration disabling when `SmoothMotion` is false overall vs when `NVSmooth30` is false.
  - Verify candidate path resolution and status strings.
  - Verify environment variable sanitization (`SM86_ENABLE_D3D11_BRIDGE=0`, `SM86_LOW_LATENCY=0`, etc.).
  - Verify swapchain attachment verification logic distinguishing pending restart vs truly attached.
- `tests/ampere_smooth_motion_unit.cpp`:
  - Verify mutual exclusion UI evaluation when MFG is active.
  - Verify INI configuration priority and fallback defaults.
  - Verify restart-pending UI status reporting.
- All unit tests must compile and pass cleanly via `g++ -std=c++20`.
