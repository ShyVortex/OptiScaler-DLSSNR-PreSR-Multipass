# External MFG Startup Gate: graceful no-op for game-owned FG

> **Correction (2026-10-27, DLSS Unlocked #58):** the root cause below is wrong. The
> `FGHooks::CreateSwapChainForHwnd Can't init FG Feature or invalid FGOutput setting!` warning was never fatal. In
> the v0.9.72 log the wrapped swapchain is created right after it, and the game was still failing after this change
> (`9f463e7` test build). The early return added in `17fe1221` is kept because it is correct and harmless. It only
> stops OptiScaler from attempting internal FG under External mode; it did not fix the launch. See
> `FG-WRC-GENERATIONS-EXTERNAL-MFG.md` for the full analysis.

## 1. Overview

This defect was reported as a startup failure when external frame generation was enabled for the Ampere SM86 MFG path. The underlying issue was not a user misconfiguration, but a startup guard that treated the intentionally external path as an invalid internal FG configuration and aborted swapchain creation.

Relevant code paths:
- `OptiScaler/hooks/FG_Hooks.cpp`
- `OptiScaler/dllmain.cpp`
- `OptiScaler/Config.cpp`
- `OptiScaler/State.h`

## 2. Reported Issue

The failure reproduced when the runtime determined that external frame generation was active and the game or external unlocker was taking ownership of MFG, while OptiScaler still evaluated the internal FG swapchain path. The startup log showed the external configuration being accepted, but the internal FG validation path still issued a fatal startup rejection instead of exiting cleanly.

Symptom pattern:
- `FrameGen.External=true`
- `DLSSG.AmpereMfgUnlock=true`
- `DLSSG.AmpereMfgMode=External`
- valid sideloaded external MFG setup
- `FGHooks::CreateSwapChainForHwnd` failing with an internal FG startup error instead of leaving the game-owned path alone

## 3. Root Cause

The architecture explicitly allows the external-MFG case:
- `State::Instance().externalFrameGeneration` is set at startup when the game or unlocker owns FG.
- OptiScaler applies volatile overrides so the internal FG path is not allowed to conflict with external ownership.

The bug was in the guard logic inside `CheckForFGStatus()` in `OptiScaler/hooks/FG_Hooks.cpp`:
- it treated `externalFrameGeneration` and the intentionally disabled internal-output state as an invalid FG setup,
- then returned a failure from the swapchain creation path,
- which caused the code to reject what should have been a clean no-op while the external mod owns the frame-generation pipeline.

This is a valid architecture state, not a user error.

## 4. Solution Applied

The fix keeps OptiScaler's internal FG startup path strictly disabled when the runtime is in external FG ownership mode, and exits cleanly instead of treating that as an invalid internal FG configuration.

The updated behavior:
- if `State::Instance().externalFrameGeneration` is active, skip internal FG swapchain creation,
- if no internal FG output is selected, skip internal FG swapchain creation,
- allow the original game swapchain creation path to continue without an artificial startup failure.

This preserves the intended design:
- external/frame-generation-proxy ownership remains valid,
- OptiScaler's internal FG is not started when it should not be active,
- the game or external unlocker continues controlling MFG without crashes.

## 5. Verification

The patch was validated by checking the exact startup gate in `FG_Hooks.cpp` and confirming the surrounding runtime logic in `dllmain.cpp` remains consistent with the architecture rules for external FG ownership.

The result is a graceful no-op rather than a forced internal-FG failure, which matches the project architecture and avoids a false “disable the feature” workaround.
