# Frame Generation: Intel Xe Multi-Frame Generation (XeMFG) Native Integration

## 1. Overview & Environment

- **Operating System**: Linux (Proton / Wine / VKD3D-Proton) & Windows 10/11 x64
- **Hardware Targets**: Intel Arc GPUs, NVIDIA GeForce GPUs (RTX 20/30/40/50), AMD Radeon GPUs (RX 6000/7000/8000)
- **Target Runtimes**: `libxess_fg.dll` / `igxess_fg.dll` (Intel XeSS Frame Generation v1.3.1+)
- **Mod Configuration**: OptiScaler with `FGInput = dlssg` and `FGOutput = xefg`
- **Issue Reference**: Native Intel XeMFG Implementation & `XeFGUnlock.asi` Plugin Port

---

## 2. Reported Issue & Symptoms

A user provided an external ASI plugin bundle in `XeMFG/` (`XeFGUnlock.asi` and `XeFGUnlock.ini`) designed to unlock Intel Xe Multi-Frame Generation (XeMFG) for OptiScaler. The following defects and limitations were observed in the field:

1. **In-Game Multiplier Control Lock**:
   - Although the plugin unlocks Multi-Frame Generation inside `libxess_fg.dll`, the multiplier cannot be selected or changed in the game's native DLSS Frame Generation display settings.
   - The game's settings menu either locks the multiplier to 2X (1 generated frame) or ignores multiplier selections, requiring the user to adjust the multiplier strictly through the OptiScaler in-game overlay menu (`FGXeFGInterpolationCount`).

2. **Immediate Crash on Disabling Frame Generation In-Game**:
   - When Frame Generation is enabled in the game menu, attempting to toggle it off in-game causes an immediate game crash or access violation (`0xC0000005`).

3. **External Plugin Burden & Missing Native Linux Support**:
   - Requiring an external ASI plugin loader and separate INI file introduces fragility across different game engines and launchers.
   - Linux users running games under Proton/Wine require an out-of-the-box native implementation that does not rely on Windows-specific external ASI loaders or fall back to 2X-only frame generation.

---

## 3. Reverse Engineering & Binary Analysis of `XeFGUnlock.asi`

Complete binary disassembly of `XeMFG/XeFGUnlock.asi` (PE32+ x86_64, image base `0x180000000`, 35,840 bytes) and cross-referencing against `libxess_fg.dll` (v1.3.1.78, 22,957,432 bytes) revealed the exact mechanism used to unlock Intel Xe Multi-Frame Generation:

### 3.1 Binary Patches in `libxess_fg.dll`

The plugin applies 5 distinct memory patches to `.text` in `libxess_fg.dll`:

| Patch ID | Target RVA | Size | Expected Bytes | Replacement Bytes | Functional Role |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`U1/frame-count-fallback`** | `0x20da4f` | 6 B | `0f 85 cc 00 00 00` (`jne +0xcc`) | `e9 cd 00 00 00 90` (`jmp +0xcd; nop`) | Bypasses the internal fallback to 1 generated frame when frame count $> 1$. |
| **`U2/model-downgrade`** | `0x1a5de4` | 2 B | `74 09` (`je +0x09`) | `eb 06` (`jmp +0x06`) | Prevents the neural rendering pipeline from downgrading model quality when generating multiple intermediate frames. |
| **`U3/default-ceiling`** | `0x1a517d` | 5 B | `bb 03 00 00 00` (`mov ebx, 3`) | `bb [MaxFrames] 00 00 00` | Rewrites the hardcoded default generated frame limit ceiling (up to 5 for 6X MFG). Dynamic byte offset at index 1. |
| **`U4/override-clamp`** | `0x1a45c2` | 10 B | `c7 87 6c 01 00 00 01 00 00 00` (`mov [rdi+0x16c], 1`) | `c7 87 6c 01 00 00 [MaxFrames] 00 00 00` | Rewrites the context structure clamp storing the maximum permitted generated frames. Dynamic byte offset at index 6. |
| **`U5/reported-maximum`** | `0x20973b` | 5 B | `b8 01 00 00 00` (`mov eax, 1`) | `b8 [MaxFrames] 00 00 00` | Replaces the return value in `xefgSwapChainGetProperties` (`props.maxSupportedInterpolations`) so OptiScaler reads up to 5. Dynamic byte offset at index 1. |

### 3.2 Presentation Pacing & Scheduler Thunks

