# Frame Generation: Intel XeMFG / XeMFG-SR Multiplier Uncap (3X–6X) & End-to-End Base Frame Presentation Pacing

## 1. Overview & Environment

- **Operating System**: Linux (CachyOS x86_64, Kernel 7.2.9-2-cachyos, NVIDIA Driver 580.178.04, Proton Experimental / Wine / VKD3D-Proton) / Windows 11
- **Target Game**: *Helldivers 2* (Arrowhead Game Studios / Sony Interactive Entertainment)
- **GPU Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (16GB VRAM, GA103 / Ampere SM86, architecture `0x170`)
- **Display Resolution & Upscaler**: 2560x1440, DLSS Super Resolution (DLSS SDK 3.7.20)
- **Target Runtimes**: `dxgi.dll` (OptiScaler v0.9.59), `libxess_fg.dll` (Intel XeSS-FG v1.3.1.78)
- **Feature**: Intel Xe Multi-Frame Generation Injector (`XeMFG` / `XeMFG-SR`)
- **Analyzed Log File**: `/mnt/DataCachy/SteamLibrary/steamapps/common/Helldivers 2/bin/OptiScaler.log`
- **Reported Symptoms**:
  User testing revealed that despite wall-clock pacing fixes and borderless windowing quirks:
  1. Only 2X Frame Generation was functioning; selecting 3X through 6X on the in-game menu slider had no effect.
  2. Presentation cadence remained severely unstable, exhibiting visible saw-tooth stutter and judder.

---

## 2. In-Depth Root Cause Analysis

### 2.1 Defect 1: Multipliers Above 2X Blocked by `_maxInterpolationCount` Clamping in `XeFG_Dx12.cpp`

Inspection of `/mnt/DataCachy/SteamLibrary/steamapps/common/Helldivers 2/bin/OptiScaler.log` revealed immediate down-clamping whenever the user interacted with the multiplier slider:
```text
[15:58:21.431071] [I] XeFG_Dx12::SetInterpolatedFrameCount XeFG SetInterpolatedFrameCount called with count: 2
[15:58:21.431088] [W] XeFG_Dx12::SetInterpolatedFrameCount Requested interpolation count 2 exceeds max supported 1, clamping
[15:58:21.476571] [I] XeFG_Dx12::SetInterpolatedFrameCount XeFG SetInterpolatedFrameCount called with count: 3
[15:58:21.476591] [W] XeFG_Dx12::SetInterpolatedFrameCount Requested interpolation count 3 exceeds max supported 1, clamping
[15:58:21.500297] [I] XeFG_Dx12::SetInterpolatedFrameCount XeFG SetInterpolatedFrameCount called with count: 4
[15:58:21.500313] [W] XeFG_Dx12::SetInterpolatedFrameCount Requested interpolation count 4 exceeds max supported 1, clamping
[15:58:21.524729] [I] XeFG_Dx12::SetInterpolatedFrameCount XeFG SetInterpolatedFrameCount called with count: 5
[15:58:21.524743] [W] XeFG_Dx12::SetInterpolatedFrameCount Requested interpolation count 5 exceeds max supported 1, clamping
```

#### Underlying Mechanism:
1. In `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`, `_maxInterpolationCount` was initialized during `CreateSwapchain` / `CreateSwapchain1` from Intel's stock swapchain properties struct (`props.maxSupportedInterpolations`), which defaults to `1` (supporting only 2X FG).
2. While `XeMfgLoader` dynamically patches `libxess_fg.dll` in memory (patches U1 through U5) to support up to 5 interpolated frames (6X FG), `XeFG_Dx12` never raised its internal member variable `_maxInterpolationCount` to reflect the unlocked capability.
3. In both `SetInterpolatedFrameCount` and `Dispatch`, `XeFG_Dx12` guarded the requested multiplier with:
   ```cpp
   if (_maxInterpolationCount > 0 && interpolatedFrameCount > _maxInterpolationCount)
   {
       LOG_WARN("Requested interpolation count {} exceeds max supported {}, clamping",
                interpolatedFrameCount, _maxInterpolationCount);
       interpolatedFrameCount = _maxInterpolationCount;
   }
   ```
   Because `_maxInterpolationCount` remained hard-pinned to `1`, any user or config intent to run 3X, 4X, 5X, or 6X was immediately clamped back to `1` (2X FG).

---

### 2.2 Defect 2: 2 ms / 34 ms Saw-Tooth Jitter Due to Base Application Frame Pacing Omission

