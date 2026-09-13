# v0.8.0 with RTX 40 MFG

This branch retains the built-in Ada unlock as an optional build feature. Both builds include the Starfield tracking fix. The unlock is compiled into OptiScaler: no extra helper, ASI loader or external-FG mode is needed.

## Build

The default build excludes the unlock implementation, hooks, capability overrides, menu and configuration field. Ordinary DLSS FG/MFG remains available. Legacy `AdaMfgUnlock` settings are ignored and removed on save.

```powershell
# Without the unlock (default)
MSBuild OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /p:OptiScalerRtx40Mfg=false
./package_release.ps1 -Version nr-standard

# Compile the optional unlock
MSBuild OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /p:OptiScalerRtx40Mfg=true
./package_release.ps1 -Version nr-rtx40-mfg -EnableRtx40Mfg
```

Enabled builds use `x64/Release-RTX40-MFG`; standard builds use `x64/Release`. Separate intermediate folders prevent mixing objects/PCH files. Packaging checks the DLL flavour even with `-SkipBuild` and omits the unlock INI default for standard packages.

`OptiScalerRtx40Mfg=true` defines `OPTISCALER_RTX40_MFG`. The runtime toggle still defaults off. Keeping the restoration/build-flag commits separate lets upstream reviews omit their source changes too: a disabled build flag alone does not remove them from a PR diff.

## Enable at runtime

1. Install the complete **unlock-enabled** package, preserving your INI and separately supplied NR runtime.
2. Under frame-generation settings, enable **RTX 40 MFG unlock (restart)**, save and restart the game. Alternatively set `[DLSSG] AdaMfgUnlock=true` before launch.
3. Enable the game's DLSS FG or configure OptiScaler's normal DLSSG output. Start at 3x and check motion as well as the FPS counter.

The option defaults off and only patches RTX 40/Ada. It requires a supported NVIDIA DLSSG runtime; game multiplier overrides need Streamline 2.7.1+. Keep the game's working runtime. This package does not include NVIDIA FG/NR DLLs or another MFG unlocker.

The patch retargets compatible Blackwell interpolation kernels for Ada and changes two frame-count gates in memory. It exposes up to five generated frames (6x including the real frame) only when both gates and a kernel group match. Unknown/ambiguous signatures remain unchanged. Disabling also requires a restart; it does not undo a live patch.

RTX 20/30 unlocks, external-FG ownership, residual frame interpolation and NVFP4 remain absent. This does not add a missing FG integration to a game. Dynamic MFG and real RTX 40 motion quality remain unverified here; the available test GPU is RTX 5090.

## Validation and source

`tests/mfg_unlock/run.ps1` compiles the production patcher/scanner against controlled PE images. Cases cover both gate layouts, kernel retargeting, repeat calls, unsupported GPUs, missing/ambiguous gates, malformed kernels and restart semantics. `-Runtime <nvngx_dlssg.dll>` additionally patches an image mapped without DLL initialization, under simulated Ada identity; it checks that the disk file is unchanged. Neither test proves real RTX 40 interpolation works.

The branch changes are the patcher, DLSSG/Streamline/load hooks, the toggle and INI handling, project registrations, tests and package documentation. The [NR upstream inventory](NR-UPSTREAM-DIFF-INVENTORY.md) describes the v0.8.0 base.

Adapted from [y4my4my4m's work](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG/commit/7b7220bb) and the earlier fork's Ada kernel retargeting, under GPL-3.0. This is the built-in implementation, not Dashdogy's separate unlocker.
