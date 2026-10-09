# Intel XeMFG PR #21 Regression Resolution: Native Presentation Thunk Pacing, Live Multipliers, and Swapchain Lifecycle

## 1. Overview & Context

- **Operating System**: Windows 11 x64 / Windows 10 x64.
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll`) with OptiScaler XeMFG multi-frame unlocker.
- **Affected Games**:
  - *Resident Evil Requiem* (`re9.exe`, RE Engine, Direct3D 12)
  - *The Last of Us Part II Remastered* (`tlou2.exe`, Direct3D 12)
- **Observed Log Files**:
  - `OptiScaler_RE-test15.log`
  - `OptiScaler_TLOU2-test2.log`
- **Originating PR**: PR #21 (commit `15be4405`, merging contributor commits `da7ba14b` and `12643451`).

---

## 2. Reported Symptoms

Following the merge of PR #21 into the repository, real-world engine testing revealed two distinct sets of critical regressions:

### 2.1 Resident Evil Requiem (`OptiScaler_RE-test15.log`)
- While increasing the XeFG multiplier in the menu or game increased the reported FPS counter (e.g. 180 FPS on 3X, 240 FPS on 4X), the actual visual motion on screen was completely identical to XeFG OFF (30/60 FPS).
- Intermediate generated frames were not visibly smoothing motion.
- Pacing logs showed:
  `XeMfgLoader::TryApply: native provider presentation deadline pacing status: detours=0, verified=false`
- `constData.frameRenderTime` fed to Intel was repeatedly logged as `0.0f`.

### 2.2 The Last of Us Part II (`OptiScaler_TLOU2-test2.log`)
- `XeMfgLoader::TryApply` reported `detours=0, verified=false`.
- The in-game ImGui menu multiplier slider (`Max Generated Frames##xemfg`) became completely ineffective at runtime; changing the slider did not alter the active in-memory ceiling or multiplier.
- When DLSS-G mode transitioned to `eOff` (during cutscenes, menus, or when toggled off by the user), the log flooded with continuous warnings on every single frame:
  `WARNING: result=-12 (XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES), framesPresented=1, enabled=1`
- Upon returning to gameplay and re-enabling FG, the engine logged:
  `Missing resource of type XEFG_RES_MOTION_VECTOR (-4)`.
- Dynamic Multi-Frame Generation (DMFG) was rejected by Streamline capability queries.

---

## 3. Issue Validity Assessment & Root Causes

All reported symptoms were confirmed as **genuine architectural defects** introduced by PR #21:

### 3.1 Root Cause 1: Native Thunk Pacing Engine Gated Behind Optional DXGI VBlank Flag
- **Defect Mechanism**:
  In commit `da7ba14b`, the PR author conflated two fundamentally different concepts:
  1. The outer DXGI swapchain VBlank lock (`SyncInterval=1`, stripping tearing in `FG_Hooks.cpp`), which is an optional display feature and correctly defaults to `ExtraPacing=false`.
  2. The core native thunk pacing engine (`XeFGPacing.h`), which detours Intel's internal presentation loop (`0x25C0`, `0x3100`, `0x3430`), spaces out generated frames in time, and computes decoupled frame render times.
- The author modified `XeMfgLoader.cpp` and `XeFGPacing.h` to gate `XeFGPacing::Install` behind `ExtraPacing.value_or_default()`.
- Because `ExtraPacing` defaults to `false`, `XeFGPacing::Install` was **never called** (`detours=0, verified=false`).
- Without `XeFGPacing`:
  - `libxess_fg.dll` dumped all generated frames in the burst back-to-back in < 1 ms unmetered clumps.
  - `XeFGPacing::RenderTimeMs()` returned `0.0`. In RE Requiem, `_ftDelta` is `0.0`, so `constData.frameRenderTime` fed to Intel was `0.0f`.
  - Fed with `0.0f` delta time, Intel's model could not extrapolate motion vectors, outputting duplicated frames. The eye perceived 1 visual frame per game tick (identical to XeFG OFF), while the counter registered multiple Present calls.

