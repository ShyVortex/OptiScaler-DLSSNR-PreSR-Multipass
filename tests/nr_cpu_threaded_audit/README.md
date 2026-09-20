# Deterministic CPU-only threaded submission audit

`ThreadedSubmissionTests.cpp` directly includes production `DlssNr_GpuLifetime.cpp`
and its real submission token. Only external COM/device/queue/fence boundaries are
faked. No D3D12 library is linked; no Direct3D, WARP or game process is initialized.

Two threads rendezvous at six C++20 barriers, with no sleeps or timing-dependent
race oracle. The main thread captures an old submission. The recorder thread
resets its command-list object, creates a new recording on that same object, and
captures/closes the new recording before the old notification arrives. The old
queue then completes. The new generation must remain owned until its separate
notification and queue completion. Both retirement callbacks must run exactly
once, and neither may run while its simulated GPU work remains pending.

The fake reference counts and fence values are atomic. Fake mutable queue and
private-data storage use mutexes; the device reference is immutable after setup.
Fixture setup and teardown occur outside the worker thread's lifetime.

Build from a Visual Studio x64 developer shell at the repository root:

```powershell
cl.exe /nologo /std:c++20 /EHsc /W4 /Itests\nr_cpu_threaded_audit /Fotests\nr_cpu_threaded_audit\ThreadedSubmissionTests.obj /Fetests\nr_cpu_threaded_audit\ThreadedSubmissionTests.exe tests\nr_cpu_threaded_audit\ThreadedSubmissionTests.cpp ole32.lib
.\tests\nr_cpu_threaded_audit\ThreadedSubmissionTests.exe
.\tests\nr_cpu_threaded_audit\ThreadedSubmissionTests.exe --post-only
```

The regular run must exit 0. The `--post-only` negative control deliberately omits
the old pre-submit capture at the simulated call site, invokes the existing
post-only API after reset, and must exit 1 with premature retirement. It changes
no production code and demonstrates that the test detects the missing pin.

This is one controlled inter-thread handoff, not a stress test or proof of all
concurrent schedules. It does not exercise the global NR owner registry/lock order,
driver callbacks, actual queue hooks, resource barriers, shader execution, GPU
latency, or the Cyberpunk watchdog failure.

Observed validation: MSVC `/W4` compiled without warnings. The captured-submission
run exited 0 with both retirements exactly once, both fence signals 1, and no early
release. The post-only negative control exited 1 with early release and zero old
queue fence signals. No production source was changed for either run.
