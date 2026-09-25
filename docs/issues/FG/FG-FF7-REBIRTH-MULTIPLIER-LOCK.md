# DLSS-G: FF7 Rebirth In-Game Multiplier Fixed at 4X (SilyNoMeta FollowGame Integration)

## 1. Overview & Environment

- **Operating System**: Windows 11 build 26200
- **Hardware**: NVIDIA GeForce RTX 3080 Ti (Ampere SM86, `0x170`)
- **Driver**: NVIDIA Display Driver 616.92
- **Game**: FINAL FANTASY VII REBIRTH (Steam), version 1.0.0.5
- **Mod Configuration**: OptiScaler v0.9.26 with SilyNoMeta SM86 MFG v0.3.5-4 (Variant 2)
- **Report Reference**: [Gist 485a7e37a7da94614daecf886fb04d44](https://gist.github.com/AreChen/485a7e37a7da94614daecf886fb04d44)

---

## 2. Reported Issue & Symptoms

A user reported that when running *FINAL FANTASY VII REBIRTH* with the RTX 20/30 (SM86) Multi-Frame Generation unlocker enabled:
1. Frame generation initializes and functions properly.
2. However, changing the in-game frame generation multiplier selector (e.g. switching between 2X, 3X, and 4X in the game graphics menu) has no effect on generated frames.
3. The game output remains locked at 4X frame generation at all times.
4. `OptiScaler.log` confirmed `FrameGen.External=true`, `AmpereMfgUnlock=true`, SilyNoMeta v0.3.5-4 loaded successfully, and the SM86 backend operational.
5. In `dlssg_sm86.ini`, `MaxGeneratedFrames=3` (up to 4X ceiling) and `DynamicMFG=0` were written. In `RTXMFG/config.json`, `mode` was set to `driver` (which specifies game/profile control). Despite this, 4X remained permanently forced in-game.

---

## 3. Issue Validity Assessment

### Is this a user misconfiguration?
**No.** The user's configuration is completely valid and standard:
- `OptiScaler.ini` correctly set `AmpereMfgUnlock = true` and `AmpereMfgMaxFrames = 3`.
- In OptiScaler's architecture, `AmpereMfgMaxFrames` represents the **maximum capability ceiling** (i.e. up to 4X generation supported by the hardware and mod runtime) advertised to Streamline/DLSS-G.
- The user intentionally left `OverrideInterpolationCount` unset (defaulting to no forced override), and `RTXMFG/config.json` was generated in `mode = "driver"`. The user specifically intended for the in-game settings menu of *FINAL FANTASY VII REBIRTH* to dictate the active multiplier (2X vs 3X vs 4X).

### Defect Verification
1. In `OptiScaler/framegen/dlssg/AmpereMfgLoader.cpp`, when OptiScaler loaded SilyNoMeta's DLL and resolved `DLSSG_RequestControl`, it performed the following initialization:
   ```cpp
   const int configuredFrames = cfg->FGDLSSGAmpereMfgMaxFrames.value_or_default(); // = 3 (ceiling)
   const int multiplier = (configuredFrames >= 1 && configuredFrames <= 5) ? (configuredFrames + 1) : 0; // = 4
   uint32_t mode = dynamicMfg ? 1 : ((multiplier >= 2) ? 2 : 0); // Evaluates to 2 (Fixed Multiplier mode!)
   ```
2. Because `mode` was set to `2` (`Fixed Multiplier`) with `multiplier = 4`, OptiScaler immediately dispatched a `DLSSG_ControlRequest` with `mode = 2` to SilyNoMeta's backend.
3. Reverse-engineering of SilyNoMeta v0.3.5-4 (`version.dll`) confirms that when `mode == 2`, the mod's hook on `slDLSSGSetOptions` forcibly rewrites `options->numFramesToGenerate = (multiplier - 1)` (3 generated frames = 4X) on every single frame, discarding whatever frame count the game requested!
4. Furthermore, `MergeReshadeCompanionContent` in `OptiScaler/framegen/dlssg/AmpereMfgLoader.h` computed `Multiplier = (maxFrames + 1) = 4` and wrote `Multiplier=4` to `ReShade.ini` instead of `Multiplier=0` (`0 = FollowGame / Driver`).
5. In `OptiScaler/menu/menu_common.cpp`, whenever Dynamic MFG was toggled off, it also fell back to `liveMode = 2` with `multiplier = 4`.

This confirmed an active defect: OptiScaler was conflating the **capability ceiling** (`AmpereMfgMaxFrames`) with a **forced multiplier override** (`OverrideInterpolationCount`).

---

## 4. Technical Solution & Architectural Invariants

### 1. Unified Control Mode Resolution
A helper function `ResolveControlModeAndMultiplier` was introduced in `OptiScaler/framegen/dlssg/AmpereMfgLoader.h`:
```cpp
inline void ResolveControlModeAndMultiplier(bool dynamicMfg, int explicitOverrideFrames, int maxCeiling,
                                            uint32_t& outMode, uint32_t& outMultiplier)
{
    if (dynamicMfg)
    {
        outMode = 1; // Dynamic MFG pacing
        outMultiplier = 0;
    }
    else if (explicitOverrideFrames > 0)
    {
        outMode = 2; // Fixed multiplier override explicitly requested by user
        int clamped = explicitOverrideFrames > maxCeiling ? maxCeiling : explicitOverrideFrames;
        outMultiplier = static_cast<uint32_t>(clamped + 1);
    }
    else
    {
        outMode = 0; // FollowGame / Driver mode (game controls the multiplier)
        outMultiplier = 0;
    }
}
```

### 2. FollowGame Multiplier in Companion Configuration
`MergeReshadeCompanionContent` was updated to accept `int fixedMultiplier = 0`:
- When `dynamicMfg == true` OR `fixedMultiplier == 0`: writes `Multiplier=0` (`FollowGame`).
- Only when `!dynamicMfg && fixedMultiplier >= 2 && fixedMultiplier <= 6`: writes `Multiplier=fixedMultiplier`.

### 3. Live Programmatic Synchronization
- In `AmpereMfgLoader.cpp`, post-load dispatch sends `mode = 0, multiplier = 0` whenever `OverrideInterpolationCount` is unset or 0.
- In `menu_common.cpp`, disabling Dynamic MFG restores `mode = 0` instead of forcing `mode = 2`.
- In `menu_common.cpp`, changing the "Override DLSSG Ratio" combo box immediately dispatches the updated multiplier to SilyNoMeta live via `ApplyLiveControl`.

---

## 5. Automated Verification

The solution is verified by automated unit tests:
- `tests/dynamic_mfg_live_control_unit.cpp`: Tests `ResolveControlModeAndMultiplier` across all combinations of `dynamicMfg` and `explicitOverrideFrames`, and verifies dispatch of `mode = 0, multiplier = 0`.
- `tests/dlssg_sm86_v030_ini_unit.cpp`: Verifies companion INI generation produces `Multiplier=0` by default and `Multiplier=4` only when an explicit 4X override is requested.

---

## 6. Preservation Mandate

Multi-Frame Generation unlockers must never treat capability limits (`AmpereMfgMaxFrames`) as forced overrides. When external frame generation is active without an explicit user override, `mode = 0` (`FollowGame`) and `Multiplier = 0` must always be maintained so games can dynamically adjust generation rates.
