# Frame Generation: Intel XeMFG-SR Swapchain Allocation Capacity Overhaul, Binary Ceiling Decoupling, Reflex Cleanup & Presentation Pacing

## 1. Overview & Environment

- **Operating System**: CachyOS Linux x86_64 (Kernel `7.2.9-2-cachyos`, ID: `cachyos`, ID_LIKE: `arch`, rolling release)
- **Display Server / Compositor**: Wayland (`kwin_wayland` / KDE Plasma)
- **Target Application**: *Helldivers 2* (`helldivers2.exe`, Steam AppID 553850, Arrowhead Game Studios / Sony Interactive Entertainment)
- **GPU Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (16384 MiB / 16 GB GDDR6 VRAM, GA103 / Ampere SM86, architecture `0x170`)
- **GPU Driver**: NVIDIA Proprietary Driver `580.178.04` (CUDA 13.0)
- **Display Resolution & Refresh Rate**: 2560x1440 @ 164.9 Hz
- **Proton / Translation Stack**: Proton Experimental / Wine / VKD3D-Proton (Direct3D 12 on Vulkan)
- **Target Runtimes**: `dxgi.dll` (OptiScaler v0.9.59), `libxess_fg.dll` (Intel XeSS-FG v1.3.1.78)
- **Feature**: Intel Xe Multi-Frame Generation Injector for DLSS-SR (`XeMFG` / `XeMFG-SR`)
- **Reported Symptoms**:
  1. XeMFG-SR remaining locked to 2X (1 interpolated frame) or failing to engage higher multipliers smoothly.
  2. Persistent 71.4–74 FPS resonant lock and severe animation judder/snapback.
  3. Frame pacing stutter and periodic latency resets in mid-gameplay.

---

## 2. In-Depth Root Cause Analysis

Cross-model analysis (Gemini 4 Argos) and source code audit identified 4 compounding defects across `XeFG_Dx12.cpp`, `XeMfgLoader.cpp`, `menu_common.cpp`, `dllmain.cpp`, and `XeFGPacing.h`:

### 2.1 Bug #1: Swapchain Allocation Clamped to 1 Interpolated Frame (`XeFG_Dx12.cpp`)
In `CreateSwapchain` and `CreateSwapchain1`:
```cpp
if (Config::Instance()->ForceXeLL.value_or_default() || XeFGProxy::SetNumInterpolatedFrames() != nullptr)
    params.maxInterpolatedFrames = 1;
else
    params.maxInterpolatedFrames = intTarget;
```
Because `libxess_fg.dll` v1.3.1.78 exports `xefgSwapChainSetNumInterpolatedFrames`, `params.maxInterpolatedFrames` was unconditionally hardcoded to `1`.
In `libxess_fg.dll`, `D3D12InitFromSwapChainDesc` uses `params.maxInterpolatedFrames` as the allocation ceiling for internal descriptor heaps, render contexts, and buffer pools. Passing `1` permanently restricted the swapchain context to 1 interpolated frame (2X Multiplier) at creation time, preventing multi-frame scaling regardless of later `SetNumInterpolatedFrames` calls.

### 2.2 Bug #2: Live Binary Patch Rewriting Overwrote Provider Ceiling (`XeMfgLoader.cpp`)
`XeMfgLoader::TryApply()` initializes patches U1–U5 with a maximum ceiling of 5 generated frames (`kMaxSupportedFrames`).
However, in `XeFG_Dx12::SetInterpolatedFrameCount()` and `menu_common.cpp` (slider callback):
```cpp
if (XeMfgLoader::IsEnabled())
    XeMfgLoader::SetMaxGeneratedFrames(static_cast<uint32_t>(srFrames));
```
Every slider adjustment invoked `VirtualProtect` on `libxess_fg.dll`'s code segment during active rendering, overwriting `U4/override-clamp` with `srFrames` (e.g. 2). This downgraded the DLL's internal ceiling, clamping future multiplier requests and causing CPU instruction cache invalidation churn.

