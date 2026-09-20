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

Finished-picture late-copy slots now have production-backed CPU regressions for
per-slot producer ownership, replay on every queue, reset races, cancellation,
discarded recordings, reused addresses, normal/held composition selection and
failed-close quarantine. Composition and slot reuse require producer reset or
destruction plus completion of every captured execution. This conservative rule
can delay or skip the optional finished-picture path for long-lived recordings;
its status reports the wait. Ordinary pre-SR NR is unchanged. No new GPU waits
are introduced, and these tests do not establish GPU scheduling or image quality.
