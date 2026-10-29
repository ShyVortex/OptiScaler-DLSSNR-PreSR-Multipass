# Frame Generation: XeFG Rejects Display-Resolution Motion Vectors with Render-Resolution Depth

## 1. Overview & Environment
- **Report**: DLSS Unlocked issue #57 (https://github.com/ShyVortex/dlss-unlocked/issues/57), follow-up to the menu
  multiplier freeze (`FG-XEFG-MENU-MULTIPLIER-FREEZE.md`).
- **Reporter environment**:
  - Windows 11, NVIDIA GeForce RTX 2060 (Mobile), driver 617.42.
  - Silent Hill Townfall (Unreal Engine), DLSS Quality: 1279x720 render for a 1920x1080 output.
  - XeMFG `Source=OptiScaler` (`FGInput::Upscaler` → `FGOutput::XeFG`).
- **Analysis environment** (logs only): CachyOS rolling, kernel 7.3.0-4-cachyos, NVIDIA GeForce RTX 3080 Ti Laptop
  GPU, driver 580.178.04.

## 2. Reported Symptoms
1. XeFG did not generate frames at all.
2. The workaround `[InitFlags] DisplayResolution=false` got XeFG working, but caused strong shimmering in DLSS
   Quality; Native (DLAA) was mostly unaffected.
3. With `DisplayResolution=auto` and FG off: "Very little shimmering is still there but, significantly less than
   before".

## 3. Validity Assessment & Root Cause
Genuine OptiScaler defect.
- **The game's flags.** The game declares display-resolution motion vectors:
  `IFeature::SetInitParameters Init Flag LowResMV: false`. Depth is at render resolution.
- **How OptiScaler tags the resources.** On the upscaler FG path (`Upscaler_Inputs_Dx12.cpp` /
  `Upscaler_Inputs_Dx11wDx12.cpp`), display-resolution motion vectors are tagged at the **target** size, while depth is
  always tagged at the **render** size.
- **What XeFG does with them.** XeFG was created with `XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV`. It rejected every
  frame: "motion vector and depth resource resolutions must match" (44,212 and 48,082 times in logs 33892118 and
  33909111), followed by `XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT`.
- **Why the workaround caused shimmering.** `DisplayResolution=false` forces `LowResMV=true` for **both** DLSS and the
  FG tags (log 33962581: `LowResMV flag overrided by user: true`, `XeFG HighResMV: false`). XeFG then accepted the
  frames, but DLSS read display-resolution vectors as render-resolution and reprojected at the wrong scale. The
  reporter's test confirmed it: the shimmering mostly went away with `auto`.
- **The earlier `[XeFG] HighResMV=false` test.** It could not take effect, which is by design. Every FG input path
  copies the game's flags into `[XeFG] DepthInverted` / `JitteredMV` / `HighResMV` and calls `SaveXeFG()`. XeFG reads
  them only at swapchain creation, before the game creates its DLSS feature, so the saved value initialises XeFG
  correctly on the next launch (upstream `b84618e9`, `506f4760`). Both failing logs therefore show
  `XeFG.HighResMV: true`.
- **Audit.** No earlier fix resampled depth or motion vectors for XeFG.
  - `FG-XEMFG-SWAPCHAIN-INIT-INVALID-ARGUMENT.md` covers an unrelated swapchain-init rejection.
  - `FG-XEMFG-MOTION-VECTOR-SLOT-SPLIT.md` covers missing resources.
  - The opt-in `EnableDepthScale` (`DS_Dx12`) divides depth values by `DepthScaleMax`, does not resample, and still
    tags the render size.

## 4. Solution Applied
- **`917ecb76`: a depth resample pass.**
  - `shaders/depth_resample/DR_Dx12` copies depth to a target size with nearest sampling: the source texel containing
    each target texel's centre. Values are never filtered or rescaled.
  - It owns a fresh `R32_FLOAT` UAV target, so a depth-stencil source's flags are never copied.
  - `DRSourceTexel` in `DR_Common.h` is the C++ twin of the HLSL formula.
  - The blob was built on Linux with the new `shader_tools/build_precompiled_shader.sh`: the bundled
    `dxc.exe` / `dxil.dll` under Wine, validated by `dxv.exe`. Rebuilding the committed NR spatial shaders with it is
    byte-identical to the Windows output.
- **`d9700135`: wired into both upscaler FG paths.**
  - When FG outputs to XeFG and the motion vectors are display-resolution with differing tagged sizes
    (`FgDepthResample::Needed`), depth is resampled render → target size and tagged at the target size.
  - The game's depth is made shader-readable for the pass, and its configured `DepthResourceBarrier` state is
    restored afterwards.
  - DLSS keeps the game's own motion-vector flag.
  - Other FG outputs and the opt-in depth-scale path are unchanged. If the resample fails, the original depth is
    tagged as before, with a one-time warning.
- **`OptiScaler.ini`:**
  - `[InitFlags] DisplayResolution` notes that it is not needed for XeFG any more, and that forcing it against the
    game's real vectors makes DLSS misread them.
  - The `[XeFG] DepthInverted` / `JitteredMV` / `HighResMV` comments explain that these are detected and saved
    automatically.
- **Planned and dropped:** making the copied XeFG flags volatile. It would have broken XeFG initialisation, for the
  reason given in §3.

## 5. Verification
- `tests/xefg_depth_resample_unit.cpp` (10/10 PASS) covers:
  - the mapping: identity, centre containment, corners when upsampling, gap-free 1279x720 → 1920x1080 coverage,
    bit-identical copies including NaN / -0.0, and no 32-bit overflow up to 16384;
  - the HLSL, `.hlsl`, C++ twin and signed blob agreeing;
  - the build script;
  - the decision matrix;
  - the wiring order and sizes in both upscaler paths.

  Each part fails against the previous code.
- The XeFG / XeMFG suites still pass (19); `nr_ini_keys_unit` passes.
- clang-format 23 clean; UTF-8 BOM on the new C++ files. Generated shader headers keep the repo convention: no BOM,
  excluded from clang-format.
- **Pending:**
  - An MSVC build.
  - In game: the reporter, in Silent Hill Townfall, with `DisplayResolution=auto` and XeMFG on. Expected results:
    - no "resolutions must match" errors;
    - frame generation works;
    - the shimmering stays at the reduced level;
    - the log shows `XeFG depth resampled 1279x720 -> 1920x1080`.
