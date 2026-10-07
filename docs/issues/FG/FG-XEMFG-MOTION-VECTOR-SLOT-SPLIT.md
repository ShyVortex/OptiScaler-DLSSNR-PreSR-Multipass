# Intel XeFG / XeMFG Motion Vector Slot-Split Crash & Warm Passthrough Resolution

## 1. Overview & Context

- **Operating System**: Windows 11 x64 / Windows 10 x64.
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll`) with OptiScaler XeMFG Multi-Frame Generation Unlocker.
- **Target Applications**:
  - *The Last of Us Part II* (`tlou-ii.exe` / Naughty Dog engine) utilizing NVIDIA Streamline (`sl.interposer.dll`).
  - *Resident Evil Requiem* (`re9.exe` / Capcom RE Engine) utilizing NVIDIA Streamline (`sl.interposer.dll`).
- **Analyzed Log Files**:
  - `OptiScaler_TLOU2-test1.log` (Shader compilation loading screen fatal crash).
  - `OptiScaler_RE-test11.log` (Active gameplay pacing verification and menu pause/unpause analysis).
- **Target Branch**: `intel-xemfg`.

---

## 2. Reported Symptoms

1. **Fatal Freeze / Crash in The Last of Us Part II**:
   - In the "No Return" lobby, the game ran smoothly without crashes or black screens.
   - Upon launching the first encounter, during a loading screen where the game compiles shaders, the game froze and crashed to desktop.
   - The log recorded:
     ```
     [22:11:03.178167] [E] XeFG Log: XeFG: Invalid argument. Missing resource of type XEFG_RES_MOTION_VECTOR.
     [22:11:03.188571] [E] StreamlineHooks::streamlineLogCallback [22-11-03][streamline][error][tid:5564][305s:337ms:030us]exception.cpp:75[writeMiniDump] Exception detected - thread 5564 - creating mini-dump 'C:\ProgramData/NVIDIA/Streamline/tlou-ii/1791403863185526/sl-sha-17026aaf3.dmp'
     ```
2. **Menu Pause/Unpause Presentation Hiccups in Resident Evil Requiem**:
   - Active gameplay ran smoothly at ~95 FPS with flawless pacing.
   - However, entering pause menus or opening inventory screens caused ~146ms frametime spikes.
   - Resuming gameplay repeatedly emitted:
     ```
     [22:00:28.645438] [W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=3 (XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES), framesPresented=1, enabled=1
     ```

---

## 3. Issue Validity Assessment & Root Cause Analysis

Both reported symptoms represent genuine architectural defects in OptiScaler's frame generation interposer pipeline:

### 3.1 Defect 1: Motion Vector Slot-Split via Dynamic Readiness Probing (`IFGFeature`)

In Streamline-integrated Direct3D 12 engines, game guide resources for a frame $N$ arrive sequentially across multiple API calls:
1. Streamline invokes `setResource(Depth)`. OptiScaler queries `GetIndexWillBeDispatched()` to determine which ring buffer slot should receive the Depth texture.
2. Streamline next invokes `setResource(MotionVectors)`. OptiScaler queries `GetIndexWillBeDispatched()` to determine which ring buffer slot should receive the Motion Vectors texture.

#### The Failure Mechanism:
In earlier implementations, `GetIndexWillBeDispatched()` called `ResolveDispatchSlot()`, which dynamically inspected slot readiness via `IsSlotReady()`:
```cpp
// Previous flawed logic in GetIndexWillBeDispatched():
uint64_t willDispatchFrame = 0;
int slot = ResolveDispatchSlot(willDispatchFrame);
if (slot >= 0)
    return slot;
```
1. When `setResource(Depth)` was called, slot $A = N \pmod 4$ was empty. `IsSlotReady(A)` returned `false` because Motion Vectors had not arrived yet.
2. `ResolveDispatchSlot()` scanned previous slots. If an older slot $B$ had residual readiness flags lingering from a previous frame (due to `_resourceReady` not being flushed on dispatch), `ResolveDispatchSlot()` returned slot $B$.
3. When `setResource(MotionVectors)` was called immediately afterwards, slot $A$ now had Depth marked ready, but slot $B$ had older residual data. In fluctuating frametime conditions (such as shader compilation in loading screens), `ResolveDispatchSlot()` returned a different slot for Motion Vectors than for Depth!
4. Consequently:
   - Slot $A$ received `Depth` without `MotionVectors`.
   - Slot $B$ received `MotionVectors` without `Depth`.
5. When `XeFG_Dx12::Dispatch()` executed, Intel's runtime received a frame descriptor missing `XEFG_RES_MOTION_VECTOR`, rejected the call with an error, and Streamline threw a fatal unhandled exception and crash dump.

### 3.2 Defect 2: Destructive Swapchain Queue Teardown on Menu Pause (`XeFG_Dx12`)

When games enter pause menus, UI screens, or loading sequences, Streamline notifies the interposer that Frame Generation should be disabled (`DLSSGMode::eOff`).
In `XeFG_Dx12::SetInterpolatedFrameCount(0)`:
```cpp
// Previous teardown logic:
if (interpolatedFrameCount == 0)
{
    _passthrough = true;
    _framesToInterpolate = 0;

    if (_swapChainContext != nullptr && XeFGProxy::SetEnabled() != nullptr)
    {
        XeFGProxy::SetEnabled()(_swapChainContext, false);
    }
    Deactivate();
    return true;
}
```
1. Calling `XeFGProxy::SetEnabled(false)` instructed Intel's swapchain driver to dismantle its internal presentation queue.
2. Calling `Deactivate()` set `_isActive = false`, bypassing OptiScaler's present hooks entirely.
3. When the user unpaused or resumed gameplay, Streamline called `SetInterpolatedFrameCount(N)` ($N \ge 1$), which called `Activate()` and `XeFGProxy::SetEnabled(true)` from a cold state.
4. On the very first frame of gameplay resumption, Intel's swapchain queue had $< 2$ frames buffered, emitting `XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES` and dropping frames for ~146ms before stabilizing.

---

## 4. Technical Solutions Applied

### 4.1 Deterministic Projected Slot Tagging (`IFGFeature.h`, `IFGFeature.cpp`)

1. **Restored Deterministic Projected Slot Query**:
   In `IFGFeature::GetIndexWillBeDispatched()`, dynamic readiness scanning was removed. The query now uses a deterministic calculation based on projected frame progress:
   ```cpp
   int IFGFeature::GetIndexWillBeDispatched()
   {
       uint64_t df = 0;
       int64_t diff = _frameCount - _lastDispatchedFrame;
       if (diff > _allowedFrameAhead || diff < 0 || _lastDispatchedFrame == 0)
       {
           if (_resourceReady[GetIndex()].contains(FG_ResourceType::Depth) &&
               _resourceReady[GetIndex()].at(FG_ResourceType::Depth))
               df = _frameCount;
           else
               df = _lastDispatchedFrame + 1;
       }
       else
       {
           df = _lastDispatchedFrame + 1;
       }
       return static_cast<int>(df % BUFFER_COUNT);
   }
   ```
   Because `_frameCount` and `_lastDispatchedFrame` remain constant during the resource tagging phase of a given frame, all incoming guides (`Depth`, `MotionVectors`, `HudlessColor`, etc.) are guaranteed to tag into the exact same slot index.

2. **Isolated Dynamic Readiness to Execution Time**:
   `ResolveDispatchSlot()` is now called exclusively by `GetDispatchIndex()` inside `Present()`, immediately before driver dispatch.

### 4.2 Ring Buffer Slot Readiness Sanitization & Lifecycle Management (`IFGFeature`)

1. **Readiness Flush on Dispatched Frames**:
   In `IFGFeature::ConfirmDispatched(UINT64 frameId)`:
   ```cpp
   _resourceReady[frameId % BUFFER_COUNT].clear();
   ```
   Flushing `_resourceReady` for the confirmed frame guarantees that dispatched slots never report stale readiness to subsequent candidate evaluations.
2. **Lifecycle Reset Sanitization**:
   Added `IFGFeature::ClearAllResourceReady()`:
   - Invoked during `IFGFeature::ResetCounters()`.
   - Invoked during `Deactivate()` across all backends (`XeFG_Dx12.cpp`, `DLSSG_Dx12.cpp`, `FSRFG_Dx12.cpp`).
3. **Constrained Start Fallback in `ResolveDispatchSlot`**:
   Constrained Step 4 (`_lastDispatchedFrame == 0`) so that it strictly binds `willDispatchFrame = _frameCount` only when the candidate slot matches `_frameCount % BUFFER_COUNT`.

### 4.3 Warm Swapchain Passthrough in `XeFG_Dx12`

In `XeFG_Dx12::SetInterpolatedFrameCount()`:
```cpp
if (interpolatedFrameCount == 0)
{
    _passthrough = true;
    _framesToInterpolate = 0;
    _needResetHistory = true;

    LOG_DEBUG("XeFG entered warm passthrough mode (framesToInterpolate=0)");
    return true;
}
```
1. **Preserved Swapchain Queue State**:
   Avoids calling `XeFGProxy::SetEnabled(false)` and `Deactivate()`. Intel's swapchain driver remains active and ready in 1:1 presentation mode.
2. **Base Frame Pass-Through**:
   In `XeFG_Dx12::Present()`:
   ```cpp
   if (_passthrough)
   {
       LOG_DEBUG("XeFG is in passthrough mode, presenting base frame without interpolation");
       _lastDispatchedFrame = _frameCount;
       _actuallyDispatchedFrame = _frameCount;
       return true;
   }
   ```
   Presents base frames directly without evaluating interpolation kernels or consuming GPU resources.
3. **Seamless Resumption with Temporal History Flush**:
   When leaving menus, setting `interpolatedFrameCount > 0` clears `_passthrough`. Because `_needResetHistory` was set to `true`, the first resumed frame passes `constData.resetHistory = true` to Intel's runtime, discarding stale menu history while keeping the swapchain queue fully primed.

---

## 5. Verification & Unit Tests

### 5.1 Automated Unit Tests in `tests/xefg_ring_buffer_pacing_unit.cpp`

1. **Test 7: Multi-Resource Tagging Alignment**:
   - Simulates sequential tagging of Depth followed by Motion Vectors.
   - Asserts `depthSlot == mvSlot`.
   - Confirms `GetDispatchIndex()` executes slot successfully.
   - Asserts `ConfirmDispatched()` completely flushes slot readiness.
2. **Test 8: Warm Passthrough Queue Preservation & Unpause Transition**:
   - Simulates active gameplay dispatching frames 1 and 2.
   - Simulates entering pause menu (`SetInterpolatedFrameCount(0)`):
     - Asserts `_passthrough == true`, `_framesToInterpolate == 0`, `_isActive == true`, and `_needResetHistory == true`.
   - Simulates native frame presentation during pause.
   - Simulates unpause (`SetInterpolatedFrameCount(2)`):
     - Asserts `_passthrough == false`, `_framesToInterpolate == 2`, and `_needResetHistory == true`.
   - Simulates first resumed frame arrival:
     - Asserts `depthSlot == mvSlot` (zero slot-split).
     - Confirms `constData.resetHistory` is flagged for Intel's runtime.
     - Asserts clean dispatch and slot clearance.

### 5.2 Full Regression Test Suite Results

```bash
All XeFG Ring Buffer & Motion Vector Pacing Unit Tests PASSED! (8/8 tests)
All XeFG Save/Load Stability & Presentation Pacing Unit Tests PASSED! (5/5 tests)
All RTSS + XeFG Pacing & Marker Immunity Unit Tests PASSED! (3/3 tests)
All XeFG Swapchain Lifecycle Unit Tests PASSED! (4/4 tests)
All XeMfgLoader unit tests PASSED successfully! (8/8 tests)
All XeFG MFG & Streamline unit tests PASSED successfully! (7/7 tests)
All XeMFG Menu UI Conflict Suppression tests PASSED successfully! (3/3 tests)
```

All modified source files strictly comply with `.clang-format` and preserve UTF-8 BOM (`\xef\xbb\xbf`).
