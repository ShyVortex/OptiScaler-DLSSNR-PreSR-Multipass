# v0.7.7 - Apply NR to the finished picture

**Apply NR to the finished picture** moves the effect after the game's lighting and effects. This can help with green noise, including the issue reported in Kingdom Come: Deliverance II. The option is off by default and is available to native DirectX 12 games; it is not tied to KCD2.

- Supports the frame-generation and ordinary presentation paths, with automatic SDR, HDR10 and scRGB colour handling.
- Enable **Run the model before Super Resolution** alongside it to generate the changes at input resolution, upscale them with a separate DLSS pass, and apply them to the finished picture. NR runs once. This combination requires DLSS SR and is experimental: the later colour transfer is approximate and can affect the HUD. RR and the separate NR every-second-frame mode are excluded.
- NR timing now says **ms elapsed** and explains that it includes time shared with other GPU work. Completed GPU fences protect timing readback and query reuse, and changing placement clears old readings.

## Why the old reading could double without losing FPS

Controlled KCD2 captures at the same camera, 3840x2160, FP8, one pass and frame generation off measured about 51.6 FPS late, 50.6 FPS ordinary, and 51.7 FPS late again. NR timestamps measured about 14 ms late versus 8 ms ordinary. Independent timestamp pairs agreed and both queues used the same timestamp frequency.

Keeping the late path unchanged and limiting the game to 29 FPS reduced the NR reading to 7.2-8.0 ms. Removing only the limit restored about 14 ms. The longer interval depends on competing GPU work and scheduling; it is not a second model pass or a doubling of GPU work per frame. The display retains the measured value rather than dividing it by two. These captures do not distinguish individual shader preemption from simultaneous GPU resource contention.

## Validation and limits

The release candidate ran in KCD2 with full late NR and with the pre-SR combination. Frame-generation settings were exercised on and off; the active FG hook continued applying NR, although this scRGB setup reported an HDR10 compatibility warning and its interpolation detector remained OFF, so generated-frame quality is not independently certified by this run. Earlier KCD2 prototype testing covered the original late-hook green-noise fix with DLSSG.

GPU shader tests cover SDR/scRGB residual application, HDR10 PQ reference luminance and round trips, wide gamut, alpha, highlights, bounded edits and dark-pixel changes stored in FP16. HDR10 output has shader validation, not a live HDR10 game comparison. D3D12 WARP timing tests cover unfinished queries, ring reuse and discarded recordings. Results in other games and HDR setups still need in-game checks.

The NVIDIA NR model is supplied separately, as in previous releases. Extract the complete package and follow INSTALL-DLSSNR.md.
