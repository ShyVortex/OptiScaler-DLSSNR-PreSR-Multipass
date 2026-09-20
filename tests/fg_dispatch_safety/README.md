# CPU dispatch admission regression

Run `./tests/fg_dispatch_safety/run.ps1` from a Visual Studio x64 developer shell.

The runner mechanically extracts each production `RUI_Dx12::Dispatch` and
`HC_Dx12::Dispatch` signature and admission prefix, stopping immediately before
`ScopedGpuTime_Dx12`. A test-only `true` return marks reaching that rendering
boundary. The entry checks remain the actual production code.

Tests require a null command list to return false before the rendering boundary,
even with a valid swapchain/resource and initialized renderer. Controls cover
valid inputs, null swapchain/resource, and an uninitialized renderer.

This verifies only the admission prefix, not the complete render pipeline,
command-list lifecycle, GPU timing, or GPU execution. The fixture uses empty CPU
types and never creates a D3D12/WARP device or loads a graphics DLL. Extraction
fails if its production signature or rendering-boundary anchor moves.

Observed on 2026-09-20 with MSVC `/W4 /WX`: before the guards, 10 checks and
2 failures (the RUI and HC null-command-list cases); after the guards, 10 checks
and 0 failures. Both compilations succeeded without warnings.
