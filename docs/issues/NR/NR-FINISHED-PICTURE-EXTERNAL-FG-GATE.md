# Neural Rendering: Finished-Picture NR Silently Skipped Under External Frame Generation

## Overview & Environment
- **Test system** (from the user's `OptiScaler.log`): Windows 11 (10.0.26300), NVIDIA GeForce RTX 3080 Ti Laptop GPU
  (Ampere SM86), NGX DLSS / DLSS-NR runtime 310.9.16, sdli1995 SM86 MFG mod v0.4.0 loaded through AmpereMfgLoader.
- **Game**: The Last of Us Part II (`tlou-ii.exe`), DX12, DLSS input, linear-HDR DLSS colour, SDR swapchain.
- **Build**: OptiScaler v0.9.74-pre2 (`a0476da`), branch `merge-upstream`.
- **Development system** (verified with `uname -r`, `/etc/os-release`, `nvidia-smi`): CachyOS Linux, kernel
  `7.3.0-4-cachyos`, RTX 3080 Ti Laptop GPU, driver 580.178.04.

## Reported Issue
"Apply NR to the finished picture doesn't seem to work": the picture shows no NR edit while the menu reports
"Running".

## Validity Assessment
The skip is intentional. Commit `674da5df` stops finished-picture NR whenever external frame generation or DLSS-G is
active, because editing the presented image races the generated backbuffers. The gates now live in
`DlssNr_Dx12.cpp` (`ApplyToFinishedPicture`, `ApplyFinished`, `ApplyToFinishedPictureDx11`),
`DlssNr_Dx12_FinishedCompose.cpp` (`ApplyFinishedColor`), and `wrapped_swapchain.cpp`, which computes the same
condition. Upstream has no such gate.

The log confirms the gate was hit: `FrameGen.External: true`, the deferred path logs
`changes saved for the finished picture`, and the late context stays at
`Waiting for the previous picture to finish.` Because "finished picture" routes the edit to presentation, nothing
reaches the screen.

The real defect is that the skip is silent. The option could be enabled, and the status line still said "Running".

## Solution Applied
- `dlssnr/DlssNr_Placement.h`: `ResolveFinishedPictureGate(finished, externalFrameGeneration, dlssgOutput)` returns:
  - `blocked`: same condition as the runtime.
  - `canToggle`: never on while blocked, always off.
  - `notApplied`: the option is on but blocked.
- `dlssnr/DlssNr_Menu.cpp`:
  - The checkbox is disabled while blocked unless it is already on, so it can still be turned off.
  - An amber warning explains why it is disabled; a second line says no NR edit reaches the screen while it stays
    on. The tooltip carries the same explanation when hovered while disabled.
  - The Running status line ends with "(not applied: finished picture blocked by frame generation)".
- Runtime behaviour is unchanged: the 674da5df gates stay in place.

## Verification
- `tests/nr_finished_picture_gate_unit.cpp` (6/6 PASS) covers:
  - the gate truth table;
  - that the menu uses it for the checkbox, warning, tooltip and status line;
  - that every runtime gate site is still present.
- All existing portable NR / MFG unit tests pass; clang-format 23.1.3 / 23.1.4 clean; UTF-8 BOM present.
- Pending: in-game check that the warning appears with External FG on and that the option can be turned off.
