# Frame Generation: Turing/Ampere SM86 MFG sdli1995 v0.4.0 Detection Hardening & Dynamic MFG False Positive Resolution

## 1. Overview & Environment

- **Operating System**: CachyOS Linux (Kernel `7.2.9-2-cachyos`, Arch-based rolling) / Windows 11 build 26100+
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86, GA103, 16384 MiB, Compute Capability 8.6)
- **Driver**: NVIDIA Display Driver 580.178.04 / CUDA 13.0
- **Mod Configuration**: OptiScaler with `sdli1995/dlssg_for_sm86` v0.4.0
- **Affected Subsystem**: Ampere MFG Loader (`OptiScaler/framegen/dlssg/AmpereMfgLoader.h`, `AmpereMfgLoader.cpp`, `package_release.ps1`, `menu_common.cpp`)

---

## 2. Reported Issue & Symptoms

During preview testing of builds integrating the new version **0.4.0** of the Turing/Ampere Multi-Frame Generation mod by `sdli1995`, the in-game ImGui overlay incorrectly reported the mod variant as `SilyNoMeta` instead of `sdli1995`:

```
Mod: SilyNoMeta | DLL: found | Router: SM86 | INI: written | Loaded: yes | Live: ready
```

### Consequences:
1. **False-Positive Dynamic Multi-Frame Generation (DMFG) Exposure**:
   Because `Variant` was misclassified as `ModVariant::SilyNoMeta`, `s_status.HasDynamicMfgSupport` evaluated to `true`.
2. **Interactive UI Toggle for Unsupported Feature**:
   The Dynamic Multi-Frame Generation checkbox in `OptiScaler/menu/menu_common.cpp` became active and interactive rather than remaining disabled (greyed out).
3. **Runtime Instability & Configuration Corruption**:
   `sdli1995` v0.4.0 does not export or support the `SilyNoMeta` runtime control APIs (`DLSSG_RequestControl`, `DLSSG_SetDisplayTarget`, or dynamic target FPS loops). Toggling Dynamic MFG caused OptiScaler to invoke null/stale control functions or write incompatible parameters, resulting in unpredictable behavior and crashes.

---

## 3. Issue Validity Assessment & Root Cause Autopsy

**Status: Confirmed Valid (Architectural Root Cause Identified).**

A comprehensive source code and assembly audit of `AmpereMfgLoader.h`, `AmpereMfgLoader.cpp`, and `tests/dynamic_mfg_detection_unit.cpp` uncovered two severe circular feedback loops and missing binary markers:

### 1. Circular `ReShade.ini` Heuristic in `DetectModVariant`
In `AmpereMfgLoader.h`, `DetectModVariant` previously checked whether a `ReShade.ini` file existed beside the proxy DLL and inspected it for `[DLSSG-SM86-75-COMPANION]`:
```cpp
// Flawed legacy heuristic in DetectModVariant:
std::filesystem::path reshadeIni = dllPath.parent_path() / L"ReShade.ini";
if (std::filesystem::exists(reshadeIni, ec)) {
    // ...
    if (line.find("DLSSG-SM86-75-COMPANION") != std::string::npos) {
        if (outModName) *outModName = "SilyNoMeta";
        return ModVariant::SilyNoMeta;
    }
}
```
- **The Circular Loop**: OptiScaler's own companion writer (`AmpereMfgLoader::WriteCompanionIni()`) generates `[DLSSG-SM86-75-COMPANION]` in `ReShade.ini` whenever companion settings are synchronized.
- Furthermore, `package_release.ps1` previously staged `ReShade.ini` directly into `OptiScaler/dlssg_sm86/ReShade.ini`.
- Whenever `ReShade.ini` was present, `DetectModVariant` immediately short-circuited and declared the mod to be `ModVariant::SilyNoMeta`, completely ignoring the actual binary code of `version.dll`!

### 2. Substring Match Pollution in `HasDynamicMfgSupport`
In `AmpereMfgLoader.h`, `HasDynamicMfgSupport` previously inspected `dlssg_sm86.ini`:
```cpp
// Flawed legacy heuristic in HasDynamicMfgSupport:
if (line.find(needleDynamicMfg) != std::string::npos || ...) return true;
```
- OptiScaler's `FormatIniContent030` writes `DynamicMFG=0` (or `DynamicMFG=1`).
- The substring search for `"DynamicMFG"` matched `DynamicMFG=0`!
- Consequently, once OptiScaler wrote an INI file for any mod, the mere existence of `DynamicMFG=0` caused `HasDynamicMfgSupport` to return `true` on subsequent runs, enabling the Dynamic MFG menu toggle on pure `sdli1995` runtimes.

### 3. Missing Dedicated Binary Signatures for sdli1995 v0.4.0
- In version 0.4.0, author `sdli1995` completely restructured the mod, modularizing the architecture into `dlssg_sm86/backend/sm86_backend.dll`, `kernels/fg_kernels.dll`, and `runtime/nvngx_dlssg.dll`.
- Legacy 0.3.0 markers such as `"The 310.9 backend has no SM75"` were completely removed from `version.dll`.
- As a result, 0.4.0 binaries failed legacy binary checks and fell back to the circular INI heuristics.

