# MFG unlock CPU tests

Run the production-backed MFG patcher tests from the repository root:

```powershell
.\tests\mfg_unlock\run.ps1
```

The runner compiles the real `MfgUnlock.cpp` and scanner against controlled PE images. Its seams are
limited to configuration, GPU identity, and Windows memory/cache APIs so protection and rollback
failures can be injected without loading an installed NVIDIA runtime or executing GPU code.

These cases replace the deleted `tests/mfg_unlock_arch_gate_unit.cpp`, which copied the old broad
architecture-comparison algorithm into a standalone simulator and therefore could pass without
exercising production behavior. Unknown comparisons are now explicitly tested as no-change cases.
