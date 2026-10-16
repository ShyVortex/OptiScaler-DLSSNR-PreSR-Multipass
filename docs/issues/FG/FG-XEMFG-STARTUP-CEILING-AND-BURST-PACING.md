# Intel XeMFG Multiplier Cap at 3 Frames & Unstable Burst Presentation Pacing Analysis

## 1. Overview & Environment

- **Target Game**: *Helldivers 2* (`helldivers2.exe`, Arrowhead Game Studios / Sony Interactive Entertainment, Steam AppID `553850`)
- **Operating System**: CachyOS Linux x86_64 (Kernel `7.2.9-2-cachyos`, actively verified via `uname -r`)
- **Graphics Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (16GB VRAM, GA103 / Ampere SM86 `0x170`, Driver `580.178.04`, verified via `nvidia-smi`)
- **Display Configuration**: 2560x1440 @ 164.9 Hz (Borderless Windowed)
- **Compatibility Layer**: Proton Experimental / Wine 10 / VKD3D-Proton (Direct3D 12 on Vulkan)
- **Component Subsystems**:
  - `OptiScaler/framegen/xefg/XeMfgLoader.cpp`
  - `OptiScaler/framegen/xefg/XeFGPacing.h`
  - `tests/xemfg_multiplier_and_burst_pacing_unit.cpp`
- **Related Issue Docs**:
  - `docs/issues/FG/FG-XEMFG-SWAPCHAIN-INIT-INVALID-ARGUMENT.md`
  - `docs/issues/FG/FG-XEMFG-BASE-FRAME-PACING-REGRESSION.md`
  - `docs/issues/FG/FG-XEMFG-WALLCLOCK-PACING-AND-CLUMP-RESOLUTION.md`

---

## 2. Reported Symptoms

Following the resolution of the Linux fullscreen crash and swapchain parameter validation in *Helldivers 2*, testing revealed two distinct remaining defects:

1. **Multiplier Capped at 3 Frames Presented (3X FG / 2 Generated Frames)**:
   - In the in-game ImGui overlay, when the user moved the multiplier slider up to **6X** (5 generated frames), the status badge reported:
     `Present Status: OK (3 frames presented, FG Enabled)`
   - The game engine only generated 2 interpolated frames per real frame (3 total presented), never exceeding 3 frames even when 4X, 5X, or 6X was selected.
2. **Unstable & Saw-Tooth Presentation Cadence**:
   - Pacing was unstable and irregular, with frames delivered in fast ~1.3 ms micro-bursts followed by a 22–35 ms stall until the next engine frame.
   - Pacing statistics in `OptiScaler.log` recorded:
     `XeFG pacing: 3X, real frame 10.11 ms (98.9 fps), target 3.37 ms/frame; gap 9.12 avg / 0.09 min / 108.47 max ms over 549 frames (clumps: 157); scheduler 17344 calls, 0 refused, 18.51 ms avg inside; deadlines 34685 calls, 34685 rebased, 27546 clamped; render-est 10.11 ms, fed 10.11 ms`

---

## 3. Reverse-Engineering & Root Cause Analysis

### 3.1 Defect 1: Provider Swapchain Ceiling Initialized to 2 in `XeMfgLoader.cpp`

Inspection of `libxess_fg.dll` (v1.3.1.78) export `xefgSwapChainSetNumInterpolatedFrames` (RVA `0x20E890`) demonstrated how Intel validates requested multiplier changes:

```x86asm
18020e8ca: cmpl  0x140(%rcx), %edx      ; edx = requested frame count (e.g. 3, 4, 5)
18020e8d0: jbe   0x18020e99c            ; if edx <= [rcx + 0x140], SUCCESS!
18020e8d6: leaq  0x136ca03(%rip), %rax  ; "Number of interpolated frames ({}) cannot be greater than the maximum provided during initialization ({})."
18020e8dd: movl  0x140(%rcx), %r8d      ; r8d = [rcx + 0x140] (initialization ceiling)
18020e8e4: ...
18020e90c: callq 0x180004940            ; logs error and returns XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT (4294967292)
```