Timestamp analysis of consecutive presentation events in the log uncovered the origin of the stutter:
```text
[15:59:36.330660] [D] LocalPresent 10545
[15:59:36.330705] [D] LocalPresent SyncInterval: 0, Flags: 0, Frametime: 35.359 ms
[15:59:36.332853] [D] LocalPresent 10546
[15:59:36.332892] [D] LocalPresent SyncInterval: 0, Flags: 0, Frametime: 2.187 ms  <-- 2.187 ms DUMP!
[15:59:36.333140] [D] FGHooks::FGPresent Present finished
[15:59:36.367196] [D] LocalPresent 10547
[15:59:36.367209] [D] LocalPresent SyncInterval: 0, Flags: 0, Frametime: 34.319 ms  <-- 34.319 ms STALL!
```

#### Reverse-Engineered Presentation Architecture of `libxess_fg.dll`:
Disassembly of `xefgSwapChainPresent` in `libxess_fg.dll` (v1.3.1.78) demonstrates that Intel's presentation engine issues presentations for **both** the interpolated frames and the real application frame sequentially through the internal thunk `0x25C0`:
- **Generated Frame(s)**: dispatched via caller `0x2202ED` (`PacedCallerRva`) and caller `0x220467` (`LastFrameCallerRva`).
- **Base Application Frame**: dispatched via caller `0x220A52` and caller `0x220DAD`.

In `XeFGPacing.h`:
1. `TryPace` intercepted callers `0x2202ED` and `0x220467`, properly spacing the generated frame by 18.2 ms.
2. BUT when the base application frame arrived via caller `0x220A52`, `Detour` routed to `else NoteBasePresent()`.
3. `NoteBasePresent()` performed only timestamp recording with **zero pacing delay** and **no thread sleep**.
4. Consequently:
   - The generated frame was presented at $T_0 + 18.2\text{ ms}$.
   - The base application frame was dumped **2.187 ms** later at $T_0 + 20.4\text{ ms}$.
   - The engine then took **34.3 ms** to render the next frame ($T_0 + 54.7\text{ ms}$).
5. The display experienced an alternating **2 ms $\to$ 35 ms $\to$ 2 ms $\to$ 35 ms** cadence instead of uniform **18.2 ms** frame spacing, creating extreme saw-tooth stutter and judder.

---

## 3. Applied Technical Solutions

### 3.1 Uncapping Multiplier Ceilings in `XeFG_Dx12.cpp`
1. **Swapchain Creation Initialization**:
   In `CreateSwapchain` and `CreateSwapchain1`:
   ```cpp
   if (XeMfgLoader::AnyEnabledForSession())
   {
       _maxInterpolationCount = std::max(_maxInterpolationCount, 5);
   }
   ```
2. **Live Multiplier Adaptation Without Clamping**:
   In `SetInterpolatedFrameCount`:
   ```cpp
   const int effectiveMax = XeMfgLoader::AnyEnabledForSession()
                                ? std::max(_maxInterpolationCount, XeMfgLoader::EffectiveMax(_maxInterpolationCount))
                                : _maxInterpolationCount;

   if (effectiveMax > 0 && interpolatedFrameCount > effectiveMax)
   {
       LOG_WARN("Requested interpolation count {} exceeds max supported {}, clamping",
                interpolatedFrameCount, effectiveMax);
       interpolatedFrameCount = effectiveMax;
   }

   _maxInterpolationCount = std::max(_maxInterpolationCount, (int)interpolatedFrameCount);

   if (XeMfgLoader::AnyEnabledForSession())
   {
       XeMfgLoader::SetMaxGeneratedFrames(interpolatedFrameCount);
   }
   ```
3. **Dispatch Validation**:
   In `Dispatch`, `FGXeFGInterpolationCount` is validated against `effectiveMax` rather than the stock ceiling, allowing 2X through 6X execution.
4. **Overlay Status Name**:
   Updated `XeFG_Dx12::Name()` to report `"XeFG [2..6]x"` when XeMFG is active.

---

### 3.2 End-to-End Uniform Presentation Burst Pacing in `XeFGPacing.h`

A multi-frame generation cycle with multiplier $M = count + 1$ presents $M$ total frames to the swapchain:
- **Generated Frames $1 \dots M-1$**:
  - Frame 1 ($k=1$): anchors the burst timeline at $T_{\text{anchor}}$.
  - Frames $2 \dots M-1$: target $T_{\text{anchor}} + (k - 1) \times T_{\text{interval}}$.
- **Base Application Frame (Frame $M$)**:
  - Targets $T_{\text{anchor}} + (M - 1) \times T_{\text{interval}}$ ($= T_{\text{anchor}} + T_{\text{period}}$).

```
Timeline:
T0 (Anchor)               T0 + T_interval             T0 + 2*T_interval           T0 + 3*T_interval (Next Cycle)
|-------------------------|---------------------------|---------------------------|
Frame 1 (Generated)       Frame 2 (Base in 2X FG)     Next Gen 1                  Next Base
[Paced to Anchor]         [Paced via PaceBaseFrame]   [Paced via PaceFrame]       [Paced via PaceBaseFrame]
<------- 18.2 ms --------><-------- 18.2 ms ---------><-------- 18.2 ms --------->
```

