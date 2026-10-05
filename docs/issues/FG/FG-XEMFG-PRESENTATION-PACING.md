# XeSS-G / XeMFG: Presentation Deadline Pacing, Swapchain Synchronization & Real-Time Telemetry

## 1. Overview & Environment

- **Operating System**: Windows 11 build 26100 / Linux (Proton 9 / VKD3D-Proton / Gamescope)
- **Hardware**: NVIDIA GeForce RTX 3080 Ti (Ampere SM86, `0x170`)
- **Driver**: NVIDIA Display Driver 572.16 / 576.02
- **Game Engine**: RE Engine (*Resident Evil Requiem*, `re9.exe`)
- **Mod Configuration**: OptiScaler with Intel XeSS-FG (`libxess_fg.dll` v1.3.1.78) and XeMFG multi-frame generation unlocker
- **Test Baseline**: Clean application shutdown, menu conflict suppression, and pipeline auto-configuration (`FGInput = DLSSG`, `FGOutput = XeFG`, `FGEnabled = true`) verified.

---

## 2. Reported Issue & Symptoms

Following the successful integration of XeMFG multi-frame generation and exit-crash resolution, user testing in *Resident Evil Requiem* revealed two visual and temporal defects:

1. **XeMFG Multi-Frame Visual Illusion (>2X FG)**:
   - When setting multi-frame multipliers above 2X (e.g. 3X, 4X, 5X), the FPS counter accurately scaled upward (e.g. 180 FPS to 240+ FPS).
   - However, visually on screen, camera panning and animation motion remained identical to native framerate (~60 FPS), with no perceivable frame interpolation smoothness.
2. **2X XeFG Micro-Stutters on Windows vs. Linux**:
   - In 2X XeFG mode on native Windows DXGI, noticeable micro-stutters and frame-time jitter persisted.
   - In contrast, the identical 2X XeFG fallback pipeline running on Linux under Proton/Gamescope was completely smooth and fluid.
3. **Question on Detour Safety**:
   - Previous attempts using `XeFGUnlock.asi` caused severe stutters and game-shutdown deadlocks, raising questions about whether OptiScaler's detours avoided those failure modes.

---

## 3. Issue Validity Assessment

### Is this a user misconfiguration?
**No.** OptiScaler configuration was verified correct:
- `OptiScaler_test5.log` confirmed `FGInput=DLSSG`, `FGOutput=XeFG`, all 5 memory patches (`U1`–`U5`) applied cleanly, and Streamline capability advertised.
- In-game graphics settings set Frame Generation to "On" with Streamline dispatching valid frames.

### Defect Verification & Root Cause Analysis

#### A. Multi-Frame Presentation Deadline Clumping in `libxess_fg.dll`
Reverse engineering of `libxess_fg.dll` v1.3.1.78 revealed that the internal presentation scheduler contains a hardcoded ceiling in its presentation deadline calculator (thunk `0x3430` / RVA `0x224b30`):
- For `frameIndex == 1` (standard 2X FG), it calculates a half-interval deadline based on `cycleInfo->frameInterval`.
- For any unlocked frame with `frameIndex > 1` (indices 2, 3, 4, 5 for 3X–6X FG), it executes a fallthrough branch:
  ```c
  *pDeadline = cycleState->timestamp; // Zero-duration deadline!
  ```
- Because intermediate deadlines are set to `0ms`, `libxess_fg.dll`'s presentation worker thread submits all 2 to 5 generated frames to the DXGI swapchain within a single sub-millisecond burst (<0.2ms).
- On Windows DXGI with `SyncInterval = 0` and `DXGI_PRESENT_ALLOW_TEARING`, the display driver's flip queue overwrites intermediate frames before the physical monitor can scan them out during VBlank. Only the final frame in the burst reaches the scanout raster. The Present counter registers 240 FPS, but the display only renders native 60 FPS motion.

#### B. Flaws of `XeFGUnlock.asi` Spin-Wait Loops
Investigation of `XeFGUnlock.asi` explained why previous community mods failed:
- `XeFGUnlock.asi` hooked internal thunk `0x25c0` and implemented an active CPU spin-wait loop:
  ```c
  while (QueryPerformanceCounter(&now) < target) {
      _mm_pause();
      Sleep(0);
  }
  ```
- `Sleep(0)` only yields to threads of equal priority on the current logical core, pinning CPU execution at 100%, starving the engine's worker threads, and causing deadlocks when the game terminates.
- Furthermore, `XeFGUnlock.ini` explicitly admitted its pacing failure: `"; 4 and 5 ( 5x and 6x FG) do work, but have issues with pacing unless you enable V-Sync."`

#### C. Windows Unmetered Presentation vs. Linux Compositor Pacing
- On Linux (Proton / VKD3D-Proton / Gamescope), D3D12 `Present` calls route through Vulkan WSI. The display server (Wayland / Gamescope) enforces display-synchronized mailbox/FIFO delivery, naturally pacing generated frames to display refresh intervals.
- On Windows native DXGI, when games disable V-Sync (`SyncInterval = 0`, `DXGI_PRESENT_ALLOW_TEARING`), presentation is unmetered. Without vertical blank synchronization, micro-variations in render times cause severe frame-time jitter (stutter).