### 2.3 Bug #3: Fake DLSS-G Reflex Injection & Redundant Per-Frame `SetEnabled(true)`
In `dllmain.cpp`, `XeFG_Dx12.cpp`, and `menu_common.cpp`, the XeMFG-SR path injected fake Streamline DLSS-G state:
```cpp
State::Instance().dlssgDetectedInterpolationCount = srFrames;
State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOn;
ReflexHooks::setDlssgFrameCount(static_cast<uint8_t>(srFrames));
```
In games without Streamline DLSS-G (such as *Helldivers 2*), no Streamline markers are emitted. `ReflexHooks::update()` timed out after 10 frames, logged `DLSS FG no longer detected`, reset `_FgNumFramesToGenerate = 0`, and called `setFPSLimit(0)`, triggering mid-game latency reconfigurations. Furthermore, `XeFG_Dx12::Dispatch()` called `XeFGProxy::SetEnabled()(_swapChainContext, true)` on every present, forcing Intel's swapchain to revalidate state redundantly.

### 2.4 Bug #4: Omission of Pacing on Final Generated Frame (`XeFGPacing.h`)
In `TryPace`:
```cpp
if (g_schedNative != nullptr && SchedulerUsable(ctx))
{
    if (!isLast)
        ScheduleFrame(ctx, burst, index);

    return; // Omission: returned immediately for isLast!
}
```
When `g_schedNative` was active, intermediate frames (1..4) routed to `ScheduleFrame`, but the final generated frame (`isLast == true`, caller `0x220467`) was skipped and returned immediately with zero pacing, dumping into the swapchain in 0 ms. In `PaceFrame`, `WaitUntil` wait duration was not accumulated into `g_burstBlockQpc`, and `multiplier <= 2` exited before `NoteFrame`, preventing cycle time updates in 2X mode.

---

## 3. Technical Solutions Applied

### 3.1 Swapchain Allocation Capacity Fix (`XeFG_Dx12.cpp`)
Updated `CreateSwapchain` and `CreateSwapchain1` to allocate `effectiveMax` (up to 5 for 6X Multi-Frame Generation) when `XeMfgLoader::AnyEnabledForSession()` is active:
```cpp
if (Config::Instance()->ForceXeLL.value_or_default())
    params.maxInterpolatedFrames = 1;
else if (XeMfgLoader::AnyEnabledForSession())
    params.maxInterpolatedFrames = effectiveMax;
else if (XeFGProxy::SetNumInterpolatedFrames() != nullptr)
    params.maxInterpolatedFrames = 1;
else
    params.maxInterpolatedFrames = intTarget;
```
Removed redundant per-frame `XeFGProxy::SetEnabled()(_swapChainContext, true)` from `XeFG_Dx12::Dispatch()`.

### 3.2 Decoupled Binary Provider Ceiling from Runtime Multiplier
- Removed `XeMfgLoader::SetMaxGeneratedFrames` from `XeFG_Dx12::SetInterpolatedFrameCount()` and `menu_common.cpp` XeMFG-SR slider callbacks.
- Patches U3–U5 remain permanently unlocked at 5 (`kMaxSupportedFrames`).
- Active multiplier adjustments route purely through Intel's official API `xefgSwapChainSetNumInterpolatedFrames()`.

### 3.3 Reflex DLSS-G Spoofing Cleanup
- Removed fake DLSS-G state injection (`dlssgDetectedInterpolationCount`, `dlssgLastSetMode`, `ReflexHooks::setDlssgFrameCount`) from `dllmain.cpp` and `menu_common.cpp` XeMFG-SR sections.
- In `XeFG_Dx12.cpp`, gated DLSS-G Reflex updates behind `if (State::Instance().activeFgInput == FGInput::DLSSG)`.
- In non-DLSS-G titles, `ReflexHooks` binds strictly to native Reflex without timing out or zeroing FPS limits.

