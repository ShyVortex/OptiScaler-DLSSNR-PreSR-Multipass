# Intel XeMFG Presentation Pacing Fix: Redundant Deadline Hook Elimination & Feedback Loop Resolution

## 1. Overview & Context

- **Operating System**: Windows 11 x64 / Windows 10 x64.
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll`) with OptiScaler XeMFG multi-frame unlocker.
- **Target Application**: RE Engine games (specifically *Resident Evil Requiem* / `re9.exe`) and other Direct3D 12 titles utilizing DLSS Frame Generation or XeFG.
- **Observed Log File**: `OptiScaler_test9.log`.

---

## 2. Reported Symptoms

When running *Resident Evil Requiem* on Windows with Intel XeSS-FG and native XeMFG:
1. Pausing the game temporarily disabled XeFG as expected.
2. Upon resuming gameplay, presentation started smoothly for approximately 1–2 seconds.
3. Rapidly thereafter, frame times progressively degraded and stretched (rising from 25ms to over 80ms), with frame rate collapsing into severe, escalating stutters.
4. Inspection of `OptiScaler_test9.log` revealed escalating presentation delays, frame-time dilation, swapchain backbuffer queue exhaustion, and warnings such as `XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES` (`0x80000004`).

---

## 3. Issue Validity Assessment & Root Cause Analysis

### 3.1 Defect Verification: Genuine Architecture Bug
Investigation and binary disassembly confirmed a critical defect in OptiScaler's presentation pacing interception logic:

1. **Native Pacing Function Disassembly (`libxess_fg.dll` at `0x180224b30`)**:
   Reverse engineering of the binary at `0x180224b30` in `libxess_fg.dll` using `objdump -d` revealed Intel's native presentation deadline algorithm:
   ```asm
   180224b30:   mov    %r8,0x18(%rsp)
   180224b35:   push   %r14
   180224b37:   push   %rsi
   180224b38:   push   %rdi
   ...
   180224be5:   cmpl   $0x1,0x58(%rsp)     ; Check if totalFrames <= 1
   180224bea:   jbe    180224c30          ; Branch: immediate presentation
   180224bec:   cmpl   $0x0,0x50(%rsp)     ; Check if frameIndex == 0 (base frame)
   180224bf1:   je     180224c30          ; Branch: immediate presentation
   180224bf3:   mov    0x58(%rsp),%ecx    ; ecx = totalFrames
   180224bf7:   mov    0x50(%rsp),%eax    ; eax = frameIndex
   180224bfb:   cltd
   180224bfc:   idiv   %ecx               ; Divide
   180224bfe:   imul   %r14,%rax          ; Multiply by frameInterval
   180224c02:   add    %rbx,%rax          ; Add baseTimestamp
   180224c05:   sub    %r12,%rax          ; Subtract latencyOffset
   180224c08:   mov    %rax,(%rsi)        ; Store computed presentation deadline
   ```

   **Key Finding**:
   Intel's native provider function **already** computes the exact deadline formula for all multipliers and intermediate frames:
   $$\text{deadline} = \text{baseTimestamp} + \frac{\text{frameIndex} \cdot \text{frameInterval}}{\text{totalFrames}} - \text{latencyOffset}$$
   where:
   - Stack argument 5 (`0x50(%rsp)`) is `frameIndex` ($1 \le \text{frameIndex} < \text{totalFrames}$).
   - Stack argument 6 (`0x58(%rsp)`) is `totalFrames` ($N + 1$, where $N$ is the number of generated frames).
   - `%r14` is `frameInterval`.
   - `%rbx` is `baseTimestamp`.
   - `%r12` is `latencyOffset`.

2. **The Double-Pacing Bug in OptiScaler**:
   In `OptiScaler/framegen/xefg/XeMfgLoader.cpp`, OptiScaler installed an inline Detours hook (`hkCalculateDeadline`) on this exact function:
   ```cpp
   // Former code in XeMfgLoader.cpp:
   static int64_t __fastcall hkCalculateDeadline(void* pContext, int64_t baseTimestamp,
                                                 int64_t frameInterval, int64_t latencyOffset,
                                                 uint32_t frameIndex, uint32_t totalFrames)
   {
       int64_t deadline = o_CalculateDeadline(pContext, baseTimestamp, frameInterval, latencyOffset,
                                              frameIndex, totalFrames);
       if (totalFrames > 2 && frameIndex > 0)
       {
           int64_t step = frameInterval / static_cast<int64_t>(totalFrames);
           deadline += static_cast<int64_t>(frameIndex) * step; // <-- BUG: REDUNDANT ADDITION!
       }
       return deadline;
   }
   ```
   Because `o_CalculateDeadline` already added $\text{frameIndex} \cdot (\text{frameInterval} / \text{totalFrames})$, the hook added it a second time.

3. **Runaway Positive Feedback Loop**:
   - For an intermediate frame, the scheduled deadline was placed twice as far into the future as intended.
   - The presentation worker thread waited double the allotted interval before releasing the frame to DXGI `Present`.
   - Because the presentation was artificially stalled, the wall-clock interval measured for the subsequent frame (`lastFGFrameTime`) expanded.
   - On the next frame, the enlarged `frameInterval` was multiplied again by the redundant hook offset, pushing the deadline even further into the future.
   - This runaway cascade caused swapchain backbuffer starvation, backpressure into the render pipeline, `XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES` warnings, and frametime collapse from 25ms to 80ms within seconds of unpausing.

---

## 4. Technical Solutions Applied

### 4.1 Complete Removal of the Redundant Hook (`XeMfgLoader.cpp`)
- Removed `hkCalculateDeadline`, `o_CalculateDeadline`, and the Detours hook installation from `OptiScaler/framegen/xefg/XeMfgLoader.cpp`.
- Removed `<detours/detours.h>` header dependency.
- Allowed `libxess_fg.dll` to execute its native, verified presentation deadline algorithm without external interference.

### 4.2 Updated Telemetry & In-Game Menu
- In `OptiScaler/framegen/xefg/XeMfgLoader.h`, updated `XeMfgLoader::Status` to reflect that presentation deadline calculation is natively verified within `libxess_fg.dll`.
- In `OptiScaler/menu/menu_common.cpp`, updated the pacing status badge from `"active (0x224b30 hook)"` to `"active (native provider)"`.

### 4.3 Clarification of Extra Pacing Guidance (Option A)
- Retained `XeMfgExtraPacing` (`Display VBlank Sync (Extra Pacing)`) as an optional feature for environments where display tearing without V-Sync is intolerable.
- Updated the in-game menu tooltip in `OptiScaler/menu/menu_common.cpp` to explicitly recommend **OFF**:
  > `"Forces SyncInterval=1 and disables tearing on generated frames.\nRecommended: OFF (Default) — allows XeSS-FG to pace presentations natively.\nEnable only if your display exhibits severe tear-lines without V-Sync."`
- Updated inline comments in `OptiScaler/Config.h` and documented `ExtraPacing=false` in `OptiScaler.ini` under `[XeMFG]`.

### 4.4 Internal Swapchain Logging Callback Verification
- Verified that `XeFG_Dx12::xefgLogCallback` in `OptiScaler/framegen/xefg/XeFG_Dx12.cpp` is registered with `libxess_fg.dll` at swapchain creation, providing full internal diagnostics from `libxess_fg.dll` and `libxell.dll`.

---

## 5. Automated Unit Verification

Updated and executed comprehensive automated test suites:

1. **[tests/xemfg_loader_unit.cpp](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/tests/xemfg_loader_unit.cpp)**:
   - **Test 8 (Native Provider Presentation Deadline Calculation)**: Validates that Intel's native provider formula calculates monotonic, evenly spaced presentation intervals across all intermediate frames (e.g. 3X FG, 4X FG).
   - Validated that all 8 unit tests in `xemfg_loader_unit` pass cleanly.

2. **Full Regression Suite**:
   - `tests/xemfg_loader_unit.cpp` (8/8 PASS)
   - `tests/xefg_mfg_streamline_unit.cpp` (7/7 PASS)
   - `tests/xefg_save_load_stability_unit.cpp` (5/5 PASS)
   - `tests/rtss_xefg_pacing_immunity_unit.cpp` (3/3 PASS)
   - `tests/xefg_ring_buffer_pacing_unit.cpp` (4/4 PASS)

3. **Compiler & Clang-Format Compliance**:
   - UTF-8 BOM (`\xef\xbb\xbf`) preserved on all modified files.
   - `clang-format --dry-run --Werror` verified clean with zero violations.
