# Frame Generation: NVSmooth30 Integration & SwapChain Present Conflict

## 1. Overview & Environment

- **Operating System**: Windows 11 build 26100 / 26200
- **Hardware**: NVIDIA GeForce RTX 3060 (Ampere SM86, `0x170`, GA106)
- **Driver**: NVIDIA Display Driver 616.92+ (Branch R570+)
- **Games**: Cyberpunk 2077 (Steam), Control (Steam)
- **Mod Configuration**: OptiScaler v0.9.25 / v0.9.26 with NVSmooth30 v0.3.0
- **Report Reference**: [DLSS Unlocked Issue #42](https://github.com/ShyVortex/dlss-unlocked/issues/42)

---

## 2. Reported Issue & Symptoms

A user reported two interconnected defects when attempting to use the NVSmooth30 unlocker for driver-level Smooth Motion on Ampere (RTX 30 series):

1. **Menu UI Desynchronization ("Disabled in config" on checked checkbox)**:
   - In the OptiScaler in-game overlay menu, under the Smooth Motion section on an RTX 30 GPU, the checkbox `Enable NVSmooth30 Unlocker (RTX 30)` appears checked (`[X]`).
   - However, the status label printed immediately beneath the checkbox reports `"Disabled in config"`.
   - Users are unable to determine whether the unlocker is truly active, disabled, or failing to load.

2. **Presentation Crash / Black Screen (`0x80004002` / `E_NOINTERFACE` on Present)**:
   - When explicitly setting `EnableNVSmooth30 = true` in `OptiScaler.ini` (or activating Smooth Motion), launching games such as *Cyberpunk 2077* or *Control* either crashes immediately upon entering 3D rendering or presents an unresponsive black screen.
   - Diagnostic logs reveal an unhandled `0x80004002` (`E_NOINTERFACE`) HRESULT returned during swapchain `Present` / `Present1` dispatch.

---

## 3. Issue Validity Assessment

### Part 1: Menu UI Desynchronization
- **Validity**: **100% Genuine OptiScaler Defect.**
- **Verification**:
  - In `OptiScaler/Config.h`, `CustomOptional<bool> SmoothMotionNVSmooth30 { false };` initializes with default value `false`.
  - In `OptiScaler/framegen/smoothmotion/NVSmooth30Loader.cpp`, startup `TrySetup()` checks `cfg->SmoothMotionNVSmooth30.value_or(false)`. Since the setting is unset by default, this evaluates to `false`, leaving `s_status.Enabled = false`.
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

---

## 5. Automated Verification

- `tests/nvsmooth30_loader_unit.cpp`:
  - Verify mutual exclusion gating rejecting setup when DLSS-G / Ampere MFG / Ada MFG is active.
  - Verify configuration disabling when `SmoothMotion` is false overall vs when `NVSmooth30` is false.
  - Verify candidate path resolution and status strings.
- `tests/ampere_smooth_motion_unit.cpp`:
  - Verify mutual exclusion UI evaluation when MFG is active.
  - Verify INI configuration priority and fallback defaults.
- All unit tests must compile and pass cleanly via `g++ -std=c++20`.