### 3.2 Root Cause 2: Destruction of Live In-Memory Ceiling Adaptation
- **Defect Mechanism**:
  In commit `da7ba14b`, the author gutted `XeMfgLoader::SetMaxGeneratedFrames`, adding an early `return` if `g_applied` was true ("Keep both code and advertised capability fixed for this session") and deleting the live byte rewriting of `U3/default-ceiling`, `U4/override-clamp`, and `U5/reported-maximum` via `VirtualProtect`.
- In `menu_common.cpp`, the call to `SetMaxGeneratedFrames` from the slider was removed, and false help text stating "Save Settings and restart after changing" was added.
- In `EnabledForSession()`, the return value was latched with `static const bool`, preventing dynamic activation from the menu without restarting the game.

### 3.3 Root Cause 3: Swapchain Passthrough Missing Hardware Deactivation
- **Defect Mechanism**:
  In `XeFG_Dx12::SetInterpolatedFrameCount(0)`, OptiScaler entered passthrough mode (`_passthrough = true`) but **never called** `XeFGProxy::SetEnabled()(_swapChainContext, false)` or `ClearAllResourceReady()`.
- Intel's proxy remained enabled in hardware (`enabled=1`), expecting motion vectors that the game stops rendering when FG is disabled.
- On every single frame, Intel intercepted the swapchain Present, looked for motion vectors, found none, and emitted `WARNING: result=-12 (XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES)`.
- On re-enabling, stale resource states caused `Missing resource of type XEFG_RES_MOTION_VECTOR (-4)`.

### 3.4 Root Cause 4: Dynamic MFG Capability Gate Blocked for Emulation
- **Defect Mechanism**:
  Commit `12643451` restricted `canEnableDynamic` and `bIsDynamicMFGSupported` strictly to `fg->GetDMFGSupport()`. For XeFG, `_supportsDMFG` is hardcoded to `false` in `IFGFeature.cpp`. This disabled Dynamic Multi-Frame Generation for XeSS-G entirely.

---

## 4. Applied Solutions

### 4.1 Restoring Unconditional Native Thunk Pacing Engine
- **`OptiScaler/framegen/xefg/XeFGPacing.h`**:
  - Removed `#include "Config.h"`.
  - Removed the `!config->XeMfgExtraPacing.value_or_default()` gate from `XeFGPacing::Install()`.
- **`OptiScaler/framegen/xefg/XeMfgLoader.cpp`**:
  - In `XeMfgLoader::TryApply()`, removed reading `XeMfgExtraPacing` for native pacing and passed `enablePacing = true` unconditionally to `ApplyToMemory()`.
- **`OptiScaler/hooks/FG_Hooks.cpp`**:
  - Preserved `XeMfgExtraPacing` exclusively for controlling the DXGI swapchain VBlank lock (`SyncInterval=1`, stripping tearing).

### 4.2 Restoring Live In-Memory Ceiling & Dynamic Multiplier Adaptation
- **`OptiScaler/framegen/xefg/XeMfgLoader.cpp`**:
  - Restored dynamic memory byte modification for patch sites `U3/default-ceiling` (offset 1), `U4/override-clamp` (offset 6), and `U5/reported-maximum` (offset 1) using `VirtualProtect` (`PAGE_EXECUTE_READWRITE`), modifying bytes, restoring page protection, and flushing CPU instruction caches with `FlushInstructionCache`.
  - Made `EnabledForSession()` evaluate dynamically without `static const bool`.
- **`OptiScaler/menu/menu_common.cpp`**:
  - Reconnected `Max Generated Frames##xemfg` slider to invoke `XeMfgLoader::SetMaxGeneratedFrames(static_cast<uint32_t>(maxFrames))` directly when `XeMfgLoader::IsEnabled()`.
  - Restored accurate label `"Display VBlank Sync (Extra Pacing)##xemfg"` and descriptive help text.

