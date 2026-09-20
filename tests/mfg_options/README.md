# DLSSG options CPU regressions

Run `pwsh -NoProfile -File tests/mfg_options/run.ps1` from a Windows checkout with
Visual Studio C++ build tools installed. No game or GPU driver entry point runs.

The runner extracts the production Streamline hook method bodies and compiles
them against the repository's real Streamline ABI headers. Config, logging,
global state and external runtime calls are CPU test substitutes. Guard-page
tests cover the known v1-v5 structure lengths; a contained CPU access violation
is an expected failure on the original full-size-copy implementation.

Coverage includes accepted-versus-requested pacing, queued UI intent and
in-flight generation changes, one GetState per game query, version-bounded
copies and unknown-version pass-through, verified capabilities, stale limits,
and patch/rollback failure guards. This does not validate NVIDIA kernels,
GPU synchronization, live module patching or runtime crash freedom.

The extractor assumes balanced braces in the selected methods, including
comments and strings. Compilation catches many extraction errors, but method
extraction and test seams are not a substitute for the full solution build.
