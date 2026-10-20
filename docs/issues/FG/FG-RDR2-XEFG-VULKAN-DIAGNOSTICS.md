# Intel XeMFG Vulkan Diagnostics & Inactive Status Remediation

## 1. Overview & Environment

- **Operating System**: CachyOS Linux (Kernel: `7.2.11-1-cachyos`, Arch-based rolling) / Windows 10/11 x64
- **Hardware Platform**: NVIDIA GeForce RTX 3080 Laptop GPU (16 GB VRAM) / RTX 40-Series (RTX 4060 Ti)
- **NVIDIA Driver Version**: 580.178.04, CUDA 13.0
- **Target Applications**: Titles supporting dual Vulkan / DirectX 12 APIs (e.g. *Red Dead Redemption 2*, `rdr2.exe`)
- **Target Subsystem**: Intel Xe Multi-Frame Generation (XeMFG) in `OptiScaler/menu/menu_common.cpp` and `OptiScaler/framegen/xefg/`

---

## 2. Reported Symptoms

A user reported that Intel XeSS Frame Generation (XeFG) with the `OptiScaler (Injected)` source was not generating interpolated frames in *Red Dead Redemption 2* (RDR2) on OptiScaler v0.9.70:
1. In the in-game overlay menu under **Intel Xe Multi-Frame Generation (XeMFG)**:
   - `Enable XeMFG Unlocker` was checked.
   - `Input Source (restart)` was set to `OptiScaler (Injected)`.
   - DLL status reported: `DLL: found | Patches: 5/5 | Pacing: active (native provider)`.
   - A persistent orange message stated:
     ```
     Note: Save Settings and restart the game to initialize the XeFG SwapChain.
     ```
   - In-game multiplier displayed in green text:
     ```
     In-Game Multiplier: 6X (Active)
     ```
2. The user saved settings and restarted the game multiple times.
3. The user switched the game display mode to "Borderless Window", but XeFG still failed to run and the same prompt persisted.

---

## 3. Issue Validity Assessment & Root Cause Analysis

### 3.1 Defect Verification: Confirmed Architectural Incompatibility & Misleading UI Telemetry

Detailed analysis of the user's submitted screenshot and the OptiScaler codebase identified the following findings:

1. **RDR2 Running in Vulkan Mode**:
   - The top banner in the user's screenshot confirmed the active graphics API:
     ```
     Vulkan | DLSS 310.9.16 | Input: DLSS | Spoof: Off
     ```
   - Red Dead Redemption 2 supports both Vulkan and DirectX 12 APIs, but defaults to Vulkan upon installation.

2. **Intel XeSS Frame Generation (`libxess_fg.dll`) is Strictly DirectX 12 Only**:
   - Intel's XeSS Frame Generation library exclusively implements Direct3D 12 interfaces:
     - `xefgSwapChainD3D12CreateContext`
     - `xefgSwapChainD3D12InitFromSwapChainDesc`
     - `xefgSwapChainD3D12TagFrameResource`
   - Intel provides no Vulkan implementation or Vulkan swapchain presentation hooks.
   - OptiScaler's frame generation proxies (`XeFG_Dx12`, `FSRFG_Dx12`, `DLSSG_Dx12`) all intercept presentation via DXGI swapchain hooks (`CreateSwapChainForHwnd`, `IDXGISwapChain3/4`).
   - In Vulkan, presentation swapchains are created via `vkCreateSwapchainKHR` on `VkDevice`. DXGI swapchain hooks never execute, leaving `State::Instance().currentFGSwapchain == nullptr` indefinitely.

3. **Why Borderless Window Had No Effect**:
   - Changing window mode in RDR2 does not change the graphics rendering API from Vulkan to DirectX 12. The swapchain remained a Vulkan `VkSwapchainKHR`.

4. **Misleading In-Game Menu Telemetry**:
   - In `OptiScaler/menu/menu_common.cpp` (lines 3884–3888), when `currentFGSwapchain == nullptr`, OptiScaler unconditionally displayed:
     ```
     Note: Save Settings and restart the game to initialize the XeFG SwapChain.
     ```
     In Vulkan titles, saving settings and restarting will **never** initialize the swapchain.
   - Furthermore, lines 3934–3938 unconditionally printed `In-Game Multiplier: 6X (Active)` in bright green text even when `currentFGSwapchain == nullptr` and the game was running under Vulkan, falsely signaling to the user that multi-frame generation was operational.

---

## 4. Technical Remediation

1. **Vulkan Contextual Detection (`OptiScaler/menu/menu_common.cpp`)**:
   - Evaluated `const bool isVulkan = (state.swapchainApi == API::Vulkan || state.api == API::Vulkan);` in the XeMFG menu subsection.

2. **Explicit Error Banner (Excluding Game Examples)**:
   - When `isVulkan` is true, the misleading restart note is replaced with a clear, prominent red error banner:
     ```cpp
     if (isVulkan)
     {
         ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.35f, 0.35f, 1.0f)),
                            "Error: Game is running in Vulkan mode. Intel XeSS Frame Generation (XeFG) only "
                            "supports DirectX 12.\n"
                            "If the game supports DirectX 12, switch the Graphics API to DirectX 12 in the game "
                            "settings and restart.");
     }
     ```

3. **Checkbox Disablement on Vulkan**:
   - When `!xeUnlock && isVulkan`, the `Enable XeMFG Unlocker` checkbox is disabled with an explanatory tooltip informing the user that DirectX 12 is required.

4. **Accurate In-Game Multiplier Status Guarding**:
   - If `isVulkan`: Displays `In-Game Multiplier: %dX (Inactive - DirectX 12 Required)` in red.
   - Else if `state.currentFGSwapchain == nullptr`: Displays `In-Game Multiplier: %dX (Pending SwapChain)` in orange.
   - Only when `state.currentFGSwapchain != nullptr`: Displays `In-Game Multiplier: %dX (Active)` in green.

5. **User Action in RDR2**:
   - To activate XeFG in *Red Dead Redemption 2*, players simply switch the in-game API:
     **Settings** $\rightarrow$ **Graphics** $\rightarrow$ **Advanced Settings** (Unlocked) $\rightarrow$ **Graphics API**: switch from **Vulkan** to **DirectX 12**, apply, and restart.

---

## 5. Verification & Unit Testing

- **Unit Test**: `tests/xemfg_vulkan_diagnostics_unit.cpp`
  - Case 1: Verifies Vulkan API detection across `state.swapchainApi` and `state.api`.
  - Case 2: Verifies checkbox disablement and tooltip text when `!xeUnlock && isVulkan`.
  - Case 3: Verifies explicit error banner generation under Vulkan, confirming absence of hardcoded game examples and presence of DX12 requirement.
  - Case 4: Verifies multiplier status resolution across Vulkan (`Inactive - DirectX 12 Required`), pending swapchain (`Pending SwapChain`), and active swapchain (`Active`).
- **Standards Verification**:
  - Zero `clang-format` warnings (`clang-format --dry-run --Werror`).
  - Mandatory UTF-8 BOM (`\xef\xbb\xbf`) verified across all modified files.