### 3.4 Pacing Engine Complete Coverage (`XeFGPacing.h`)
- Restructured `TryPace` so `g_schedNative` only returns early for intermediate frames (`!isLast`), allowing `isLast == true` to fall through to `ProviderPacesLastFrame` and `PaceFrame`.
- In `PaceFrame`, measured `WaitUntil` wait duration and accumulated it into `g_burstBlockQpc`:
  ```cpp
  const int64_t waitQpc = end.QuadPart - beforeWait.QuadPart;
  if (waitQpc > 0)
      g_burstBlockQpc += waitQpc;
  ```
- Moved `NoteFrame(index, count, now.QuadPart)` before `if (multiplier <= 2) return;` so 2X mode records cycle times and feeds valid delta times.

### 3.5 Elimination of 71.4 FPS Resonant Lock: Decoupling TsDetour Render Time Deadlines
- **Root Cause of 60 FPS vs. 90 FPS / Uncapped Discrepancy**:
  - At 60 FPS cap, the game was **limiter-bound**; base engine rendered in ~28 ms and slept until the 33.3 ms (2X) / 50.0 ms (3X) limit, keeping the ring median tightly pinned to a stable metronome without positive feedback.
  - At 90 FPS cap or uncapped, the game became **GPU-bound** (~28–35 ms). Intel's `0x224B30` computed `step = min(median, fNs) / countPlus1` (e.g. 4.66 ms for 28 ms at 6X).
  - However, `TsDetour` previously computed `(unit - nativeUnit) = 14.0 - 4.66 = +9.34 ms` offset to `*out` and set `g_burstStepNs = unit (14.0 ms)`.
  - By replacing Intel's pure render time step with the wait-polluted median, `TsDetour` stalled the presentation thread for 56 ms per cycle, creating the 71.4–74 FPS resonant lock!
- **Architectural Resolution in `XeFGPacing.h`**:
  - When `rawF >= 1.0f` (valid decoupled `frameRenderTime` fed via `TagFrameConstants`), set `targetUnit = nativeUnit`.
  - This ensures `(targetUnit - nativeUnit) == 0` (zero offset to `*out`) and `g_burstStepNs = targetUnit`, preserving Intel's pure decoupled render time step.
  - When `rawF < 1.0f` (e.g. `FTInput=Zero` where Intel clamps to 0.125 ms), `targetUnit` remains `unit` (`median / countPlus1`), cleanly rebasing Intel's sub-millisecond clump without resonant locking.
  - In `ScheduleFrame`: anchor `g_targetQpc = after.QuadPart` so the final generated frame (`isLast`) seamlessly steps by one interval from the last intermediate frame.
  - In `PaceFrame`: anchor `g_targetQpc = g_lastPacedQpc + g_intervalQpc` when intermediate frames were handled by Intel's scheduler.

### 3.6 Windowed Deadline Clamping, isLast Deduplication & Render Time Stabilization (`OptiScaler_2.log`)
In-depth analysis of high-resolution runtime logs (`OptiScaler_2.log`) under uncapped rendering identified 3 additional timing defects causing severe sub-millisecond frame dumps and alternating frame time oscillations:
1. **GPU Execution Latency Anchors Deadlines in the Past**:
   - `XeFG_Dx12::Dispatch` / `SetPresentId` records $T_0$.
   - The GPU takes 5–10 ms executing DLSS-SR upscaling and XeFG optical flow.
   - By the time `ScheduleFrame` is called for Frame 1, wall-clock time is already $T_0 + 10\text{ ms}$.
   - Intel's `0x224B30` computed Frame 1's deadline as $T_0 + 5.1\text{ ms}$ (already expired!).
   - Consequently, Frame 2's deadline ($T_0 + 10.2\text{ ms}$) had also expired or arrived in $< 0.002\text{ ms}$, dumping Frame 2 immediately without presentation spacing.
   - **Fix**: In `TsDetour`, query `nowNs = NsFromQpc(nowQpc.QuadPart)` and apply windowed clamping on `candidateNs`:
     - **Cap**: `if (candidateNs > nowNs + unit) candidateNs = nowNs + unit;` (Frame 1 can never wait $> 1 \times \text{unit}$, eliminating the 10 ms / 2X bias).
     - **Floor**: `if (candidateNs < nowNs) candidateNs = nowNs;` (ensures Frame 1 is floored to current wall time, allowing Frame 2 to step forward into the future by $+1 \times \text{unit}$ and wait ~5 ms instead of 0.002 ms).
     - **Monotonic Step**: `if (nowNs > 0 && g_nextDeadlineNs < nowNs + (g_burstStepNs / 2)) g_nextDeadlineNs = nowNs + g_burstStepNs;`
