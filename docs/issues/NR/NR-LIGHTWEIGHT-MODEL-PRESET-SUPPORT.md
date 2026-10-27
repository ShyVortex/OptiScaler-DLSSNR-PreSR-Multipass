# DLSS-NR: DLSS 5 Lightweight Model ("Crazy Cuckoo Small FP8") Support & Preset Integration

## Overview & Environment

- **Operating System**: Linux 7.2.9-2-cachyos (CachyOS)
- **GPU**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere, GA103 / SM86)
- **NVIDIA Driver**: 580.178.04
- **Analyzed Binary**: `nvngx_dlssnr.dll` (v310.9.16.0, CL 39025366, DVS Production)
- **Target Subsystem**: DLSS Neural Rendering (DLSS-NR / Pre-SR / Multipass)

In newer DLSS 5 (Neural Rendering) releases, NVIDIA introduced a secondary, highly optimized lightweight neural network model designed to drastically reduce inference time on Tensor Cores (yielding **up to a 40% performance improvement**) compared to the full-precision teacher model.

This document details the reverse engineering of the patched `nvngx_dlssnr.dll` binary, the architectural mapping of the embedded weight sets, and the enhancements made to OptiScaler to expose and control these models seamlessly at runtime.

---

## 1. Binary Reverse Engineering of `nvngx_dlssnr.dll` (v310.9.16.0)

A binary comparison between the unpatched shipping library and the patched community binary (`/home/angelo/Scaricati/nvngx_dlssnr.dll`) identified the following structural modifications:

### 1.1 Architecture Unlocking & Embedded Kernel Payloads
- **Architecture Minimum Gate**: The checks at RVA `0x136e4` (Direct3D 11) and RVA `0x15584` (Direct3D 12) originally required Blackwell 2 (`0x1b0`) or newer. The patch lowered this constraint to Turing (`0x160`), permitting execution on RTX 20, 30, and 40 series hardware.
- **Hardware Capability Jump Table**: The capability evaluation jump table at RVA `0x18144-0x18150` was redirected to `0x180017c2b`, preventing the driver from rejecting SM75 (Turing), SM86 (Ampere), or SM89 (Ada) architectures.
- **Embedded Section `.nrcubin`**: A dedicated section (~9.7 MB) was appended containing precompiled CUDA binaries for SM75, SM86, SM89, etc., with pointers patched at RVA `0x605f6-0x60695`.

### 1.2 Descriptor Table & Neural Network Models (`0x1800dbe30` & `0x23c20`)
Disassembly of `CG2RNetworkManager::CreateFeature` (RVA `0x17cfd`), `CG2RNetworkManager::ResolvePresetToDescriptor` (RVA `0x21850`), and `CG2RFindWeightByPreset` (RVA `0x23c20`) revealed an internal descriptor table containing two distinct models:

| Preset | Model Weight Identifier | Weight Tag | Backbone Architecture | Characteristics & Performance |
| :--- | :--- | :--- | :--- | :--- |
| **Preset 1** | `CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights` | `WEIGHTS_HT` | `CC_SILVER_AARDWOLD` | **Quality Model ("Honest Tench")**: Full-precision teacher quantized network. Highest detail reconstruction and fidelity; standard compute cost. |
| **Preset 2** | `hnet-crazy-cuckoo-small-fp8` | `WEIGHTS_CC_SMALL_FP8` | `CC_SMALL_FP8` | **Lightweight Model ("Crazy Cuckoo Small FP8")**: Uses 8-bit floating point (**FP8**) Tensor Core quantization and reduced channel width. **Up to 40% faster inference execution**. |
| **Preset 0** | Hardcoded dispatch fallback to **Preset 2** | `WEIGHTS_CC_SMALL_FP8` | `CC_SMALL_FP8` | **Auto (Default)**: In `CG2RFindWeightByPreset` (RVA `0x23c47-0x23c73`), if `preset == 0` or is unspecified, execution defaults directly to `hnet-crazy-cuckoo-small-fp8`. |

---

## 2. Issue Validity & Codebase Assessment

### 2.1 Backend Readiness
OptiScaler's low-level feature creation pipeline was already equipped to pass preset hints:
- `Config.h`: Declared `CustomOptional<uint32_t> DlssNrPreset { 0 };` and `NrPassOverrides::preset`.
- `PassProfiles.h`: Bound `preset` into `ModelSettings` with `std::min(preset, 3u)`.
- `DlssNr_Proxy.cpp` (DX12) & `DlssNrFeature_Vk_Model.cpp` (Vulkan): Passed `DLSSNR.Hint.Render.Preset` before invoking NGX `CreateFeature`.
- `DlssNr_Dx12_Models.cpp`: `TuningMatchesFeature` tracked `nr.builtSettings[pass] != PassSettings(cfg, pass)`. Because `ModelSettings::operator==` compares `preset`, modifying the preset automatically triggered a guarded recreation of the NGX feature without leaking resources.

