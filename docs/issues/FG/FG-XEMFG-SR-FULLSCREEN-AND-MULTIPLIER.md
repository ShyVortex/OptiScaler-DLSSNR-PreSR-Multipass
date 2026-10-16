# Frame Generation: XeMFG-SR Fullscreen Incompatibility, Live Multiplier Adaptation & OptiFG Auto-Management

## 1. Overview & Environment

- **Operating System**: Linux (CachyOS x86_64, Kernel 7.2.9-2-cachyos, NVIDIA Driver 580.178.04, Proton Experimental / Wine / VKD3D-Proton)
- **Target Game**: *Helldivers 2* (build 1.003.x, Arrowhead Game Studios / Sony Interactive Entertainment)
- **GPU Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (16GB VRAM, GA103 / Ampere SM86)
- **Display Resolution & Upscaler**: 2560x1440, DLSS Super Resolution (DLSS SDK 3.7.20)
- **Target Runtimes**: `dxgi.dll` (OptiScaler v0.7.x / v0.9.59), `libxess_fg.dll` (Intel XeFG v1.3.1)
- **Feature**: Intel Xe Multi-Frame Generation Injector for DLSS-SR (`XeMFG-SR`)
- **Reported Symptoms**:
  1. Interpolation failed to activate in exclusive fullscreen mode (`Present Status: OK (1 frames presented, FG Disabled)`).
  2. In borderless windowed mode, interpolation activated (`framesPresented=6, frameGenResult=SUCCESS`), but gameplay exhibited severe frame drops, micro-stutters, and visual judder.
  3. Modifying the in-game multiplier slider froze presentation and caused repeated deactivations.
  4. The in-game ImGui overlay rendered manual `Frame Generation (OptiFG)` controls and `Advanced OptiFG Settings` alongside XeMFG-SR.

---

## 2. Problem Statement & Root Cause Analysis

### 2.1 Intel XeSS-FG Exclusive Fullscreen Incompatibility
Log inspection at `/mnt/DataCachy/SteamLibrary/steamapps/common/Helldivers 2/bin/OptiScaler.log` revealed repeated critical errors from Intel `libxess_fg.dll`:
```text
[E] XeFG Log: Interpolation is not supported in fullscreen mode. Interpolation won't be enabled.
```
Intel XeSS Frame Generation fundamentally **does not support DXGI Exclusive Fullscreen**. When a game creates an exclusive fullscreen swapchain, Intel XeSS-FG unconditionally sets `isFrameGenEnabled = 0` and presents only native frames.

In OptiScaler, `Config::Instance()->FGXeFGForceBorderless` defaulted to `false`. While `dllmain.cpp` handled `GameQuirk::ForceBorderlessWhenUsingXeFG`, *Helldivers 2* was not included in `Quirks.h`. Consequently, the swapchain was created in exclusive fullscreen, causing Intel XeSS-FG to disable interpolation.

### 2.2 Destructive Deactivations on Multiplier Changes & Initialization
In `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`:
```cpp
if (State::Instance().activeFgInput != FGInput::DLSSG &&
    _framesToInterpolate != Config::Instance()->FGXeFGInterpolationCount.value_or_default())
{
    LOG_INFO("Interpolation count changed {} -> {}", _framesToInterpolate,
             Config::Instance()->FGXeFGInterpolationCount.value_or_default());

    state.WAR_xefgRequestFGToggle = true;
    ...
}
if (State::Instance().activeFgInput != FGInput::DLSSG && state.WAR_xefgRequestFGToggle)
{
    state.WAR_xefgRequestFGToggle = false;
    state.fgChanged = true;
    UpdateTarget();
    Deactivate();
}
```
1. `_framesToInterpolate` was initialized to `0` upon swapchain creation, causing `GetInterpolatedFrameCount()` to report 2X instead of the configured count until first dispatch, where it immediately triggered `WAR_xefgRequestFGToggle`.
2. On every multiplier change, `WAR_xefgRequestFGToggle` called `Deactivate()` (`XeFGProxy::SetEnabled(..., false)`) and paused presentation for 10 frames (`UpdateTarget()`).
3. In `OptiScaler/menu/menu_common.cpp`, dragging the `Generated Frames##xemfg_sr` slider also set `state.fgChanged = true;`, forcing `XeFG_Dx12::EvaluateState` to call `Deactivate()` on the subsequent frame.
4. During these deactivations, `FGHooks::FGPresent` logged:
   ```text
   [T] FGHooks::FGPresent: FG feature exists but is inactive/paused; pass-through present only
   ```
   causing severe framerate collapse and freezing telemetry on `FG Disabled`.

