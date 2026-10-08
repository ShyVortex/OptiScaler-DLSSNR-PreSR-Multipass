# RTSS (RivaTuner Statistics Server) Reflex Injection Filtering & XeFG Pacing Protection

## 1. Overview & Context

- **Operating System**: Windows 11 x64 / Windows 10 x64.
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll`) with OptiScaler native XeMFG and Streamline Reflex hooks.
- **Third-Party Application**: RivaTuner Statistics Server (RTSS, `RTSSHooks64.dll`).
- **Target Applications**: Direct3D 12 games utilizing NVIDIA Reflex or DLSS Frame Generation (e.g. *Resident Evil Requiem*, *Cyberpunk 2077*).

---

## 2. Reported Symptoms

When running Intel XeSS Frame Generation on Windows with RTSS active in the background, users observed:
1. OptiScaler displayed an in-game warning:
   > `"RTSS + XeFG detected: RTSS Reflex Injection is known to cause issues. Especially when using XeFG. Please disable it."`
2. If RTSS Reflex injection was left active, users experienced micro-stutters, periodic frame drops, and erratic frame-time pacing.
3. Users requested architectural precautions to ensure that RTSS (for OSD monitoring or framerate limiting) does not degrade presentation smoothness or corrupt frame pacing when Intel XeFG is active.

---

## 3. Issue Validity Assessment & Root Cause Analysis

### 3.1 Defect Verification: Genuine Hazard
Investigation of `OptiScaler/hooks/Reflex_Hooks.cpp` and RTSS hook mechanics confirmed that the issue was a genuine architectural collision:

1. **Synthetic Marker Generation with High 32-bit Signatures**:
   - RTSS injects artificial calls to `NvAPI_D3D_SetLatencyMarker` to calculate latency metrics even in games that do not natively provide Reflex markers.
   - RTSS tags its synthetic markers with a high-bit token (`pSetLatencyMarkerParams->frameID >> 32 != 0`), generating 64-bit frame IDs in excess of 4 billion (e.g. `0x0000000100000042` = 4,294,967,362).

2. **Frame Counter Corruption in `IFGFeature`**:
   - In `Reflex_Hooks.cpp`, line 216:
     ```cpp
     if (pSetLatencyMarkerParams->markerType == PRESENT_START && State::Instance().currentFG != nullptr)
     {
         if (pSetLatencyMarkerParams->frameID != frameCount)
             State::Instance().currentFG->SetFrameCount(pSetLatencyMarkerParams->frameID);
     }
     ```
   - OptiScaler passed the 64-bit RTSS synthetic frame ID directly to `SetFrameCount()`, jumping `IFGFeature::_frameCount` from normal game values (e.g. 50) directly to billions.
   - This corrupted the ring buffer index (`_frameCount % BUFFER_COUNT`) and caused massive discontinuities in `GetDispatchIndex` (`_frameCount - _lastDispatchedFrame`).
   - When the next actual game frame was rendered (e.g. frame 51), `IFGFeature` encountered an apparent 4-billion-frame counter rewind, breaking dispatch synchronization.

3. **Spurious Asynchronous FG Reset**:
   - In `Reflex_Hooks.cpp`, lines 115–118:
     ```cpp
     if (_lastAsyncMarkerFrameId + 10 < pSetLatencyMarkerParams->frameID)
     {
         _FgNumFramesToGenerate = 0;
     }
     ```
   - Because RTSS frame IDs exceeded 4 billion, `_lastAsyncMarkerFrameId + 10 < frameID` evaluated to true on every frame, resetting `_FgNumFramesToGenerate = 0` and breaking low-latency pacing calculations.

---

## 4. Technical Precautions Applied

1. **Synthetic Marker Identification & Isolation (`Reflex_Hooks.cpp`)**:
   - Detected RTSS markers using `const bool isRtssMarker = (pSetLatencyMarkerParams->frameID >> 32) != 0;`.
   - Strictly insulated `_FgNumFramesToGenerate` resets so RTSS markers do not trigger spurious resets:
     ```cpp
     if (!isRtssMarker && _lastAsyncMarkerFrameId + 10 < pSetLatencyMarkerParams->frameID)
         _FgNumFramesToGenerate = 0;
     ```
   - Strictly gated `currentFG->SetFrameCount()` behind `!isRtssMarker`. RTSS synthetic markers are ignored for frame generation synchronization, ensuring only legitimate game-native markers advance the pipeline.

2. **Latency ID Sanitization**:
   - Sanitized `State::Instance().reflexFrameId` using 32-bit masking (`frameID & 0xFFFFFFFF`) when RTSS markers are present, preventing integer overflows in telemetry displays and log outputs.

3. **Status Indication & Notification Updates**:
   - Replaced the warning toast with an informative notification:
     > `"RTSS + XeFG Active: RTSS Reflex Injection detected. OptiScaler is filtering synthetic markers to protect XeFG frame pacing."`
   - In `OptiScaler/menu/menu_common.cpp`, updated the low-latency method status badge to display `" (RTSS - Protected)"`.

4. **Swapchain Hook Contention Resolution**:
   - The removal of the forced `SyncInterval = 1` override in `wrapped_swapchain.cpp` (resolved in Task 2) ensures that RTSS's framerate limiter and DXGI present hooks operate smoothly without flip queue deadlocks or alternating 260ms VBlank stalls.

---

## 5. Automated Unit Verification

Implemented [tests/rtss_xefg_pacing_immunity_unit.cpp](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/tests/rtss_xefg_pacing_immunity_unit.cpp):
- **Test 1 (Synthetic Marker Filtering)**: Confirms that RTSS synthetic markers (`frameID >> 32 != 0`) are filtered from `SetFrameCount`, `reflexFrameId` is sanitized to 32 bits, and `_FgNumFramesToGenerate` is preserved.
- **Test 2 (Game-Native Marker Synchronization)**: Confirms that legitimate 32-bit game markers update `SetFrameCount` and Reflex tracking smoothly.
- **Test 3 (Interleaved RTSS Marker Flood)**: Confirms that concurrent, high-frequency RTSS synthetic marker injection does not disrupt XeFG ring buffer dispatch, maintaining perfect 1:1 frame pacing.

```text
Running RTSS + XeFG Pacing & Marker Immunity Unit Tests...
  [PASS] Test 1: RTSS synthetic marker filtered from SetFrameCount and ID sanitized
  [PASS] Test 2: Legitimate game-native marker updates frame counter normally
  [PASS] Test 3: Interleaved RTSS injection maintains flawless 1:1 frame dispatch pacing
All RTSS + XeFG Pacing & Marker Immunity Unit Tests PASSED!
```
