# Intel XeFG / XeMFG Menu & Pipelined Gameplay Stutter Resolution

## 1. Overview & Context

- **Operating System**: Windows 11 x64 / Windows 10 x64.
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll`) with OptiScaler XeMFG Multi-Frame Generation Unlocker.
- **Target Application**: RE Engine titles (specifically *Resident Evil Requiem* / `re9.exe`) and other Direct3D 12 titles utilizing NVIDIA Streamline (`sl.interposer.dll`) or native XeFG.
- **Analyzed Log File**: `OptiScaler_test10.log` (838,951 lines).
- **Target Branch**: `intel-xemfg`.

---

## 2. Reported Symptoms

Following the implementation of presentation deadline fixes and RTSS pacing immunity:
1. Random stutters and dropped frames persisted during active gameplay.
2. Stutters and severe frame drops were prominently observed in the game's **main menu** and **pause menu**, where static screens should exhibit stable frame delivery.
3. Resuming gameplay from the pause menu ran smoothly for 1–2 seconds before abruptly destabilizing with frame drops and pacing jitter.

---

## 3. Issue Validity Assessment & Root Cause Analysis

Analysis of `OptiScaler_test10.log` established that the reported symptoms stemmed from three distinct, interconnected defects in OptiScaler's frame generation dispatch and lifecycle pipeline:

### 3.1 Defect A: Pipelined Ring Buffer Slot Desynchronization (`IFGFeature`)
In modern multi-threaded game engines such as Capcom's RE Engine:
1. The game's engine simulation thread submits frame constants for the *next* frame $N+1$ via `slSetConstants(N+1)` before the render thread completes tagging the guides (Depth, Motion Vectors) for current frame $N$.
2. `Sl_Inputs_Dx12::CheckForFrame()` intercepted the new frame ID and called `fg->StartNewFrame()`, immediately incrementing `_frameCount` to $N+1$.
3. Concurrently, the render thread completed and tagged Depth and Velocity resources for frame $N$ into slot index $N \pmod 4$.
4. When DXGI `Present()` was invoked for frame $N$, `IFGFeature::GetDispatchIndex` blindly computed candidate frame:
   $$\text{willDispatchFrame} = \_lastDispatchedFrame + 1 = (N) + 1 = N+1$$
   and inspected slot $(N+1) \pmod 4$.
5. Because slot $(N+1) \pmod 4$ had not yet been tagged by the render thread, `XeFG_Dx12::Dispatch()` found:
   `_frameResources[(N+1) % 4][Depth].validity != ValidNow`
   and emitted:
   ```
   [W] XeFG_Dx12::Dispatch Depth or Velocity is not ready, skipping
   ```
   (16 occurrences in `OptiScaler_test10.log`), completely skipping frame $N$ which was fully ready in slot $N \pmod 4$.

### 3.2 Defect B: Destructive Swapchain Disablement & Cascading Drop Chain (`XeFG_Dx12::Present`)
When `Dispatch()` skipped a frame due to Defect A or transient guide delays, `XeFG_Dx12::Present()` executed:
```cpp
// Former lines 1474-1480 in XeFG_Dx12.cpp:
if (!dispatchResult && _swapChainContext != nullptr)
{
    if (XeFGProxy::SetEnabled() != nullptr)
        XeFGProxy::SetEnabled()(_swapChainContext, false);
    _needResetHistory = true;
    _lastDispatchedFrame = _frameCount;
}
```
This produced two catastrophic side effects:
1. **Swapchain Queue Starvation**: Calling `SetEnabled(false)` mid-stream right before DXGI Present instructed Intel's swapchain driver to disable frame generation processing for that presentation call. On the subsequent frame, Intel's runtime emitted:
   ```
   [W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=3 (XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES)
   ```
   (13 occurrences in `OptiScaler_test10.log`), dropping the presentation queue and stuttering.
2. **Permanent Dispatch Index Offset**: Setting `_lastDispatchedFrame = _frameCount` manually forced `_lastDispatchedFrame` to jump ahead. On the subsequent present, the dispatch logic attempted to read slot $(N+2) \pmod 4$ while the render thread was only on $N+1$. This created a cascading chain of 7+ consecutive dropped frames (e.g. at line 580,331 of the log).

### 3.3 Defect C: Silent `fgChanged` Tripwire on Menu Hudless Format Transitions (`XeFG_Dx12::SetResource`)
In `XeFG_Dx12::SetResource()`, a static format cache compared the DXGI format of the incoming `HudlessColor` descriptor:
```cpp
// Former lines 1667-1674 in XeFG_Dx12.cpp:
if (type == FG_ResourceType::HudlessColor)
{
    static DXGI_FORMAT lastFormat[BUFFER_COUNT] = {};
    auto desc = fResource->GetResource()->GetDesc();

    if (lastFormat[fIndex] != DXGI_FORMAT_UNKNOWN && lastFormat[fIndex] != desc.Format)
    {
        State::Instance().fgChanged = true;
        return false;
    }
    lastFormat[fIndex] = desc.Format;
}
```
During main menu and pause menu navigation, RE Engine frequently alternates render targets and intermediate formats between UI rendering passes and 3D background passes (e.g. `R11G11B10_FLOAT` vs `R8G8B8A8_UNORM`).
1. Every time a format transition occurred, `SetResource` tripped `State::Instance().fgChanged = true`.
2. In `XeFG_Dx12::EvaluateState()`, `fgChanged == true` triggered `Deactivate()` and `UpdateTarget()`.
3. `UpdateTarget()` enforced `_waitingFrames = 10;`, pausing frame generation for 10 frames (~160–330 ms) every 1–2 seconds.
4. When the 10 frames elapsed, XeFG re-activated and immediately hit Defects A and B, creating violent, repeating stutters even while sitting completely idle in static menus.

---

## 4. Technical Solutions Applied

### 4.1 Resource-Aware Pipelined Dispatch (`IFGFeature.h`, `IFGFeature.cpp`)
1. **Decoupled Tentative Count from Confirmed Dispatch**:
   Added `_actuallyDispatchedFrame` to `IFGFeature` to track the exact frame ID that successfully completed driver execution, separate from `_lastDispatchedFrame`.
2. **Slot Resource Readiness Inspection**:
   Added `IsSlotReady(int index)` verifying that both `Depth` and `Velocity` have been tagged as `ValidNow` or `UntilPresent` in `_resourceReady`.
3. **Unified Resolution Logic (`ResolveDispatchSlot`)**:
   - Catches un-dispatched frames waiting in the ring buffer (e.g. after menu resumption or activation where `_lastDispatchedFrame > _actuallyDispatchedFrame`) and dispatches them instead of skipping.
   - Evaluates candidate frame $N+1$; if its slot is ready, dispatches it.
   - If candidate $N+1$ is not ready (due to simulation running ahead of render), inspects slot $N$ or $N+2$. If slot $N$ has complete guides ready, dispatches slot $N$.
   - On initial start or when `_lastDispatchedFrame == 0`, scans ring buffer slots for the oldest ready frame.
4. **Dispatch Confirmation (`ConfirmDispatched`)**:
   `ConfirmDispatched(willDispatchFrame)` is now called on successful driver dispatch across `XeFG_Dx12.cpp`, `DLSSG_Dx12.cpp`, and `FSRFG_Dx12.cpp`.

### 4.2 Graceful Single-Frame Passthrough (`XeFG_Dx12::Present`)
1. **Removed Destructive Swapchain Disablement**:
   Eliminated `XeFGProxy::SetEnabled()(_swapChainContext, false);` and `_lastDispatchedFrame = _frameCount;` from `Present()`.
2. **Seamless Native Presentation**:
   When `Dispatch()` returns `false` on a transient frame, the pipeline sets `_needResetHistory = true` (to reset temporal history on subsequent interpolated frames) and returns `false`.
3. OptiScaler's swapchain hook passes the native backbuffer through to DXGI `Present()` without starving Intel's swapchain queue or de-syncing the ring buffer.

### 4.3 Menu Hudless Format Stability (`XeFG_Dx12::SetResource`)
1. **Eliminated `fgChanged` Stall Loop**:
   Removed `State::Instance().fgChanged = true; return false;` upon `HudlessColor` format changes.
2. **Dynamic Format Tracking**:
   Format transitions now update `lastFormat[fIndex]` and emit diagnostic log messages without deactivating the pipeline or triggering 10-frame pauses.

### 4.4 Lifecycle Counter Synchronization
- Reset `_actuallyDispatchedFrame = 0` alongside `_lastDispatchedFrame = 0` in `XeFG_Dx12::CreateContext()`, `Activate()`, and `Deactivate()`.
- Synchronized `_actuallyDispatchedFrame = _frameCount` in passthrough mode.

---

## 5. Automated Unit Test Verification

### 5.1 Ring Buffer Pacing & Dispatch Tests (`tests/xefg_ring_buffer_pacing_unit.cpp`)
All test cases pass cleanly with `g++ -std=c++20 -O2`:
- **Test 1**: Pipelining up to 2 frames ahead operates smoothly.
- **Test 2**: Gaps > 2 immediately resynchronize to current valid frame.
- **Test 3**: Large discontinuities correctly clamp `_lastDispatchedFrame`.
- **Test 4**: Motion vector scale zero inputs safely default to 1.0f.
- **Test 5 (RE Engine test10 simulation ahead of render)**: Simulates simulation thread advancing `_frameCount` to $N+1$ via `StartNewFrame()` while render thread tags guides for frame $N$. Verifies `ResolveDispatchSlot()` dispatches frame $N$ without skipping.
- **Test 6 (Unpause resumption)**: Simulates pause/unpause where `_lastDispatchedFrame = 10` but `_actuallyDispatchedFrame = 9`. Verifies frame 10 is dispatched rather than skipped.

### 5.2 Full Test Suite Execution
- `tests/rtss_xefg_pacing_immunity_unit.cpp`: PASSED (3/3 tests).
- `tests/xefg_save_load_stability_unit.cpp`: PASSED (5/5 tests).
- `tests/xemfg_loader_unit.cpp`: PASSED (8/8 tests).

### 5.3 Formatting & BOM Verification
- All modified files (`IFGFeature.h`, `IFGFeature.cpp`, `XeFG_Dx12.cpp`, `DLSSG_Dx12.cpp`, `FSRFG_Dx12.cpp`, `tests/xefg_ring_buffer_pacing_unit.cpp`) verified compliant with `clang-format --dry-run --Werror`.
- UTF-8 BOM (`\xef\xbb\xbf`) verified and preserved on all required files.
