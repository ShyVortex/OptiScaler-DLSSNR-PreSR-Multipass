# Intel XeFG / XeMFG Resident Evil Requiem Presentation Pacing & Quirks Resolution

## 1. Overview & Context

- **Operating System**: Windows 11 x64 (build 10.0.26300).
- **Target Feature**: Intel XeSS Frame Generation (`libxess_fg.dll`) with OptiScaler XeMFG Multi-Frame Generation Unlocker.
- **Target Application**: *Resident Evil Requiem* (`re9.exe` / Capcom RE Engine v1.3.1.0) utilizing NVIDIA Streamline (`sl.interposer.dll`).
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86, architecture `0x170`).
- **Analyzed Log File**: `OptiScaler_RE-test12.log` (623,330 lines).
- **Target Branch**: `intel-xemfg`.

---

## 2. Reported Symptoms

Following the resolution of the motion vector slot-split crash in *The Last of Us Part II* (which confirmed complete stability with zero crashes), testing in *Resident Evil Requiem* showed persistent pacing issues:
1. **Severe Visual Micro-Stutter / Judder in Active Gameplay**:
   - Despite displaying high total framerate, camera motion and panning felt visibly jittery, with constant micro-stuttering across gameplay.
2. **Pause Menu / Inventory Presentation Hiccups**:
   - Entering pause screens or inventory menus triggered driver warnings in `PostPresent`:
     ```
     [01:08:17.617432] [W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=-12 (XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES), framesPresented=1, enabled=1
     [01:08:17.688670] [W] XeFG_Dx12::PostPresent XeFG LastPresentStatus WARNING: result=-14 (XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES), framesPresented=1, enabled=1
     ```
3. **Requirement of Launch Safety**:
   - Crucially, any pacing or lifecycle adjustments for RE9 must guarantee that the game does not crash on launch or during resolution switches.

---

## 3. Issue Validity Assessment & Root Cause Analysis

Analysis of `OptiScaler_RE-test12.log` established two distinct architectural defects in the presentation pipeline:

### 3.1 Defect 1: Unmetered 4X Frame Clumping & Scanout Overwrites (`FG_Hooks.cpp`)

In active gameplay with 4X Frame Generation (multiplier 4: 3 interpolated frames + 1 base frame per render cycle):
- Base game render frametime: **~36.5 ms** (27.4 base FPS).
- Analysis of `LocalPresent` timestamps revealed the exact interval between presentations:
  ```
  [01:08:26.502534] Frame 15065 (interpolated):   7.7 ms
  [01:08:26.505220] Frame 15066 (interpolated):   2.7 ms
  [01:08:26.508379] Frame 15067 (interpolated):   3.2 ms
  [01:08:26.532046] Frame 15068 (base frame):    23.7 ms
  Total Cycle:                                  37.3 ms
  ```
  ```
  [01:08:26.538503] Frame 15069 (interpolated):   6.4 ms
  [01:08:26.540585] Frame 15070 (interpolated):   2.1 ms
  [01:08:26.543478] Frame 15071 (interpolated):   2.9 ms
  [01:08:26.568595] Frame 15072 (base frame):    25.1 ms
  Total Cycle:                                  36.5 ms
  ```

#### The Failure Mechanism:
1. Because V-Sync was OFF in the in-game display settings, RE9 invoked `Present(0, DXGI_PRESENT_ALLOW_TEARING)` (`SyncInterval = 0`, `Flags = 0x200`).
2. On native Windows DXGI, unmetered presentation with tearing allows Intel's presentation worker thread to dump all 3 generated frames to the swapchain as fast as they finish execution—within an **11 ms burst** (instantaneous framerate of 300–500 FPS).
3. The swapchain then stalled for **25 ms** waiting for the game's simulation thread to finish rendering the next base frame.
4. Because the intermediate frames arrived in rapid succession without waiting for the physical display refresh interval, the monitor's scanout raster either tore or overwrote them before they could be perceived by the human eye. The user only perceived the 25ms stall on every single cycle, creating persistent judder.

### 3.2 Defect 2: Passthrough Half-Tagging Inconsistency During Pause/Menus (`XeFG_Dx12.cpp`)

When entering pause screens, Streamline sets `DLSSGMode::eOff`, calling `SetInterpolatedFrameCount(0)`.
1. OptiScaler placed the pipeline into warm passthrough mode (`_passthrough = true`, `_framesToInterpolate = 0`).
2. However, Streamline continued calling `SetResource(Depth)` and `SetResource(Velocity)`.
3. `XeFG_Dx12::SetResource()` forwarded `XeFGProxy::D3D12TagFrameResource()` calls to Intel's swapchain driver for these guides.
4. Concurrently, `XeFG_Dx12::Present()` returned early at `if (_passthrough) return true;`, bypassing `TagFrameConstants()`, `SetPresentId()`, and `D3D12TagFrameResource(Backbuffer)`.
5. Because Intel's swapchain driver was still active, it received partial guide resources without matching Constants or Backbuffer, returning:
   - `-12 (XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES)`
   - `-14 (XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES)`
   on every frame until gameplay resumed.

### 3.3 Defect 3: Exclusive Fullscreen Risk in RE Engine Titles

As documented in the OptiScaler documentation and wiki:
- Intel XeFG is strictly incompatible with Exclusive Fullscreen on Windows DXGI.
- Previous RE9 quirk entries lacked `ForceBorderlessWhenUsingXeFG`. If RE9 attempted exclusive fullscreen mode switching, the swapchain presentation broke.

