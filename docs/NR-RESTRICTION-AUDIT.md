# NR restriction audit

Reviewed NR runtime loading, failure/retry paths, private DLSS scheduling, GPU retirement and exposure/capture limits.

## Fixed

- **Pristine import addresses required:** chain the existing ANSI/Unicode path target instead of insisting on the Windows export address. Restore the original target on teardown. This addresses a possible cause of the Starfield 310.8.2 import-adaptation failure; new logging distinguishes pointer races from protection failures.
- **Preloaded runtime rejected:** acquire a reference to the exact loaded module. Preserve foreign features and leave device-wide shutdown to its original owner.
- **One driver error eligible for fallback:** try direct NR after any failed creation without a returned handle. Successful driver calls and partially returned handles retain their existing ownership.
- **Teardown race rejected a new owner:** coordinate module/device teardown and acquisition rather than turning a transient overlap into an initialization failure.
- **One unreadable guide disabled NR:** skip that frame and reset temporal history; retry naturally when readable depth/motion returns.
- Remove the redundant exclusive file read, deduplicate search paths, and correct the misleading “disabled for this session” log: actual model failures have a Retry action.

The version/hash/size gate and fixed import offset were already removed in v0.8.2.

## Retained

- GPU fences, resource-state restoration, null/bounds checks and separate feature ownership prevent executing with invalid resources or releasing live GPU work.
- The four-retired-generation limit pauses further allocation while old DLSS GPU work remains outstanding; it resumes after collection. Removing it would restore unbounded allocation during repeated reconfiguration.
- Actual model creation/evaluation errors stop that context until Retry; they are runtime results, not file-version exclusions. Exposure/capture budgets limit inspection work and do not block model loading.
- NR exports and parseable import tables remain necessary. Differing wrappers for duplicate imports of the same API remain an explicit unsupported case; the backend cannot preserve multiple distinct chains with one adapter.
- Native Vulkan still uses its driver path. Optional MFG instruction-patch signatures remain necessary to locate actual patch sites; they are separate from NR loading.

## Validation

Proxy, WARP lifetime/pipeline and import-parser regressions; NVIDIA GPU tests for normal, different-hash, preloaded and wrapped runtimes. Tests verify existing wrapper chaining/restoration, survival of a foreign feature across backend teardown, and 32 owner acquisitions across four threads. The signed runtime also passes direct evaluation when already loaded by NGX.

The tester’s exact 310.8.2 binary, RTX 4090 gameplay and long-session behavior remain unverified locally. The guide-skip change is compiled; its in-game recovery has not been exercised.