`XeFGUnlock.asi` installs 3 hooks into `libxess_fg.dll`'s presentation pipeline:
- **Presentation Pacing Hook (RVA `0x224cf0`)**: Intercepts presentation scheduling when generated frames exceed 1 (i.e. $> 2X$ generation).
- **Internal Scheduler Thunk (RVA `0x21ee30`)**: Routes every generated frame through `libxess_fg.dll`'s internal presentation pacing scheduler.
- **Deadline Rebase Thunk (RVA `0x224b30`)**: Re-bases presentation timestamps to burst frame intervals (`target_interval = real_frame_ms / (numFrames + 1)`), preventing micro-stutter and frame bunching.

---

## 4. Root Cause Analysis

### 4.1 Root Cause: In-Game Multiplier Control Lock
1. In `OptiScaler/hooks/Streamline_Hooks.cpp` (`StreamlineHooks::hkslDLSSGGetState`, lines 1394–1396):
   ```cpp
   if (optiState.activeFgInput == FGInput::DLSSG)
   {
       ...
       // Struct version 1 ends at 56 bytes, ahead of this field.
       if (originalStructVersion >= 2)
           state.numFramesToGenerateMax = 1;
   }
   ```
   OptiScaler unconditionally forced `state.numFramesToGenerateMax = 1` whenever `activeFgInput == FGInput::DLSSG`. The game engine queries this value to populate its in-game menu; receiving `1` forces the game UI to hide or disable any options above 2X.
2. In `StreamlineHooks::hkslDLSSGSetOptions`:
   When the user selected a multiplier in games supporting Streamline MFG (e.g. 3X, 4X), OptiScaler accepted `newOptions.numFramesToGenerate`, but **never forwarded** it to the active FG feature (`currentFG->SetInterpolatedFrameCount(...)`).
3. In `OptiScaler/framegen/xefg/XeFG_Dx12.cpp` (line 1037):
   ```cpp
   bool XeFG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }
   ```
   `XeFG_Dx12` defined `SetInterpolatedFrameCount` as a no-op stub returning `true`, and only updated its internal interpolation count from `Config::Instance()->FGXeFGInterpolationCount`.

### 4.2 Root Cause: Crash on Disabling FG In-Game
1. In `XeFG_Dx12.cpp`:
   During swapchain creation, `XeFGProxy::D3D12InitFromSwapChainDesc` wraps the DXGI swapchain.
2. When the game disables FG (`sl::DLSSGMode::eOff`), it immediately halts `slEvaluateFeature(eFeatureDLSSG)` and ceases tagging resources (depth, motion vectors, HUDless).
3. In `XeFG_Dx12::Present()`:
   ```cpp
   if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
   {
       LOG_DEBUG("Pausing FG");
       Deactivate();
       _waitingNewFrameData = true;
       return false;
   }
   ```
   After 3 frames without input data, OptiScaler calls `Deactivate()`, which invokes `XeFGProxy::SetEnabled(false)` and sets `_isActive = false`.
4. In `OptiScaler/hooks/FG_Hooks.cpp` (`FGHooks::FGPresent`):
   When `_isActive` is false, `fg->Present()` is completely skipped. OptiScaler calls `o_FGSCPresent(This, SyncInterval, Flags)` directly on the wrapped swapchain.
5. Because `libxess_fg.dll` wrapped the swapchain, its internal presentation hook expects valid tagged constants and active context structures. Calling `Present` on the wrapped swapchain without tagging or with `SetEnabled(false)` while pacing thunks are hooked caused `XeFGUnlock.asi`'s pacing hook to access uninitialized or null state, dividing by zero (`real_frame_ms / 0`) or dereferencing invalid pointers, causing an immediate crash.

---

## 5. Technical Architecture & Applied Solution

### 5.1 Native `XeMfgLoader` (`OptiScaler/framegen/xefg/XeMfgLoader.h` & `.cpp`)
- Replaces the external `XeFGUnlock.asi` plugin completely.
- Automatically initializes when `libxess_fg.dll` / `igxess_fg.dll` is loaded (via `LibraryLoad_Hooks.cpp` and `XeFGProxy::InitXeFG()`).
- Applies patches U1, U2, U3, U4, U5 directly in memory with:
  - Strict pattern/byte verification against known signatures.
  - Page-protection transactional safety (`VirtualProtect` to `PAGE_EXECUTE_READWRITE`, write, restore original protection, `FlushInstructionCache`).
  - Full rollback capabilities on failure.
- Implements burst frame presentation pacing so deadlines above 2X are paced evenly.

### 5.2 Streamline & In-Game Multiplier Routing
- In `Streamline_Hooks.cpp` (`hkslDLSSGGetState`):
  When `activeFgOutput == FGOutput::XeFG`, set `state.numFramesToGenerateMax = XeMfgLoader::EffectiveMax()`. This allows games to detect and display multipliers up to 6X.
