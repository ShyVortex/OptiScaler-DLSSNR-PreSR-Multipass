# Direct NR compatibility runtime

The RTX20/30/40 compatibility DLL can be rejected by the NGX driver's signed loader. NR now tries a direct backend after a driver creation failure, provided the driver returned no feature handle.

The backend resolves the runtime's required NR exports and caller-path imports by name. There is no version, file-size or hash allowlist and no fixed import offset. New builds can move their imports without an OptiScaler update. Missing exports, malformed imports and initialization failures produce specific log messages. No runtime is downloaded or bundled.

`DlssNr_CompatibilityRuntime.*` owns loading and model calls. Its small `Paths.cpp` adapter supplies OptiScaler's NGX search paths. The existing NR proxy retains the backend until GPU retirement, then releases features. For its own module it shuts down the last device owner and unloads; for a preloaded module it drops only its own reference and leaves device-wide shutdown to the original owner.

The model requires its caller's module path to contain `nvngx.dll`. The runtime's named ANSI/Unicode path imports supply that alias only during direct calls and only for the calling OptiScaler module. Other queries chain the existing import target, including loader/overlay wrappers; teardown restores that target. There is no helper DLL, driver modification, or change to files on disk.

See [the restriction audit](NR-RESTRICTION-AUDIT.md) for the related recovery and ownership changes.

## Validation

- Release x64 build; proxy routing, GPU retirement and pipeline capture regressions.
- Real RTX 5090: driver rejection followed by successful direct creation; four init/shutdown/unload cycles, two independent features per cycle, 24 fenced evaluations and finite, non-black image readback.
- Original 310.8.0 compatibility runtime and a different-hash metadata variant both pass the GPU test. Calls outside backend scope still fail the caller check.
- Import lookup handles relocated ANSI/Unicode entries and rejects malformed tables. A non-NR module is rejected for missing exports. The signed runtime retains its normal driver path.

Run `tests/nr_compatibility/run.ps1 -Driver <installed _nvngx.dll> -RuntimeDirectory <compatibility DLL folder>` from a Visual Studio developer PowerShell. Optional `-AdditionalRuntimeDirectories` exercises other local runtime builds through the direct backend.

This covers DX12 NR, including the DX11-to-DX12 bridge. Native Vulkan still uses its existing driver path. The tester's 310.8.2 binary is not available locally; its execution, RTX 40 hardware, in-game modes, image quality and long-session stability remain untested.