### 2.3 Erratic `frameRenderTime` & Motion Vector Scaling Collapse
Telemetry inspection revealed extreme fluctuations in `frameRenderTime` fed to Intel:
```text
[D] XeFG_Dx12::Dispatch Reset: 0, Opti FT: 0.7258, Source FT: 40.8548, Set FT: 0.7258
XeFG pacing: 6X, real frame 44.21 ms (22.6 fps), target 7.37 ms/frame; deadlines 11878 calls, 4559 rebased, 4525 clamped; render-est 0.00 ms, fed 124.43 ms
```
In `XeFGPacing.h`:
```cpp
const int64_t blockNs = NsFromQpc(g_burstBlockQpc);
g_renderTimeNs = g_periodNs > blockNs ? g_periodNs - blockNs : 0;
```
`blockNs` accumulated the time Intel's scheduler blocked presenting generated frames. When `blockNs` approached or exceeded `g_periodNs`, `g_renderTimeNs` collapsed to `0.7 ms` or `0.0 ms`.

In `XeFG_Dx12::Dispatch`, because `0.7258 > 0.0`, it fed `0.7258 ms` to Intel's `constData.frameRenderTime` while the true frame render time was `40.85 ms`. Intel XeSS-FG uses `frameRenderTime` to scale motion vectors and compute optical flow interpolation. When told the frame took only `0.7 ms`, motion vectors were scaled down by ~60x, causing massive visual judder, motion blurring, and erratic frame clumping.

### 2.4 OptiFG UI Overlap During XeMFG-SR
In `OptiScaler/menu/menu_common.cpp`, `Frame Generation (OptiFG)` was rendered without checking `xeMfgSrActive`. Users were presented with interactive controls (`HUDFix`, `Limit`, `Scale Depth`, `Flip`, etc.) and the `Advanced OptiFG Settings` collapsing header, confusing users and risking configuration conflicts with XeMFG-SR's automated upscaler pipeline.

---

## 3. Technical Precautions & Applied Solutions

### 3.1 Helldivers 2 Game Quirk & User Borderless Toggle (`Quirks.h`, `Config.cpp`, `OptiScaler.ini`)
Instead of globally forcing borderless windowing across all games:
1. **Game Quirk**: Added a dedicated quirk in `OptiScaler/misc/Quirks.h`:
   ```cpp
   // Helldivers 2
   // ForceBorderless required for XeFG / XeMFG-SR interpolation (Intel XeSS-FG disables interpolation in exclusive fullscreen)
   QUIRK_ENTRY("helldivers2.exe", GameQuirk::ForceBorderlessWhenUsingXeFG),
   ```
   Ensures *Helldivers 2* automatically creates borderless swapchains under Linux and Windows without affecting other titles.
2. **Configuration Support**: Added `ForceBorderless = auto` under `[XeMFG-SR]` in `OptiScaler.ini` and `Config.cpp`, allowing users to enable borderless windowing on any game at will.
3. **In-Game Menu Toggle**: Added a `"Force Borderless Window (Exclusive Fullscreen Workaround; restart)##xemfg_sr"` checkbox in the XeMFG-SR menu subsection.

### 3.2 Seamless Live Multiplier Adaptation (`XeFG_Dx12.cpp`, `menu_common.cpp`)
1. **Initial Synchronization**: In `XeFG_Dx12::CreateSwapchain` and `CreateSwapchain1`:
   ```cpp
   else
   {
       _framesToInterpolate = Config::Instance()->FGXeFGInterpolationCount.value_or_default();
   }
   ```
   `GetInterpolatedFrameCount()` reports the true multiplier immediately on swapchain creation.
2. **Bypass Destructive Deactivations**: In `XeFG_Dx12::Dispatch`:
   ```cpp
   if (!XeMfgLoader::AnyEnabledForSession())
       state.WAR_xefgRequestFGToggle = true;
   ```
   Eliminates `Deactivate()` and 10-frame pauses when multiplier changes.
3. **Menu Slider Decoupling**: Removed `state.fgChanged = true;` from the `Generated Frames##xemfg_sr` slider callback in `menu_common.cpp`. `SetNumInterpolatedFrames` dynamically adjusts Intel XeSS-FG live during gameplay without pausing presentation.

