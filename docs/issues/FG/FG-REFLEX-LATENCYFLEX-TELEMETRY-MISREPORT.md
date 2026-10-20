# In-Game Reflex Telemetry Misreport & LatencyFlex Override Remediation

## 1. Overview & Environment

- **Operating System**: CachyOS Linux (Kernel: `7.2.11-1-cachyos`, Arch-based rolling) / Windows 10/11 x64
- **Hardware Platform**: NVIDIA GeForce RTX 3080 Laptop GPU (16 GB VRAM) / RTX 40-Series (RTX 4070 Ti)
- **NVIDIA Driver Version**: 580.178.04, CUDA 13.0
- **Target Subsystem**: `fakenvapi` (`OptiScaler/nvapi/fakenvapi.*`, `low_latency.*`), `InputCommon` (`low_latency/input/`), and In-Game Overlay (`menu/menu_common.cpp`)
- **Affecting Scenarios**: DirectX 12 / DirectX 11 titles utilizing NVIDIA Reflex on native NVIDIA GPUs

---

## 2. Reported Symptoms

A user operating an NVIDIA GeForce RTX 4070 Ti submitted the following issue:
> *"With the latest version, LatencyFlex is taking over Reflex in games. Is this expected behavior? Isn't Reflex superior (i own a Nvidia GPU 4070Ti)? Describe the solution you'd like: I'd like to default to Reflex if it is superior to LatencyFlex... I've noticed increased frame-time jitter & microstutters with LatencyFlex compared to Reflex."*

Specific user observations:
1. Upon opening the OptiScaler in-game overlay menu under the **Framerate** subsection, the display showed:
   ```
   Current method: LatencyFlex
   ```
2. The LatencyFlex configuration combo box became active (`usingLFX = true`), leading users to believe OptiScaler had replaced native driver Reflex with LatencyFlex on their NVIDIA GPU.
3. Users requested clarification and an automatic preference/default for NVIDIA driver Reflex over LatencyFlex.

---

## 3. Issue Validity Assessment & Root Cause Analysis

### 3.1 Defect Verification: Confirmed Telemetry Defect & Secondary Logic Gap

Trace analysis across `OptiScaler/nvapi/`, `OptiScaler/hooks/Reflex_Hooks.cpp`, and `OptiScaler/menu/menu_common.cpp` revealed that **native NVIDIA driver Reflex was actively running at the driver level all along, but the in-game UI telemetrically misreported it as LatencyFlex**:

1. **Actual Driver Reflex Execution via `ReflexHooks`**:
   - When OptiScaler hooks NVAPI via `NvApiHooks::Hook`, it attaches to the system's real `nvapi64.dll`.
   - The hooked functions `ReflexHooks::hkNvAPI_D3D_SetSleepMode` and `ReflexHooks::hkNvAPI_D3D_Sleep` evaluate `if (IsXeFGActive())`.
   - When XeFG is not active (standard rendering, DLSS-G, or FSR-FG), calls pass directly to `o_NvAPI_D3D_SetSleepMode` and `o_NvAPI_D3D_Sleep` (the driver's real Reflex implementation).
   - No fake latency tech (`currently_active_tech`) is ever instantiated or active.

2. **Root Cause A: Hardcoded `_lowLatencyMode` Initialization (`fakenvapi.h`)**:
   - In `OptiScaler/nvapi/fakenvapi.h` (line 19):
     ```cpp
     inline static LowLatencyMode _lowLatencyMode = LowLatencyMode::LatencyFlex;
     ```
   - The static mode variable defaulted to `LowLatencyMode::LatencyFlex` instead of `LowLatencyMode::None`.

3. **Root Cause B: Incomplete Inactive State Query in `get_low_latency_tech_context` (`low_latency.cpp`)**:
   - In `OptiScaler/nvapi/fakenvapi/low_latency.cpp` (lines 53–72):
     ```cpp
     bool LowLatency::get_low_latency_tech_context(void** low_latency_context, LowLatencyMode* low_latency_tech)
     {
         if (auto current_tech = currently_active_tech.load())
         {
             if (!current_tech || !low_latency_context || !low_latency_tech)
                 return false;

             *low_latency_context = current_tech->get_tech_context();
             *low_latency_tech = current_tech->get_mode();
         }
         return true;
     }
     ```
   - When native Reflex was running, `currently_active_tech.load()` was `nullptr`.
   - The function exited and returned `true` **without** setting `*low_latency_tech` or `*low_latency_context`.
   - Consequently, `_lowLatencyMode` was never modified and retained its erroneous initial value (`LowLatencyMode::LatencyFlex`).

4. **Root Cause C: Telemetric Misreport in In-Game Overlay (`menu_common.cpp`)**:
   - In `menu_common.cpp` (lines 6197–6215):
     ```cpp
     if (state.reflexLimitsFps)
     {
         fakenvapiMode = fakenvapi::getCurrentMode();

         if (fakenvapiMode == LowLatencyMode::AntiLag2)
             currentMethod = "FSR Anti-Lag 2.0";
         else if (fakenvapiMode == LowLatencyMode::LatencyFlex)
             currentMethod = "LatencyFlex";
         ...
         else if (fakenvapiMode == LowLatencyMode::None)
         {
             if (fakenvapi::isUsingAsMainNvapi())
                 currentMethod = "None";
             else
                 currentMethod = "Reflex";
         }
     }
     ```
   - Because `fakenvapi::getCurrentMode()` returned `LowLatencyMode::LatencyFlex`, the menu selected `"LatencyFlex"`.
   - Line 6240 printed `Current method: LatencyFlex`.
   - Line 6363 (`bool usingLFX = fakenvapi::getCurrentMode() == LowLatencyMode::LatencyFlex;`) evaluated to `true`, un-disabling the LatencyFlex mode dropdown.

5. **Root Cause D: Vendor Auto Fallthrough in `input_common.cpp`**:
   - In `OptiScaler/low_latency/input/input_common.cpp` (lines 186–198):
     ```cpp
     if (desiredMode == LowLatencyMode::Auto)
     {
         auto vendorId = IdentifyGpu::getPrimaryGpu().vendorId;
         if (vendorId == VendorId::Intel)
             desiredMode = LowLatencyMode::XeLL;
         else if (vendorId == VendorId::AMD)
             desiredMode = LowLatencyMode::AntiLag2;
         // else if (vendorId == VendorId::Nvidia)
         //     desiredMode = LowLatencyMode::Reflex; // TODO: not supported yet
         else
             desiredMode = LowLatencyMode::LatencyFlex;
     }
     ```
   - Because the NVIDIA case was commented out, an `Auto` selection on NVIDIA fell through to `LatencyFlex`.

---

## 4. Technical Solution Applied

1. **State Initialization Sanitization (`OptiScaler/nvapi/fakenvapi.h`)**:
   - Replaced default `_lowLatencyMode = LowLatencyMode::LatencyFlex;` with `LowLatencyMode::None;`.

2. **Explicit Inactive Tech Reporting (`OptiScaler/nvapi/fakenvapi/low_latency.cpp`)**:
   - When `currently_active_tech.load()` is `nullptr`:
     - Sets `*low_latency_context = nullptr;`
     - Sets `*low_latency_tech = LowLatencyMode::None;`
     - Returns `false` (signaling that no fake latency tech context is active).

3. **Safe Context Synchronization (`OptiScaler/nvapi/fakenvapi.cpp`)**:
   - In `fakenvapi::updateModeAndContext()`:
     - When `lowLatencyCtx` is unavailable or `get_low_latency_tech_context` returns `false`, explicitly resets `_lowLatencyTechContext = nullptr;` and `_lowLatencyMode = LowLatencyMode::None;`.
     - Suppresses verbose trace logging when native Reflex is active without `_usingFakenvapiAsMainNvapi`.

4. **NVIDIA Vendor Auto Mode Alignment (`OptiScaler/low_latency/input/input_common.cpp`)**:
   - Handled `VendorId::Nvidia` under `desiredMode == LowLatencyMode::Auto` by setting `desiredMode = LowLatencyMode::None;`, preventing unintended fallback to `LatencyFlex`.

---

## 5. Verification & Unit Testing

- **Unit Test**: `tests/reflex_method_telemetry_unit.cpp`
  - Case 1: Asserts that inactive fake tech queries return `false`, `nullptr` context, and `LowLatencyMode::None`.
  - Case 2: Asserts that active fake tech (LatencyFlex, XeLL) returns `true`, valid context, and active mode.
  - Case 3: Asserts that when `state.reflexLimitsFps == true` and `fakenvapiMode == LowLatencyMode::None` on NVIDIA, `currentMethod` evaluates strictly to `"Reflex"` and `usingLFX` evaluates to `false`.
  - Case 4: Asserts that `InputCommon` vendor selection under `LowLatencyMode::Auto` resolves to `LowLatencyMode::None` on NVIDIA.
- **Standards Verification**:
  - Zero `clang-format` warnings (`clang-format --dry-run --Werror`).
  - Mandatory UTF-8 BOM (`\xef\xbb\xbf`) verified across all modified files via Python assertion.