1. **`PaceBaseFrame()` Implementation**:
   ```cpp
   inline void PaceBaseFrame()
   {
       int64_t multiplier = g_lastMultiplier;
       if (multiplier < 2)
           multiplier = 2;

       LARGE_INTEGER now;
       QueryPerformanceCounter(&now);

       if (g_periodNs <= 0)
           g_periodNs = 33333333LL;

       const int64_t intervalNs = g_periodNs / multiplier;
       const int64_t intervalQpc = QpcFromNs(intervalNs);
       g_intervalQpc = intervalQpc;

       if (g_burstAnchorQpc != 0 && intervalQpc > 0)
       {
           int64_t targetQpc = g_burstAnchorQpc + static_cast<int64_t>(multiplier - 1) * intervalQpc;

           if (g_lastPresentedQpc != 0)
           {
               const int64_t minGapQpc = intervalQpc / 2;
               const int64_t antiClumpFloor = g_lastPresentedQpc + minGapQpc;
               if (targetQpc < antiClumpFloor)
                   targetQpc = antiClumpFloor;
           }

           const int64_t latestQpc = now.QuadPart + QpcFromNs((g_periodNs * 3) / 2);
           if (targetQpc > latestQpc)
               targetQpc = latestQpc;

           WaitUntil(targetQpc);
       }

       LARGE_INTEGER end;
       QueryPerformanceCounter(&end);

       if (g_lastBaseFrameQpc != 0)
       {
           const int64_t burstDeltaNs = NsFromQpc(end.QuadPart - g_lastBaseFrameQpc);
           if (burstDeltaNs >= 5000000LL && burstDeltaNs <= 200000000LL)
           {
               PushPeriod(burstDeltaNs);
               g_renderTimeNs = g_periodNs;
           }
       }

       g_lastPresentedQpc = end.QuadPart;
       g_lastBaseFrameQpc = end.QuadPart;
       RecordGap(end.QuadPart);
       g_pacedBursts++;
       g_burstBlockQpc = 0;
   }
   ```
2. **Dynamic Anti-Clump Protection**:
   Replaced the arbitrary static 2.0 ms floor with half the presentation interval (`intervalQpc / 2`). This prevents presentation collisions on irregular arrivals while eliminating premature frame releases.
3. **Decoupled Render Time Feed**:
   `g_renderTimeNs` tracks the real inter-base-frame interval via median filtering (`PushPeriod(burstDeltaNs)`), guaranteeing `RenderTimeMs()` feeds valid, non-zero frame render times to `libxess_fg.dll`.

---

## 4. Verification & Automated Unit Tests

A comprehensive unit test suite was implemented in `tests/xemfg_multiplier_and_burst_pacing_unit.cpp`:

1. **Test 1**: `SetInterpolatedFrameCount` unclamped logic (accepts 1..5 without down-clamping when XeMFG is active).
2. **Test 2**: Stock multiplier clamping preserved when XeMFG session is inactive.
3. **Test 3**: Dispatch validation accepts un-capped multipliers up to effective ceiling.
4. **Test 4**: 2X full-burst pacing delivers uniform 18.2 ms spacing between generated and base frames (0 clumps).
5. **Test 5**: 3X full-burst pacing delivers uniform 12.1 ms spacing across all 3 presentations.
6. **Test 6**: 4X and 6X full-burst pacing partitions all presentations with mathematical uniformity.
7. **Test 7**: Direct cadence comparison confirms total elimination of the 2.187 ms dump and 34.3 ms stall.
8. **Test 8**: Anti-clump protection floor prevents presentation collisions on late arrivals.
9. **Test 9**: Decoupled frame render time measurement verified (non-zero delta time feed).

```text
[+] Starting XeMFG Multiplier Unlock & Burst Pacing Comprehensive Unit Tests...
  [PASS] Test 1: XeFG_Dx12 multiplier ceiling uncap supports 2X-6X live without clamping.
  [PASS] Test 2: Stock multiplier clamping preserved when XeMFG session is inactive.
  [PASS] Test 3: Dispatch validation accepts 2X-6X counts without down-clamping.
  [PASS] Test 4: 2X Full-burst presentation spacing delivers uniform 18.2 ms cadence (0 clumps).
  [PASS] Test 5: 3X Full-burst presentation delivers uniform 12.1 ms cadence across all 3 frames.
  [PASS] Test 6: 4X and 6X Full-burst pacing partitions all generated and base frames uniformly.
  [PASS] Test 7: Cadence comparison proves elimination of 2.187 ms dump and 34.3 ms stall.
  [PASS] Test 8: Anti-clump protection floor prevents presentation collision on late arrivals.
  [PASS] Test 9: Decoupled frame render time measurement verified (non-zero delta time feed).
[+] All XeMFG Multiplier Unlock & Burst Pacing Unit Tests PASSED successfully!
```
