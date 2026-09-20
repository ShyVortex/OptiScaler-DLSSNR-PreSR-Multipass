# MFG caller policy regressions

Run `pwsh -NoProfile -File tests/mfg_callers/run.ps1` with Visual Studio C++
build tools installed. This is a CPU-only suite: it does not load NVIDIA DLLs,
create a graphics device, or start a game.

The runner extracts the production caller-policy helpers from
`NVNGX_DLSS_Dx12.cpp`, `DLSSG_Dx12.cpp` and `NVNGX_Parameter.cpp`. The tests cover
verified capabilities, acceptance-only pacing, failed-patch limits, refusing
evaluation after incomplete rollback, Streamline-versus-direct-NGX ownership,
and generation acknowledgment for direct NGX overrides/Default intent.

Only the selected helper bodies execute in this harness. Their callers and
external runtime integration require the full solution build and separate
runtime validation. The deliberately small brace extractor must be updated
if braces in comments or strings make extraction ambiguous.