---

## 4. Technical Solutions Applied

### 4.1 Game Quirks for RE9 (`OptiScaler/misc/Quirks.h`)

Updated `re9.exe` and `re9demo.exe` quirk table entries in `Quirks.h`:
```cpp
QUIRK_ENTRY("re9.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::AllowedFrameAhead2,
            GameQuirk::ForceBorderlessWhenUsingXeFG, GameQuirk::OverrideVsyncWhenUsingXeFG),
QUIRK_ENTRY("re9demo.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::AllowedFrameAhead2,
            GameQuirk::ForceBorderlessWhenUsingXeFG, GameQuirk::OverrideVsyncWhenUsingXeFG),
```
1. **`ForceBorderlessWhenUsingXeFG`**: Ensures the game swapchain remains in Borderless Windowed mode when XeFG is active, preventing Exclusive Fullscreen incompatibilities.
2. **`OverrideVsyncWhenUsingXeFG`**: Automatically enables V-Sync override when XeFG is active.
3. **Swapchain Preservation**: Maintained swapchain context preservation (`DoNotPreserveFGSwapChain` remains absent), ensuring zero launch crashes during initial resolution switches (1080p intro to 1440p menu).

### 4.2 Presentation Synchronization to VBlank (`OptiScaler/hooks/FG_Hooks.cpp`)

In `FG_Hooks.cpp`:
```cpp
else if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::XeFG &&
         (config->XeMfgExtraPacing.value_or(false) || config->OverrideVsync.value_or_default()) &&
         !IdentifyGpu::getPrimaryGpu().usesDxvk)
{
    if (SyncInterval < 1)
        SyncInterval = 1;

    Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
    LOG_DEBUG("XeMFG ExtraPacing applied: SyncInterval={}, Flags={:X}", SyncInterval, Flags);
}
```
When `OverrideVsync` or `XeMfgExtraPacing` is active on native Windows DXGI:
- `SyncInterval` is locked to `1` (or higher if user configured `VsyncInterval`).
- `DXGI_PRESENT_ALLOW_TEARING` is explicitly stripped.
- The GPU and display driver present each individual generated frame synchronized to vertical blank, spacing frames evenly across the physical refresh rate (e.g. 120Hz, 144Hz) and eliminating the 11ms burst clumping.

### 4.3 Passthrough Resource Tagging Sanitization (`OptiScaler/framegen/xefg/XeFG_Dx12.cpp`)

In `XeFG_Dx12::SetResource()`:
```cpp
if (type != FG_ResourceType::UIColor ||
    (XeFGProxy::SetUiCompositionState() != nullptr || Config::Instance()->FGDrawUIOverFG.value_or_default()))
{
    if (!_passthrough)
    {
        auto frameId = static_cast<uint32_t>(_frameCount - indexDiff);
        auto result =
            XeFGProxy::D3D12TagFrameResource()(_swapChainContext, fResource->cmdList, frameId, &resourceParam);
        LOG_DEBUG("D3D12TagFrameResource, frameId: {}, type: {} result: {} ({})", frameId,
                  magic_enum::enum_name(type), magic_enum::enum_name(result), (int32_t) result);

        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            State::Instance().fgChanged = true;
            UpdateTarget();
            Deactivate();

            return false;
        }
    }
    else
    {
        LOG_DEBUG("XeFG in passthrough mode, skipping D3D12TagFrameResource for type: {}",
                  magic_enum::enum_name(type));
    }
}
```
When `_passthrough` is `true`:
- Resources are cached locally in `_frameResources[fIndex]` and flagged ready in OptiScaler.
- Driver-level `D3D12TagFrameResource` calls are bypassed, preventing partial resource submission and eliminating `-12` and `-14` errors.
- Upon unpausing, `_passthrough` resets to `false` and subsequent complete frames are tagged normally.

---

## 5. Verification & Unit Tests

### 5.1 Automated Unit Tests

1. **`tests/xefg_swapchain_lifecycle_unit.cpp`**:
   - **Test 1**: Asserts `re9.exe` and `re9demo.exe` entries configure `AllowedFrameAhead2`, `ForceBorderlessWhenUsingXeFG`, and `OverrideVsyncWhenUsingXeFG` while preserving swapchain context.
   - **Test 2–4**: Verifies resolution transitions cleanly resize without swapchain destruction, and verifies safe fallback recreation.
2. **`tests/xefg_ring_buffer_pacing_unit.cpp`**:
   - **Test 8**: Verifies that while `_passthrough == true`, `SetResource` caches resources locally but submits zero driver tags (`_driverTagCount` unchanged). Upon unpausing (`_passthrough == false`), driver tags are submitted normally with zero slot-split.

### 5.2 Test Suite Execution Results

```bash
All XeFG Swapchain Lifecycle Unit Tests PASSED! (4/4 tests)
All XeFG Ring Buffer & Motion Vector Pacing Unit Tests PASSED! (8/8 tests)
All XeFG Save/Load Stability & Presentation Pacing Unit Tests PASSED! (5/5 tests)
All RTSS + XeFG Pacing & Marker Immunity Unit Tests PASSED! (3/3 tests)
All XeMfgLoader unit tests PASSED successfully! (8/8 tests)
All XeFG MFG & Streamline unit tests PASSED successfully! (7/7 tests)
All XeMFG Menu UI Conflict Suppression tests PASSED successfully! (3/3 tests)
```

All modified source files strictly comply with `.clang-format` and preserve UTF-8 BOM (`\xef\xbb\xbf`).