### 4. Inverted Candidate Search Priority
- In `AmpereMfgLoader.cpp`, candidate discovery arrays (`rootCandidates`) checked `dlssg_for_sm86/SilyNoMeta/version.dll` before `dlssg_for_sm86/sdli1995/version.dll`.

---

## 4. Applied Solution & Architecture

### 1. Disentangling Binary Inspection from INI Files
The proxy DLL binary is the sole ground truth. Companion INI files (`dlssg_sm86.ini`, `ReShade.ini`) must never be used to declare mod variant identity or capability.
- Removed the circular `ReShade.ini` check from `DetectModVariant`.
- Removed reading `dlssg_sm86.ini` and `ReShade.ini` from `HasDynamicMfgSupport`. `HasDynamicMfgSupport` now strictly inspects the binary executable image for real Dynamic MFG code.

### 2. First-Class sdli1995 v0.4.0 Recognition (`AmpereMfgLoader.h`)
Implemented explicit detection for `sdli1995 v0.4.0`:
- **Modular Layout Verification**: Checks if `dlssg_sm86/backend/sm86_backend.dll` or `backend/sm86_backend.dll` exists beside the proxy DLL.
- **Binary Strings**: Scans for 0.4.0 markers:
  - `"Auto was removed in 0.4.0"`
  - `"sm86_backend.dll is next to the proxy"`
  - `"DlssgMod_GetInfo"`
- When matched, sets `*outModName = "sdli1995 v0.4.0"` and returns `ModVariant::Sdli1995`.
- Preserved legacy `sdli1995` detection via `"DLSSG_SM75_SLOTS"`, `"DlssgProxy_Name"`, `"DlssgProxy_Role"`, and `"The 310.9 backend has no SM75"`.
- Guarded nonexistent files with clean `ModVariant::Unknown` returns.

### 3. Strict Capability Guarding (`AmpereMfgLoader.cpp`)
Dynamic Multi-Frame Generation and live programmatic control exports are strictly exclusive to the `SilyNoMeta` fork:
```cpp
s_status.Variant = DetectModVariant(dllPath, &s_status.ModName);
s_status.HasDynamicMfgSupport = (s_status.Variant == ModVariant::SilyNoMeta) && HasDynamicMfgSupport(dllPath);
s_status.LiveControlSupported = (s_status.Variant == ModVariant::SilyNoMeta);

if (s_status.Variant == ModVariant::SilyNoMeta &&
    (s_pfnRequestControl != nullptr || s_pfnSetDisplayTarget != nullptr))
{
    s_status.LiveControlSupported = true;
}
```
If `s_status.Variant == ModVariant::Sdli1995`, `HasDynamicMfgSupport` and `LiveControlSupported` are unconditionally `false`.

### 4. Candidate Priority Realignment (`AmpereMfgLoader.cpp`)
Reordered `rootCandidates` across `ProbeCandidate` and `TrySetup` to prioritize `sdli1995` over `SilyNoMeta`:
```cpp
mainOverride / L"dlssg_for_sm86" / L"sdli1995" / L"version.dll",
mainOverride / L"dlssg_for_sm86" / L"SilyNoMeta" / L"version.dll",
```

### 5. Packaging Sanitization (`package_release.ps1`)
Removed lines 346–349 in `package_release.ps1` that staged `ReShade.ini` into `OptiScaler/dlssg_sm86/ReShade.ini`, preventing stale configuration files from polluting releases.

---

## 5. Verification & Test Suite Results

### 1. Dedicated Dynamic MFG Detection Unit Tests (`tests/dynamic_mfg_detection_unit.cpp`)
- **Case 1**: Empty and non-existent path edge cases return `false` / `ModVariant::Unknown`.
- **Case 2**: Standard `sdli1995` upstream binary correctly identified as lacking `DynamicMFG`.
- **Case 3, 4, 5, 5b, 5c**: `SilyNoMeta` forks correctly detected via `DynamicMFG`, `DynamicTargetFPS`, authorship, `DLSSG_RequestControl` export, and UTF-16LE COMPANION strings.
- **Case 6, 7**: Buffer chunk boundary crossing and UTF-16LE binary formatting verified.
- **Case 8 (Updated)**: **Immunity to Companion Pollution**: Verified that an opaque binary with `dlssg_sm86.ini` containing `DynamicMFG=0` and `ReShade.ini` containing `Dynamic=0` returns `HasDynamicMfgSupport == false`.
- **Case 9, 9b, 9c**: Real `SilyNoMeta` release binary verified as `SilyNoMeta v0.3.5`, real `sdli1995` 0.4.0 release binary verified as `sdli1995 v0.4.0`, and modular directory structure asserted.
- **Case 10 (Updated)**: Synthetic `sdli1995 v0.4.0` binary signatures verified, and immunity to companion file pollution asserted.
- **Result**: `=== All Dynamic Multi-Frame Generation (DMFG) Unit Tests PASSED! ===`

### 2. Configuration & INI Tests (`tests/dlssg_sm86_v030_ini_unit.cpp`)
- All 16 test cases passed cleanly, verifying MaxGeneratedFrames clamping, dual-compatibility formatting, and Preset C support.

### 3. Full Standalone Test Suite Verification
- All 30 standalone unit tests in `tests/` compiled and passed cleanly with zero regressions.
- All modified C++ files validated with `python3` asserting UTF-8 BOM (`\xef\xbb\xbf`) and verified clean with `clang-format --dry-run --Werror`.