---

## 4. Technical Solution & Architecture

### 1. Passive Mathematical Deadline Detour (`hkCalculateDeadline`)
Instead of hooking `0x25c0` or introducing CPU spin loops, OptiScaler hooks the presentation deadline calculator (`0x224b30`) directly using Microsoft Detours:
```cpp
static void* hkCalculateDeadline(void* pContext, uint64_t* pDeadline, void* pCycleState, void* pCycleInfo,
                                 uint32_t frameIndex, uint32_t totalFrames)
{
    void* result = o_CalculateDeadline(pContext, pDeadline, pCycleState, pCycleInfo, frameIndex, totalFrames);

    if (pDeadline != nullptr && pCycleInfo != nullptr && totalFrames >= 2 && frameIndex >= 1 && frameIndex <= 5)
    {
        uint64_t frameInterval = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(pCycleInfo) + 8);
        if (frameInterval > 0)
        {
            uint64_t intervalPerFrame = frameInterval / totalFrames;
            *pDeadline += static_cast<uint64_t>(frameIndex) * intervalPerFrame;
        }
    }
    return result;
}
```
- **Zero Spin-Wait**: Deadlines are passively partitioned across the frame cycle.
- **Native Worker Scheduling**: `libxess_fg.dll`'s native background presentation thread (`0x180007a30`) spaces presentations evenly across the interval.
- **Clean Lifecycle**: Safely attached in `XeMfgLoader::TryApply()` and unhooked in `XeMfgLoader::Shutdown()`.

### 2. Real-Time Telemetry & Visual Debug Verification
Using Intel's XeSS-FG SDK (`xefg_swapchain.h` and `xefg_swapchain_debug.h`):
1. **Telemetry Capture**:
   - Added `PostPresent()` virtual hook to `IFGFeature`.
   - In `XeFG_Dx12::PostPresent()`, OptiScaler invokes `XeFGProxy::GetLastPresentStatus()(_swapChainContext, &presentStatus)`.
   - Tracks `framesPresented`, `frameGenResult`, and `isFrameGenEnabled`. Logs warnings if Intel's pipeline silently falls back to duplicate backbuffers.
2. **In-Game Menu Telemetry Display**:
   - Real-time `Present Status` indicator in the OptiScaler overlay displaying live presented frame counts and status codes.
3. **Visual Proof Toggles**:
   - **`Debug Markers (Corners)`** (`XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES`): Renders colored marker quads in the screen corners of real interpolated frames.
   - **`Only Interpolated Frames`** (`XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION`): Blanks native frames and presents strictly generated frames. If interpolation is active, motion remains visible; if failed, the screen turns black.

### 3. Swapchain VBlank Synchronization (`ExtraPacing`)
In `OptiScaler/hooks/FG_Hooks.cpp` and `OptiScaler/wrapped/wrapped_swapchain.cpp`:
- When running on Windows (`!usesDxvk`) with `XeMfgExtraPacing` enabled (default `true`) and `ForceVsync` not explicitly disabled:
  ```cpp
  if (SyncInterval < 1)
      SyncInterval = 1;

  Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
  ```
- Aligns all generated and native frames to display VBlank cycles, eliminating frame-time jitter, tearing lines, and scanout collisions.
- Added interactive toggle **`Display VBlank Sync (Extra Pacing)`** under the XeMFG menu subsection.

---

## 5. Test Verification & Results

### Automated Unit Tests
- **`tests/xemfg_loader_unit.cpp`**:
  - Test 1: Fast-path RVA parameter unlock patch application (`U1`–`U5`).
  - Test 2: Full rollback restores byte-for-byte image identity.
  - Test 3: Relocated signature scanning fallback and 6X multiplier.
  - Test 4: Atomic rollback on patch failure prevents half-patched state.
  - Test 5: Multiplier range clamping (1 to 5).
  - Test 6: Safe lifecycle teardown restores pristine image byte-for-byte.
  - Test 7: Presentation telemetry recording & lifecycle reset (`RecordPresentStatus`).
  - Test 8: Multi-frame presentation deadline calculation evenly spaces presentation intervals.
- **`tests/xefg_mfg_streamline_unit.cpp`**: Tests 1–7 covering Streamline advertising, multiplier routing, passthrough transitions, and mutual exclusion.
- **`tests/xemfg_menu_exclusion_unit.cpp`**: Tests 1–3 covering menu control suppression.
- **Compilation & Verification**:
  ```bash
  g++ -std=c++20 tests/xemfg_loader_unit.cpp -o tests/xemfg_loader_unit && ./tests/xemfg_loader_unit
  g++ -std=c++20 tests/xefg_mfg_streamline_unit.cpp -o tests/xefg_mfg_streamline_unit && ./tests/xefg_mfg_streamline_unit
  g++ -std=c++20 tests/xemfg_menu_exclusion_unit.cpp -o tests/xemfg_menu_exclusion_unit && ./tests/xemfg_menu_exclusion_unit
  ```
  Result: **100% PASS** across all test suites.

### Formatting & Encoding Compliance
- Preserved UTF-8 BOM (`\xef\xbb\xbf`) on all C++ source and header files.
- `clang-format --dry-run --Werror` verified clean with zero formatting warnings.