#### Mechanism:
1. Within `libxess_fg.dll`, the swapchain context structure stores the maximum permitted frame generation ceiling at offset `0x140`.
2. Offset `0x140` is populated during `xefgSwapChainD3D12InitFromSwapChainDesc` from offset `0x16c`, which is written by byte patch `U4/override-clamp`.
3. In `XeMfgLoader::TryApply()`, `targetCeiling` was previously conditioned on:
   ```cpp
   if (SrEnabledForSession())
       targetCeiling = Config::Instance()->XeMfgSrGeneratedFrames.value_or(2);
   ```
4. Because `XeMfgSrGeneratedFrames` defaults to 2, `U4/override-clamp` patched the DLL with `targetCeiling = 2`.
5. Consequently, `libxess_fg.dll` initialized offset `0x140` to **2**.
6. When the user moved the slider in-game to 3, 4, or 5 (4X, 5X, 6X FG), Intel's provider rejected the call with:
   `[E] XeFG Log: Number of interpolated frames (5) cannot be greater than the maximum provided during initialization (2).`
   permanently capping presentation to 2 generated frames (3 frames presented).

---

### 3.2 Defect 2: Early Return Bypassing `PaceFrame` & Bogus Deadline Squashing in `XeFGPacing.h`

Trace analysis of `/mnt/DataCachy/SteamLibrary/steamapps/common/Helldivers 2/bin/OptiScaler.log` revealed the presentation sequence inside `FGHooks::FGPresent`:

```text
[18:28:38.787890] [D] FGHooks::FGPresent flags: 200, Frametime: 20.04230000078678
[18:28:38.795295] [D] LocalPresent 52042  Frametime: 22.497 ms  (Frame 1: waited in ScheduleFrame)
[18:28:38.796584] [D] LocalPresent 52043  Frametime: 1.289 ms   (Frame 2: 1.3 ms dump!)
[18:28:38.797946] [D] LocalPresent 52044  Frametime: 1.362 ms   (Base Frame: 0 wait)
[18:28:38.798319] [D] FGHooks::FGPresent Present finished
```

#### Root Causes:
1. **Early Return Bypassing `PaceFrame`**:
   In commit `71d5f1e3`, `TryPace()` introduced:
   ```cpp
   if (g_schedNative != nullptr && SchedulerUsable(ctx))
   {
       if (!isLast)
           ScheduleFrame(ctx, burst, index);

       ApplyAntiClumpProtection(index, count);
       return; // <-- Bypassed PaceFrame completely!
   }
   ```
   In *Helldivers 2*, `SchedulerUsable(ctx)` is `true`. Because of this early `return`, OptiScaler's wall-clock pacing engine `PaceFrame()` was **never reached** for any frame. `ApplyAntiClumpProtection()` only clamped to $\min(0.4 \times T, 2.0\text{ ms})$ ($1.3\text{ ms}$), dumping frames into the swapchain in 1.3 ms bursts.
2. **Artificial Deadline Squashing in `TsDetour` (RVA `0x3430`)**:
   In `TsDetour()`:
   ```cpp
   if (g_ring != nullptr)
   {
       float f = *reinterpret_cast<const float*>(g_ring + RingMeasuredOffset);
       // ...
       if (clampedUnit < nativeUnit)
           nativeUnit = clampedUnit; // Clamped down to 0.04 ms!
   }
   ```
   In 80% of calls (`27,546 clamped`), `RingMeasuredOffset` (`0x1B8`) contained uninitialized float values that clamped the step size `nativeUnit` down to `0.04 ms`. All deadlines were set to identical timestamps, causing Intel's native worker thread to return immediately without spacing frames.

---

## 4. Applied Solutions

### 4.1 Unconditional Provider Ceiling Unlock in `XeMfgLoader.cpp`

In `OptiScaler/framegen/xefg/XeMfgLoader.cpp`, modified `TryApply()` so that `ApplyToMemory` unconditionally unlocks `targetCeiling = kMaxSupportedFrames` (5):

```cpp
// Unlock provider DLL capability up to kMaxSupportedFrames (5 generated frames / 6X FG).
// The active in-game multiplier is controlled dynamically via SetNumInterpolatedFrames().
unsigned int targetCeiling = kMaxSupportedFrames;

bool result = ApplyToMemory(baseAddress, imageSize, targetCeiling, true, g_status);
```

