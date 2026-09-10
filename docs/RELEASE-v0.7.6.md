## OptiScaler DLSS-NR v0.7.6

- Fix optional exposure/reactive-resource handoff in the DX11/Vulkan bridges, harden NGX feature identification and release handling, and restore RR placement controls when the separate SR-only deferred option is selected (#17).
- Add opt-in RTX 20/30 MFG integration, with GPU detection and runtime loading outside DllMain, synchronized status, and corrected kernel selection (#23). The optional external runtime is not bundled in the standard ZIP; the integration remains disabled by default.
- Add experimental **Carry the pre-SR edit across RR** (#26). It leaves RR's colour input intact and applies an accumulated NR residual afterward, with guarded resource states, one-shot frame pairing, signed resampling and clean failure handling. It is off by default and needs scene-specific testing; it does not implement depth-based disocclusion rejection.
- Simplify the hybrid display to **Hybrid: active** or **Hybrid: inactive**, with **Loading may pause the game and look like a freeze. Please wait.** Detailed failure messages remain in diagnostics. Include the verified precompiled hybrid assets in automated release packages (#28).
- Identify the binary as **0.7.6-final**, with Windows file version **0.7.6.0** and development/prerelease flags disabled.

### Install or upgrade

Download and extract the complete `OptiScaler-DLSSNR-v0.7.6.zip`, keeping the `OptiScaler` subfolder beside the proxy DLL. Back up your game-specific INI before upgrading. NVIDIA FP8 remains the default; all 15 hybrid assets are included, and no CUDA installation or kernel compilation is required. Hybrid loading can temporarily pause the game.

Supply your GPU-appropriate `nvngx_dlssnr.dll` separately using the included installation guide. NVIDIA NR/FG runtimes and the optional RTX 20/30 MFG proxy are not bundled. The verified Streamline downloader remains included.

### Validation and remaining reports

x64 Release builds and targeted NGX routing, deferred seam, MFG kernel-selection and residual WARP tests passed during review. The release archive is checked for its binary version, file checksums, complete hybrid assets and portable defaults before publication. No fresh game validation is claimed, and repository-wide formatting CI has pre-existing failures.

Before-RR compatibility remains experimental. This release does not claim to resolve the vkd3d epoch-0 stall (#24), Control's NR/OptiFG hang (#18), or every reported toggle crash/flicker/colour artifact. Those reports remain open for targeted reproduction evidence.