### 4.3 Clean Swapchain Deactivation on `eOff`
- **`OptiScaler/framegen/xefg/XeFG_Dx12.cpp`**:
  - In `SetInterpolatedFrameCount(0)`, explicitly invoked `XeFGProxy::SetEnabled()(_swapChainContext, false)`, set `_isActive = false`, and called `ClearAllResourceReady()`.
  - When returning to count $> 0$, `SetInterpolatedFrameCount` and `Activate()` re-enable `XeFGProxy::SetEnabled()(_swapChainContext, true)` and prime state cleanly.
- Completely eliminated the `-12` warning flood and `-4` missing motion vector errors.

### 4.4 Dynamic Multi-Frame Generation (DMFG) Advertising
- **`OptiScaler/hooks/Streamline_Hooks.cpp`**:
  - In `hkslDLSSGGetState()`, evaluated `dynamicSupported` as true when `activeFgOutput == FGOutput::XeFG && XeMfgLoader::EnabledForSession()`.
  - Advertised `state.bIsDynamicMFGSupported = sl::Boolean::eTrue` and updated `State::Instance().dlssgGameDMFGSupported = true`, unblocking `canEnableDynamic` in `hkslDLSSGSetOptions`.
  - Updated `dummy_slDLSSGGetState()` to advertise both `numFramesToGenerateMax` and `bIsDynamicMFGSupported`.

---

## 5. Automated Unit Test Verification

All test suites were updated and compiled with `-std=c++20`:

1. **`tests/xemfg_loader_unit.cpp` (11/11 Passed)**:
   - Test 1: Fast-path RVA parameter unlock patch application.
   - Test 2: Full rollback restores byte-for-byte image identity.
   - Test 3: Relocated signature scanning fallback and 6X multiplier.
   - Test 4: Atomic rollback on patch failure.
   - Test 5: Multiplier range clamping (1 to 5).
   - Test 6: Safe lifecycle teardown restores pristine image.
   - Test 7: Presentation telemetry recording & lifecycle reset.
   - Test 8: Native provider presentation deadline calculation.
   - Test 9: Native XeFGPacing thunk hooking, scheduler routing, and deadline repair.
   - **Test 10 (New)**: Live in-memory ceiling adaptation byte rewriting (`U3`, `U4`, `U5`).
   - **Test 11 (New)**: Mandatory native presentation pacing detours installed unconditionally.

2. **`tests/xefg_mfg_streamline_unit.cpp` (8/8 Passed)**:
   - Test 1: Streamline GetState capability advertising and safety clamping.
   - Test 2: Streamline SetOptions multiplier routing and XeFG execution.
   - Test 3: In-game disable enters passthrough mode.
   - Test 4: Re-enabling Multi-Frame Generation resumes smoothly from passthrough.
   - Test 5: Strict mutual exclusion across Ada, Ampere, and XeMFG.
   - Test 6: Startup in passthrough and premature activation prevention.
   - Test 7: Streamline SetOptions transition gating and state deduplication.
   - **Test 8 (New)**: Dynamic Multi-Frame Generation (DMFG) advertising & clean swapchain deactivation on `eOff`.

3. **`tests/xemfg_config/ConfigTests.cpp`**:
   - Updated assertions in `default-off`, `saved-off`, and `explicit-on` to verify unconditional pacer installation (`InstalledDetourCount() == 3u`).
   - Updated assertions in `session-unlock-off` to verify dynamic session admission.
   - Updated assertions in `ceiling-live` to verify dynamic in-memory ceiling modification.

4. **Code Quality & Encoding Standards**:
   - Verified UTF-8 BOM (`\xef\xbb\xbf`) intact on all modified files.
   - Verified `clang-format --dry-run --Werror` passes with 0 violations.