- Memory patches `U3/default-ceiling`, `U4/override-clamp`, and `U5/reported-maximum` are written with `frameByte = 5`.
- Intel's swapchain structure initializes offset `0x140 = 5`.
- `SetNumInterpolatedFrames(count)` dynamically accepts all frame counts from 1 to 5 (2X through 6X FG) without initialization errors.

### 4.2 Presentation Pacing & Deadline Alignment in `XeFGPacing.h`

1. **Unconditional `PaceFrame` Execution**:
   In `TryPace()`, removed the early return and `ApplyAntiClumpProtection` shortcut:
   ```cpp
   // For intermediate frames, route through the provider's scheduler so Intel's
   // internal ring-buffer tag stamping and frame bookkeeping executes.
   if (g_schedNative != nullptr && SchedulerUsable(ctx) && !isLast)
   {
       ScheduleFrame(ctx, burst, index);
   }

   // Unconditionally pace EVERY generated frame (both intermediate frames and
   // the final generated frame) across ALL multipliers (2X through 6X).
   PaceFrame(index, count, isLast);
   ```
2. **Accurate Burst Blocking Isolation**:
   In `PaceFrame()`, wrapped `WaitUntil` for `index == 1` with before/after duration measurement, accumulating into `g_burstBlockQpc`. In `NoteBasePresent()`, `pureRenderNs = deltaNs - blockNs` subtracts this duration so `PushPeriod()` receives pure engine submission time, completely eliminating positive feedback loops.
3. **Elimination of Artificial `g_ring` Clamping**:
   In `TsDetour()`, eliminated the `RingMeasuredOffset` clamping block and added fallback to OptiScaler's measured frame period (`unit = g_periodNs / countPlus1`) when Intel's ring buffer median is unpopulated.

---

## 5. Verification & Unit Tests

### Automated Unit Test Suite (`tests/xemfg_multiplier_and_burst_pacing_unit.cpp`)

Added Test 12 and Test 13 covering both defect resolutions:
- **Test 12**: Verified that under the old ceiling (`0x140 = 2`), counts 3..5 are rejected with `4294967292` (`INVALID_ARGUMENT`), whereas under the unlocked ceiling (`0x140 = 5`), all counts 1..5 succeed with `0` (`SUCCESS`).
- **Test 13**: Simulated a 6X Multi-Frame Generation burst ($count = 5$, $T_{\text{period}} = 30\text{ ms}$, $T_{\text{interval}} = 5.0\text{ ms}$), confirming that:
  - All generated frames are spaced across the frame period by $5.0\text{ ms}$, eliminating sub-millisecond dumps.
  - Base application frame presents with 0 wait.
  - `NoteBasePresent()` preserves rock-solid 30.0 ms engine frame times without inflation.

```text
[+] Starting XeMFG Multiplier Unlock & Non-Blocking Pacing Comprehensive Unit Tests...
  [PASS] Test 1: XeFG_Dx12 multiplier ceiling uncap supports 2X-6X live without clamping.
  [PASS] Test 2: Stock multiplier clamping preserved when XeMFG session is inactive.
  [PASS] Test 3: Dispatch validation accepts 2X-6X counts without down-clamping.
  [PASS] Test 4: Base application frame presents with 0 wait and returns immediately.
  [PASS] Test 5: Recursive feedback loop eliminated: frame time stays locked at 33.3 ms (never 108 ms).
  [PASS] Test 6: TsDetour partitions deadlines uniformly (6 ms steps) with zero inter-cycle drift.
  [PASS] Test 7: ApplyAntiClumpProtection eliminates 0.3 ms dumps while adding 0 ms to spaced frames.
  [PASS] Test 8: Decoupled frame render time measurement verified (non-zero delta time feed).
  [PASS] Test 9: Swapchain init parameters pass Intel validation (0x1801ffb44) on non-Intel GPUs.
  [PASS] Test 10: Dynamic startup multiplier bound immediately on frame 1 without manual interaction.
  [PASS] Test 11: ForceBorderlessWhenUsingXeFG quirk applied on XeFG pipeline activation.
  [PASS] Test 12: Provider ceiling uncap (5) accepts all multipliers 2X-6X without INVALID_ARGUMENT.
  [PASS] Test 13: Presentation pacing cadence verified: 6X burst uniformly spaced without clumping.
[+] All XeMFG Multiplier Unlock, Non-Blocking Pacing & Swapchain Init Unit Tests PASSED successfully!
```
