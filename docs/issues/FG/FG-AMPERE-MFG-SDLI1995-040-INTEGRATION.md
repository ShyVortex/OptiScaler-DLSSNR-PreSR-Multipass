# Frame Generation: Turing/Ampere SM86 MFG v0.4.0 Modernization & DLSS-G Preset C (FG Lite) Integration

## 1. Overview & Environment

- **Operating System**: CachyOS Linux (Kernel `7.2.9-2-cachyos`, Arch-based rolling) / Windows 11 build 26100+
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86, GA103, 16384 MiB)
- **Driver**: NVIDIA Display Driver 580.178.04 / CUDA 13.0
- **Mod Configuration**: OptiScaler with `sdli1995/dlssg_for_sm86` v0.4.0
- **Report Reference**: [DLSS Unlocked Issue #52](https://github.com/ShyVortex/dlss-unlocked/issues/52)
- **Upstream Release**: [sdli1995/dlssg_for_sm86 v0.4.0](https://github.com/sdli1995/dlssg_for_sm86/releases/tag/0.4.0)

---

## 2. Reported Need & Feature Request

Upstream author `sdli1995` released version **0.4.0** of `dlssg_for_sm86` (the foundational Turing/Ampere Multi-Frame Generation mod). Following this release, community members requested updating OptiScaler's integration from the older, inactive `SilyNoMeta` fork (pinned at v0.3.5-5) to official `sdli1995` v0.4.0:

1. **DLSS-G Preset C (FG Lite) Support**:
   - In DLSS Unlocked issue #52, users specifically requested exposing and configuring the new **Preset C (FG Lite)** introduced in version 0.4.0, which optimizes candidate blending to dramatically reduce GPU execution time per generated frame.

2. **Packaging & Release Pipeline Staleness**:
   - The automated release packaging script (`package_release.ps1`) and CI workflow (`package_release.yml`) were still cloning from `SilyNoMeta/dlssg_for_sm86.git` tag `v0.3.5-5`.
   - Release packages lacked the new modular binaries and runtime files required for 0.4.0 execution.

---

## 3. Reverse Engineering & Binary Assembly Findings

A comprehensive reverse engineering analysis of `version.dll` (833 KB proxy) and `dlssg_sm86\backend\sm86_backend.dll` (v0.4.0 backend) using `llvm-objdump`, `pefile`, and string cross-references revealed significant architectural evolution in 0.4.0:

### 1. Modular Directory Layout & Subdirectory Dependencies
- `version.dll` no longer bundles or unpacks all CUDA kernels and runtime DLLs into memory or temporary directories.
- Instead, it expects all execution binaries to reside in a **`dlssg_sm86\`** subdirectory relative to the proxy (or next to the game executable):
  - `dlssg_sm86\backend\sm86_backend.dll` (core execution bridge)
  - `dlssg_sm86\kernels\fg_kernels.dll` (71 compiled CUDA kernels)
  - `dlssg_sm86\runtime\nvngx_dlssg.dll` (v310.9.16.0 NGX runtime)
  - `dlssg_sm86\xefg\xefg_kernels.dll` (XeSS-FG runner)
  - `dlssg_sm86\sr\` (Super Resolution INT8 runtime & kernels)
- **Critical Failure Mode**: If only `version.dll` is staged (as older 0.3.x packagers did), `version.dll` logs `backend_unusable` (`Bad Image 0xc0e90002` or `backend file could not be loaded`) and cleanly aborts frame generation. Staging the entire `dlssg_sm86\` folder is mandatory.

### 2. Assembly Analysis of Preset C (FG Lite)
- In `version.dll`:
  - `[DLSSG] Preset` is read. Values map to integer preset IDs:
    - `0`: Auto (let driver/profile decide)
    - `1`: Preset A (force UI recomposition off)
    - `2`: Preset B (force UI recomposition on)
    - `3`: **Preset C (FG Lite)**
- In `sm86_backend.dll`:
  - At `SM86Bridge_Install` (RVA `0x52c72`):
    - Bits 25..26 of `ModInstallParams->flags` extract the preset ID: `(flags >> 25) & 3` and store it at global `0x1801f1c40`.
  - At `hookUirParam` (RVA `0x39e71`):
    - When `presetId == 3`, the engine executes the Preset C branch: pins `liteGroup`, swaps candidate blending kernel from stock to `Kernel_BlendCandidatesFGLite`, and writes telemetry record `preset_pinned` with `lite_effective: true`.
    - **Performance Impact**: Benchmark logs confirm this produces a 30% to 54% reduction in GPU execution time per generated frame, significantly mitigating GPU frame generation overhead on midrange Ampere GPUs (e.g. RTX 3060, RTX 3070).

### 3. INI Section Relocation & Dual-Compatibility Contract
- 0.4.0 relocated `Optimized` and `Preset` from `[FrameGeneration]` and `[Compatibility]` to **`[DLSSG]`**.
- Legacy keys under `[FrameGeneration] Optimized` and `[Compatibility] Preset` are supported as fallback, but emit configuration warnings.
- The default `Optimized` setting in 0.4.0 is `2` (Tier 2: bit-identical + fast lossy image kernels).

---

## 4. Applied Solution & Architecture

### 1. Configuration & INI Formatter Backend (`Config.h`, `Config.cpp`, `AmpereMfgLoader.h`)
- In `OptiScaler/Config.h`: Updated `FGDLSSGAmpereMfgPreset` docstring to document `Auto / A / B / C`.
- In `OptiScaler/Config.cpp`: Added parsing for `"C"` and `"c"` from `[DLSSG] AmpereMfgPreset` to set `FGDLSSGAmpereMfgPreset.set_from_config("C")`.
- In `OptiScaler.ini`: Documented `AmpereMfgPreset = c` (FG Lite: fast / 30–50% less GPU time on 0.4.0+).
- In `OptiScaler/framegen/dlssg/AmpereMfgLoader.h`:
  - In `FormatIniContent030`: Validated `"C"` / `"c"` and normalized to `"C"`.
  - Implemented dual-section generation: generates both the new `[DLSSG]` section (`Optimized`, `Preset`) and legacy sections (`[FrameGeneration] Optimized`, `MaxGeneratedFrames`, `Engine=DLSS`, and `[Compatibility] Preset`, `Router`), ensuring 100% interoperability across older 0.3.x runtimes and 0.4.0.

### 2. In-Game Menu GUI Integration (`OptiScaler/menu/menu_common.cpp`)
- Expanded `presetOptions` combo in the Ampere MFG subsection:
  - Added `"Preset C (FG Lite: fast / 30-50% less GPU time)"`.
  - Updated `presetIdx` calculation:
    ```cpp
    int presetIdx = (currentPreset == "A" || currentPreset == "a")   ? 1
                    : (currentPreset == "B" || currentPreset == "b") ? 2
                    : (currentPreset == "C" || currentPreset == "c") ? 3
                                                                     : 0;
    ```
  - Updated `storedPresetOptions` to include `"C"`.
  - Updated `ShowHelpMarker` to detail Preset C (FG Lite candidate blending).

### 3. Packaging & CI Workflow Modernization (`package_release.ps1`, `package_release.yml`)
- In `.github/workflows/package_release.yml`:
  - Switched clone origin from `SilyNoMeta/dlssg_for_sm86.git` to `https://github.com/sdli1995/dlssg_for_sm86.git`.
  - Updated fetch target to `tags/0.4.0`.
- In `package_release.ps1`:
  - Prioritized `dlssg_for_sm86\sdli1995\version.dll` over stale `SilyNoMeta` paths.
  - Dynamically cloned/fetched `sdli1995/dlssg_for_sm86.git` at tag `0.4.0`.
  - Recursively staged the modular `dlssg_sm86\` directory into `$sm86DestDir\dlssg_sm86` (`backend`, `kernels`, `runtime`, `sr`, `xefg`).
  - Staged documentation files: `MANUAL.en.html`, `manifest.json`, and `validation.json`.

---

## 5. Verification & Test Suite Results

1. **INI Formatting & Configuration Unit Tests (`tests/dlssg_sm86_v030_ini_unit.cpp`)**:
   - **Case 4**: Verified case-insensitive validation and normalization of `Auto`, `A`, `B`, and `C`.
   - **Case 16**: Verified 0.4.0 dual compatibility: generates `[DLSSG]\nOptimized=2\nPreset=C\n`, `Engine=DLSS`, and legacy `[Compatibility]\nPreset=C`.
   - Result: All 16 unit test cases **PASSED**.

2. **Modular Architecture & Variant Detection Unit Tests (`tests/dynamic_mfg_detection_unit.cpp`)**:
   - **Case 9b**: Verified real `sdli1995` 0.4.0 binary detection as clean `ModVariant::Sdli1995`.
   - **Case 9c**: Verified presence of the modular 0.4.0 directory structure (`dlssg_sm86\backend\sm86_backend.dll`, `kernels\fg_kernels.dll`, `runtime\nvngx_dlssg.dll`).
   - Result: All unit test cases **PASSED**.

3. **Compiler & Formatting Standards Compliance**:
   - All touched C++ headers and source files asserted for UTF-8 BOM (`\xef\xbb\xbf`).
   - Clean formatting verified with `docker run ghcr.io/jidicula/clang-format:20 clang-format --dry-run --Werror` (0 violations).
