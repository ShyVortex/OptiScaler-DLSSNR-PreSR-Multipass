# DLSS-G: Where Winds Meet Multiplier Menu Hiding & Shortcut Debounce Safety

## 1. Overview & Environment

### Verified Test Environment
- **Operating System**: CachyOS Linux (Kernel `7.2.9-2-cachyos`, x86_64)
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (16 GB VRAM, GA103M)
- **Driver**: NVIDIA Linux Display Driver `580.178.04` (CUDA 13.0)
- **Build Target**: Release x64 with C++20 standard

### Reporter Environment (Issue #50)
- **Operating System**: Windows 11 build 26100.9278
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Desktop (12 GB VRAM, GA102, Device ID `0x2208`)
- **Driver**: NVIDIA Display Driver `616.92`
- **Game**: Where Winds Meet (`wwm.exe`), NetEase Games, DirectX 12
- **Integration**: Streamline 2.14.0 with DLSS-G (`nvngx_dlssg.dll` v310.9.1) & DLSS-NR (`nvngx_dlssnr.dll` v310.8.0)
- **Mod Configuration**: `dlss-unlocked-standalone-NR-v0.9.51`, `External = true`, `AmpereMfgUnlock = true`, `Dx12Upscaler = dlss`
- **Report Reference**: [ShyVortex/dlss-unlocked#50](https://github.com/ShyVortex/dlss-unlocked/issues/50)

---

## 2. Reported Issue & Symptoms

A user reported that when running *Where Winds Meet* with OptiScaler and the Ampere SM86 Multi-Frame Generation unlocker enabled:
1. On a fresh installation via `Setup.exe`, Frame Generation works properly and displays the in-game multiplier menu (allowing the user to switch between 2X and 3X).
2. After closing the game or restarting the PC, subsequent game launches no longer display the multiplier menu. The game settings display "Frame Generation: On", but the sub-options menu to select 2X or 3X disappears completely.
3. Attempting to rapidly toggle Neural Rendering (DLSS-NR) via hotkey/overlay to "fix" the issue crashes the game.
4. The user found that reinstalling OptiScaler via `Setup.exe` repeatedly was the only way to get the multiplier menu back.

---

## 3. Issue Validity Assessment

### Issue 1: Frame Generation Multiplier Submenu Disappearing
- **Validity**: **Intended game engine behavior** triggered by user configuration misunderstanding.
- **Root Cause Analysis**:
  - The user's uploaded `OptiScaler.ini` contained `AmpereMfgMaxFrames = 1`.
  - In OptiScaler, `AmpereMfgMaxFrames` represents the **maximum multiplier capability ceiling advertised to Streamline and the game engine via NVAPI DRS overrides (`0x104D6667`) and `dlssg_sm86.ini`**.
  - A value of `1` corresponds to 1 extra generated frame, which equates to **standard 2X Frame Generation**.
  - On a fresh install, `AmpereMfgMaxFrames` defaults to `auto` (which resolves to `3` / 4X capability). When the game queries Streamline / DLSS-G, it detects Multi-Frame Generation support (up to 4X) and displays the multiplier selection dropdown (`2X`, `3X`).
  - When the user adjusted the slider in the overlay to `1 (2X)` and saved settings, the advertised capability ceiling was locked to 1.
  - In modern games utilizing Streamline 2.8+ / 2.14+ (*Where Winds Meet*, *Black Myth: Wukong*), if the advertised capability ceiling is $\le 1$, the game engine recognizes that Multi-Frame Generation is not supported on this hardware. Because only standard 2X FG is physically available, the game **intentionally hides the 2x/3x multiplier submenu** and displays only "Frame Generation: On".
  - Frame Generation was actively running at 2X, but the user mistook the missing dropdown for a breakdown of the mod.
  - Reinstalling with `Setup.exe` overwrote `OptiScaler.ini` with the stock configuration (`AmpereMfgMaxFrames = auto` $\rightarrow$ 3), which temporarily restored the 4X capability advertisement until the user saved settings again.

### Issue 2: Crash on Rapid Neural Rendering (DLSS-NR) Toggling
- **Validity**: **Genuine instability under rapid hotkey spam**.
- **Root Cause Analysis**:
  - DLSS-NR model inference takes **~27 ms of GPU compute time per frame** on an RTX 3080 Ti at 1969×1107.
  - DLSS-NR and Frame Generation are completely independent subsystems in `External = true` mode; toggling NR has zero effect on the Frame Generation menu.
  - When the user spammed the NR toggle hotkey across frames:
    1. `IFeature_Dx12` alternated every single frame between dispatching `PrepareDlssNrInput` (using `shader.Buffer()` as `NVSDK_NGX_Parameter_Color`) and bypassing it (using `originalColor`).
    2. Switching the route on consecutive frames forced `resetGameHistory = true` on every frame, repeatedly zeroing camera jitter offsets and forcing driver-level NGX history resets.
    3. Mashing the key while the GPU was already saturated under a concurrent 27 ms tensor kernel pass and DLSS-G frame interpolation caused command list synchronization stalls and driver TDRs (Device Removed).
  - Furthermore, `CheckShortcut` in `OptiScaler/menu/menu_common.cpp` previously failed to update `lastInputTick = currentTick` when a shortcut key was released, leaving shortcut hotkeys with zero debounce protection against rapid key chatter.

---

## 4. Applied Technical Solutions & UX Improvements

### 1. SM86 MFG Multiplier Ceiling Slider Clarity
In `OptiScaler/menu/menu_common.cpp`:
- Renamed the slider from `"Max Generated Frames##sm86"` to `"Max Multiplier Ceiling##sm86"`.
- Updated label array to make capability boundaries explicit:
  `{ "Factory default (up to 4X)", "1 (up to 2X only)", "2 (up to 3X)", "3 (up to 4X)", "4 (up to 5X)", "5 (up to 6X)" }`.
- Added an inline dynamic warning banner when `maxFrames == 1`:
  ```cpp
  if (!dmfgActive && maxFrames == 1)
  {
      ImGui::TextColored(
          ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
          "Notice: Ceiling set to 2X. In-game 2x/3x multiplier menus will be hidden by the game engine.");
  }
  ```
- Expanded the help marker / tooltip explaining:
  - This setting advertises capability to the game engine, whereas the game's own graphics menu chooses the active multiplier.
  - Setting this to `1` causes titles like *Where Winds Meet* and *Black Myth: Wukong* to hide their in-game 2x/3x multiplier menus.
  - Users should keep this at `Factory default (up to 4X)` or higher.

### 2. OptiScaler.ini Documentation
In `OptiScaler.ini`:
- Updated comment for `AmpereMfgMaxFrames`:
  ```ini
  ; Max multiplier ceiling advertised to the game: 0=runtime default (3=4X factory default on 0.3.1-0.3.5, up to 5=6X on 310.9), 1=2X, 2=3X, 3=4X, 4=5X, 5=6X.
  ; Note: Setting this to 1 (up to 2X only) informs the game engine that Multi-Frame Generation (3X+) is unsupported, causing titles
  ; with in-engine multiplier selectors (e.g. Where Winds Meet, Black Myth: Wukong) to hide their 2x/3x selection menus.
  AmpereMfgMaxFrames=3
  ```

### 3. Shortcut Key Debounce & Hotkey Safety Enforcement
In `OptiScaler/menu/menu_common.cpp`:
- Updated `CheckShortcut` so that when a key release event triggers an input flag, `lastInputTick = currentTick;` is immediately recorded.
- Set `debounceThreshold = 500;` (500 ms cooldown window).
- Ensured any subsequent key release events within 500 ms are ignored, preventing key chatter or rapid spamming from destabilizing in-flight D3D12 command lists.

---

## 5. Automated Verification

The fixes are validated through dedicated automated unit tests:

1. **`tests/ampere_mfg_ceiling_ui_unit.cpp`**:
   - Verifies label resolution across all slider indices (0 through 5).
   - Asserts that `maxFrames == 1` correctly activates the warning notice banner.
   - Asserts that Dynamic MFG overrides labels to `"Dynamic (Up to 6X)"` and suppresses the warning banner.
   - Asserts that out-of-bounds indices cleanly fall back to default.
2. **`tests/shortcut_debounce_unit.cpp`**:
   - Verifies that initial shortcut trigger updates `lastInputTick`.
   - Verifies that subsequent presses within 50 ms and 499 ms are debounced.
   - Verifies that presses at 501 ms are accepted.
   - Verifies cross-shortcut debounce protection (e.g. Menu key pressed right after NR toggle).
   - Verifies capturing key mode updates `lastInputTick`.

---

## 6. Preservation Mandate

1. The distinction between **capability ceiling** (`AmpereMfgMaxFrames`) and **active multiplier** must always be preserved.
2. Capability ceilings must never default to 1, as doing so strips in-engine Multi-Frame Generation menus in titles supporting Streamline DLSS-G.
3. Shortcut hotkeys must remain guarded by `debounceThreshold` in `MenuCommon::UpdateManualInput`.