2. **Double Wait on Final Generated Frame (`isLast == true`)**:
   - In `libxess_fg.dll`, native instruction `0x220411` calls `0x3100` (`SchedThunkRva` $\rightarrow$ `0x21EE30` / `TsDetour`) passing `index = count`, waiting for Frame 3's deadline.
   - Immediately afterward, `0x220462` (`LastFrameCallerRva`) was called with `isLast = true`.
   - In `pre13`, `TryPace` let `isLast` fall through to `PaceFrame()`, which executed a second `WaitUntil` for `+g_intervalQpc`, adding an unnecessary 5.25 ms wait (totaling 10.5 ms).
   - **Fix**: In `TryPace`, when `g_schedNative != nullptr && SchedulerUsable(ctx)`, return immediately for `isLast == true` since `0x220411` already scheduled it.
3. **Render Time Flip-Flop via `blockNs` Subtraction**:
   - Because `ScheduleFrame` runs on Intel's asynchronous worker thread, the thread's wait time was accumulated into `g_burstBlockQpc`.
   - In `NoteFrame`, subtracting `blockNs` from `g_periodNs` ($25\text{ ms} - 21\text{ ms} = 4\text{ ms}$) caused `RenderTimeMs()` to collapse to 4.0 ms.
   - Feeding 4.0 ms into `TagFrameConstants` made Intel compute $4.0 / 4 = 1.0\text{ ms}$ steps, causing the next burst to dump in 1.0 ms.
   - With 1.0 ms steps, `blockNs` dropped to 3 ms, making the next cycle compute $25 - 3 = 22\text{ ms}$, creating an alternating flip-flop between 22 ms and 4 ms.
   - **Fix**: In `NoteFrame`, set `g_renderTimeNs = g_periodNs` directly from the stable 15-sample median cycle period, eliminating the 25 ms $\leftrightarrow$ 4 ms oscillation.

---

## 4. Verification & Standards Compliance

1. **Automated Unit Tests**:
   - `tests/xemfg_loader_unit.cpp` (11/11 PASS)
   - `tests/xefg_diagnostic_ring_unit.cpp` (6/6 PASS, including Test 6 for windowed deadline clamping and monotonic step)
   - `tests/xefg_pacing_engine_unit.cpp` (7/7 PASS)
   - `tests/xefg_main_pacing_alignment_unit.cpp` (5/5 PASS, including Test 5 for windowed clamping, hitch recovery, and isLast deduplication)
   - `tests/xemfg_menu_exclusion_unit.cpp` (6/6 PASS)
   - `tests/dlssg_sm86_ini_smoke.cpp` (PASS)
2. **Formatting & Encoding**:
   - `clang-format --dry-run --Werror` verified clean across all modified files (`XeFGPacing.h`, `XeFG_Dx12.cpp`, `menu_common.cpp`, `dllmain.cpp`, `tests/xefg_diagnostic_ring_unit.cpp`, `tests/xefg_main_pacing_alignment_unit.cpp`).
   - UTF-8 BOM (`\xef\xbb\xbf`) verified and preserved on all modified files.
3. **Architecture Gate Compliance**:
   - In accordance with Rule 8 in `workflow-planning.md`, `.agents/rules/optiscaler-architecture.md` and `.agents/CONTEXT.md` remain untouched pending real in-game verification by the user.