### 2.2 Existing Deficiencies
1. **Omission in ImGui In-Game Menu (`OptiScaler/dlssnr/DlssNr_MenuControls.cpp`)**:
   `RenderModel()` only rendered `Model passes`, `Style` (Standard, Natural, Cinematic), and tuning sliders. The `Model preset` control was completely missing from the UI, preventing users from selecting or experimenting with the new FP8 model while in-game.
2. **Ambiguous Configuration Documentation (`OptiScaler.ini`)**:
   `OptiScaler.ini` documented presets merely as "undocumented numbers 0..3", without explaining the Quality vs Performance FP8 tradeoff or the 40% speedup.
3. **Multi-Pass Limitation**:
   `PassProfiles.h` and `Config.cpp` artificially limited preset overrides to `pass <= 2` (Pass 2 and Pass 3 only).

---

## 3. Applied Architecture Solutions

### 3.1 In-Game ImGui Menu Preset Selector (`DlssNr_MenuControls.cpp`)
`RenderModel()` uses one `NrModelCombo` for every pass. The labels come from upstream 6125a75 and live in
`dlssnr/DlssNr_MenuPresets.h` (no ImGui dependency, so tests include it directly):
- **Pass 1 (Primary Model)**: `0` `"Automatic"` (runtime default, Small FP8 on 310.9), `1` `"Original"`
  ("Honest Tench"), `2` `"Optimized"` ("Crazy Cuckoo Small FP8").
- **Pass 2..10 (Multipass Layers)**: `"Inherit pass 1"` (unset), then the same three explicit presets.
- Out-of-range INI values preview as `"Unavailable preset"` instead of being clamped silently.

> **Update (PR #30 review):** The selective upstream merge (6125a75) brought in upstream's combo behind a
> `selectedPass <= 2` gate, which showed passes 3..10 as `"NR model: inherits pass 1"` even when a
> `Pass{N}Preset` was configured. The gate was removed so every pass has the combo again. Upstream's combo also
> fixed an off-by-one in the fork's earlier per-pass combo, where `"Quality (Honest Tench)"` stored preset `0`
> (Automatic) and `"Performance"` stored `1`. Guarded by `tests/nr_model_preset_menu_unit.cpp`.

### 3.2 Multi-Pass Generalization (`PassProfiles.h` & `Config.cpp`)
- In `PassProfiles.h`, removed the legacy `pass <= 2` restriction so that any pass with an explicit `extra.preset` override applies it cleanly.
- In `Config.cpp`, extended `Pass{}Preset` loading and saving across all configured multi-pass layers.

### 3.3 Configuration Documentation (`OptiScaler.ini`)
Updated the `[DlssNr]` section to clearly describe Preset `0` (Auto / Small FP8), Preset `1` (Quality / Honest Tench), and Preset `2` (Performance / Crazy Cuckoo Small FP8), along with per-pass overrides.

---

## 4. Test Verification

Automated unit test coverage was implemented in `tests/nr_model_preset_unit.cpp`, covering 8 distinct scenarios:
1. **Default Preset Resolution**: Verifies that default config resolves to Preset `0` (Auto).
2. **Preset 1 (Quality)**: Verifies mapping to `"Honest Tench"` full-precision weights.
3. **Preset 2 (Performance)**: Verifies mapping to `"Crazy Cuckoo Small FP8"` (+40% FPS).
4. **Clamping**: Verifies that out-of-range presets (> 3) clamp safely to 3.
5. **Multi-Pass Inheritance**: Verifies that Pass 2 and Pass 3 cleanly inherit Pass 1 preset when unconfigured.
6. **Per-Pass Overrides**: Verifies independent preset assignments across Pass 1 (Performance), Pass 2 (Quality), Pass 3 (Performance), and Pass 4 (Quality).
7. **Feature Rebuild Invariant**: Asserts that `ModelSettings::operator==` detects preset transitions and reports inequality to trigger guarded context recreation.
8. **UI Combo Index Mapping**: Validates zero-offset and inheritance combo index conversions.

### Menu Coverage
`tests/nr_model_preset_menu_unit.cpp` (6 cases) checks the production labels and previews, and that no pass
gate comes back in `DlssNr_MenuControls.cpp`.

### Regression Test Suite Results
All test suites compiled cleanly with C++20 and passed with zero errors:
- `tests/nr_model_preset_unit.cpp`: **PASSED** (8/8 cases)
- `tests/nr_placement_config_unit.cpp`: **PASSED**
- `tests/nr_pipeline_setup_unit.cpp`: **PASSED**
- `tests/nr_buffer_resource_unit.cpp`: **PASSED** (6/6 cases)
- `tests/nr_status_reporting_unit.cpp`: **PASSED** (3/3 cases)
- `tests/nr_format_bouncing_unit.cpp`: **PASSED** (6/6 cases)
- `clang-format --dry-run --Werror`: **PASSED** (clean style compliance)
- UTF-8 BOM Verification (`\xef\xbb\xbf`): **PASSED** across all modified files.
