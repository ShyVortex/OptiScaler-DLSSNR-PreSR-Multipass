# Shared NR model warmup regression

Run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/nr_history_warmup/run.ps1`.
Optional `-Repo` and `-OutputDirectory` select source and evidence locations.
The runner first runs the private seam suite and reuses its actual routing and
Before success-guard extracts, then compiles this focused CPU trace using MSVC.

Four-frame fixtures execute the actual PrepareRunModels resolution/placement
retry-and-allocation prefix and the actual private Before invocation. The GPU
model boundary requires a later submission epoch after creation. Empty handoff
after private NR starts must protect its shared history during warmup; ordinary
post-SR fallback would change placement and recreate the model forever.

The same-size fixture isolates the placementChanged trigger; the different-size
fixture also exercises resolutionChanged. Both expect a transient raw first
frame and successful private NR on the second frame, without a second NR model.
Early unsupported/denoiser warmup fallback remains covered by the prerequisite
seam suite. Logs, extracted production segments and source hashes are saved in
the selected output directory. No GPU or game calls occur.
