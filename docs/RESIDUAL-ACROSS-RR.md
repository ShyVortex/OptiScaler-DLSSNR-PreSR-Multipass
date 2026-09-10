# Experimental residual across RR

On D3D12, enable **Apply before Super Resolution** and **Carry the pre-SR edit across RR** while the game uses Ray Reconstruction. Both new settings default off/unchanged. Native Vulkan has no host integration for this option.

NR resolves to a private render-resolution texture. A separate shader accumulates the signed difference from the original input, reprojecting with the game's active motion-vector rectangle and scale. After the same RR evaluation succeeds, a shader samples that history at output resolution and applies the shared detail strength once. The game's RR colour input is left untouched. Hold frame, Compare, Debug view and Show skin mask must be disabled for this mode.

Only a successful pre-pass can arm the post-pass; command list, parameter block and output must match. A skipped/failed/new pre-pass invalidates the previous result. Carrier textures start in their actual resource state, and the composition texture tracks the output's allocation dimensions and format independently of the input. Missing resources/PSO, failed dispatch, unsupported output layout or zero strength do not copy an invalid result onto the RR output. The post-pass restores the game's root/descriptor state.

Cold history and cuts fade in at the configured blend rate (default 0.08). This is an experimental temporal average: view-dependent detail can smear and fast motion can lag. There is no depth-based disocclusion test, so motion validity alone does not establish that a surface is unchanged. It is not a claim of superior quality to post-RR NR.

Validation: Release x64 build; the committed residual CSO executed through Windows WARP for cold/warm history, signed values, active MV offsets, invalid motion, render-to-output sampling, alpha and zero strength; CPU seam tests cover missing, mismatched, skipped and duplicate consumption. Both residual shader blobs/headers are regenerated from the source; the existing main NR shader is unchanged. No new gameplay quality or physical RTX 20/30 validation is claimed.