### 3.3 FrameRenderTime Sanity Floor Fallback (`XeFG_Dx12.cpp`)
In `XeFG_Dx12::Dispatch`:
```cpp
auto frameRenderTime = XeFGPacing::RenderTimeMs();

if (frameRenderTime < 1.0 || (_ftDelta[fIndex] > 0.0 && frameRenderTime < 0.25 * _ftDelta[fIndex]))
    frameRenderTime = _ftDelta[fIndex];

if (!(frameRenderTime > 0.0))
    frameRenderTime = _ftDelta[fIndex];

if (!(frameRenderTime > 0.0))
    frameRenderTime = state.lastFGFrameTime;
```
If `RenderTimeMs()` collapses below 1.0 ms or below 25% of the real measured frame delta `_ftDelta[fIndex]`, OptiScaler smoothly falls back to `_ftDelta[fIndex]`. Intel XeSS-FG is guaranteed to receive realistic frame deltas, stabilizing optical flow extrapolation and eliminating micro-stutters.

### 3.4 OptiFG Menu Auto-Management & Advanced Settings Hide (`menu_common.cpp`)
In `OptiScaler/menu/menu_common.cpp`:
```cpp
// OptiFG
if (state.api != API::Vulkan && state.currentFGSwapchain != nullptr && state.activeFgInput == FGInput::Upscaler)
{
    SeparatorWithHelpMarker("Frame Generation (OptiFG)", "Using upscaler data for FG");

    if (xeMfgSrActive)
    {
        ImGui::TextColored(
            toneMapColor(ImVec4(0.4f, 0.8f, 1.0f, 1.0f)),
            "Managed exclusively by the Intel Xe Multi-Frame Generation for DLSS-SR (XeMFG-SR) section above.");
    }
    else if (...)
    {
        // Manual parameter checkboxes and Advanced OptiFG Settings
    }
}
```
Displays the identical informational message as `Frame Generation (XeFG)` and completely hides manual parameters and `Advanced OptiFG Settings`.

---

## 4. Automated Unit Verification

Extended [tests/xemfg_sr_unit.cpp](file:///home/angelo/Documenti/git-repos/OptiScaler-DLSSNR-PreSR-Multipass/tests/xemfg_sr_unit.cpp) with Tests 8 through 11:
- **Test 8 (Helldivers 2 Quirk & Borderless Sync)**: Validates that `helldivers2.exe` activates `ForceBorderlessWhenUsingXeFG`, and that `[XeMFG-SR] ForceBorderless` overrides sync with `FGXeFGForceBorderless`.
- **Test 9 (Live Multiplier Adaptation Without Toggle Deactivations)**: Validates initial `_framesToInterpolate` synchronization in `CreateSwapchain1` and verifies that live multiplier changes do not set `WAR_xefgRequestFGToggle`.
- **Test 10 (FrameRenderTime Sanity Floor Fallback)**: Validates that collapsed estimates (0.7 ms, 0.0 ms, or < 25% of real delta) fallback cleanly to `_ftDelta`, while healthy pacing estimates are preserved.
- **Test 11 (OptiFG Menu Auto-Management & Advanced Settings Hide)**: Validates that `xeMfgSrActive == true` displays the informational text and suppresses manual controls and `Advanced OptiFG Settings`.

```text
[+] Starting XeMFG-SR Comprehensive Unit Tests...
  [PASS] Test 1: Config parsing, defaults, and 1..5 range clamping verified.
  [PASS] Test 2: Legacy fallback key migration verified.
  [PASS] Test 3: Universal mutual exclusion backend verified.
  [PASS] Test 4: Loader session state evaluation verified.
  [PASS] Test 5: Dynamic target ceiling resolution verified.
  [PASS] Test 6: Live in-memory ceiling adaptation (U3/U4/U5 rewriting) verified.
  [PASS] Test 7: Post-attach automatic pipeline configuration verified.
  [PASS] Test 8: Helldivers 2 quirk resolution and borderless sync verified.
  [PASS] Test 9: Live multiplier adaptation without toggle deactivations verified.
  [PASS] Test 10: FrameRenderTime sanity floor fallback logic verified.
  [PASS] Test 11: OptiFG menu auto-management and advanced settings hide verified.
[+] All XeMFG-SR unit tests PASSED successfully!
```