- In `Streamline_Hooks.cpp` (`hkslDLSSGSetOptions`):
  When `activeFgOutput == FGOutput::XeFG`:
  - If `newOptions.mode == sl::DLSSGMode::eOff`, call `currentFG->SetInterpolatedFrameCount(0)`.
  - If `newOptions.mode != sl::DLSSGMode::eOff`, call `currentFG->SetInterpolatedFrameCount(newOptions.numFramesToGenerate)`.
  - Synchronize `State::Instance().dlssgDetectedInterpolationCount` and volatile `Config::Instance()->FGXeFGInterpolationCount`.

### 5.3 `XeFG_Dx12` Dynamic Multiplier & Safe Passthrough
- Implement `XeFG_Dx12::SetInterpolatedFrameCount(UINT count)`:
  - Dynamically invokes `XeFGProxy::SetNumInterpolatedFrames()(_swapChainContext, count)`.
  - Updates `_framesToInterpolate`.
- Safe Passthrough Mode:
  - When `count == 0` or in-game FG is `eOff`, `XeFG_Dx12` enters clean passthrough mode.
  - In passthrough mode, native frames are presented directly without executing interpolation compute passes, while maintaining valid swapchain state to prevent driver or pacing thunk crashes.

### 5.4 Configuration & Dedicated Menu UI
- **`OptiScaler.ini`**:
  ```ini
  [XeMFG]
  ; Enables Intel Xe Multi-Frame Generation unlocker (up to 6X)
  UnlockMFG=true

  ; Maximum generated frames ceiling: 1 to 5 (2X to 6X) - Default: 3 (4X)
  MaxInterpolatedFrames=3

  ; Enables burst presentation deadline pacing for multipliers > 2X
  ExtraPacing=true
  ```
- **OptiScaler Menu (`menu_common.cpp`)**:
  - Dedicated subsection `Intel Xe Multi-Frame Generation (XeMFG)`.
  - Toggle for `UnlockMFG`.
  - Dropdown for `Max Generated Frames` (2X through 6X).
  - Checkbox for `Burst Frame Pacing (>2X)`.
  - Diagnostic badges displaying `libxess_fg.dll` status, active patch count (`5/5`), and live in-game multiplier synchronization indicator.

### 5.5 Linux Compatibility Advantage
- DLSS-G relies on proprietary NVIDIA optical flow hardware and closed driver extensions (`nvngx_dlssg.dll`) that do not function across vendors on Linux.
- Intel's `libxess_fg.dll` operates via standard Direct3D 12 compute pipelines supported natively on Linux through VKD3D-Proton across AMD, Intel, and NVIDIA GPUs.
- Native XeMFG enables universal Multi-Frame Generation (up to 6X) on Linux without needing a 2X DLSS FG fallback.

---

## 6. Verification & Automated Test Results

1. **`tests/xemfg_loader_unit.cpp`** (PASSED):
   - Fast-path RVA patch application (`U1` to `U5`) and burst presentation pacing verification (`0x224cf0`, `0x21ee30`, `0x224b30`).
   - Clean byte-for-byte rollback restoring pristine binary state.
   - Relocated image fallback with signature scanning and dynamic 6X multiplier verification.
   - Atomic transaction rollback on patch target corruption preventing half-patched state.
   - Multiplier ceiling parameterization and clamping (1 to 5).
2. **`tests/xefg_mfg_streamline_unit.cpp`** (PASSED):
   - Streamline `hkslDLSSGGetState` capability advertising (`numFramesToGenerateMax = EffectiveMax(1)`) when `FGOutput == FGOutput::XeFG`, with safety clamping to 1 for non-XeFG outputs or patch failures.
   - Streamline `hkslDLSSGSetOptions` multiplier routing directly to `XeFG_Dx12::SetInterpolatedFrameCount()` and state synchronization.
   - In-game disable (`mode == eOff` / `count == 0`) safely entering passthrough mode (`_passthrough = true`) and skipping presentation passes, resolving the in-game disable crash.
   - Multi-frame generation re-enablement cleanly exiting passthrough mode.
   - Strict mutual exclusion enforcement between Ada MFG, Ampere SM86 MFG, and Intel XeMFG.
3. **Format & MSVC Rule Checks** (PASSED):
   - All modified and new `.cpp` and `.h` files format cleanly with zero `clang-format --dry-run --Werror` violations.
   - UTF-8 BOM (`\xef\xbb\xbf`) preserved across all modified files.

