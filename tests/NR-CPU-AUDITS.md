# Neural Rendering CPU audits

Run every CPU-only regression from a Visual Studio x64 developer PowerShell:

```powershell
.\tests\run-nr-cpu-audits.ps1 -OutputDirectory C:\path\outside\the\repository
```

The runner compiles production lifetime, timing, descriptor-allocation, encode,
owner-aggregation, and state-submission code against deterministic CPU boundary
fixtures. It also runs a post-only negative control that must exit with code 1.
No Direct3D device, WARP adapter, GPU workload, game, or driver stress test is
created. The suite is Windows/MSVC-specific and requires `cl.exe` in `PATH`.

Three sub-runners mechanically extract production method bodies. Their brace
scanner is intentionally small and must be updated if braces are introduced in
comments or string literals within those methods.

These checks demonstrate source-level lifetime and failure-handling behavior.
They do not reproduce or identify the cause of a game hang or operating-system
watchdog event.

The pre-existing finished-picture late-copy slots are outside this suite. Their
per-slot `pending`/`submitted` fence state does not currently renew its fence for
a replay of the same still-open producer command list. The general lifetime,
timing, and descriptor replay checks here do not establish that those special
late-copy slots are replay-safe.
