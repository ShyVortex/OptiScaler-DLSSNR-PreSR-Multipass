# CPU-only NR timing-slot regression

`TimingResetTests.cpp` includes the actual production `DlssNr_GpuTime.h` and
`DlssNr_GpuLifetime.cpp`. Its external
COM, query, queue and fence boundaries are CPU fakes. No D3D12 library is linked, no
Direct3D/WARP device is initialized, and no GPU work is executed.

The regression catches a timing slot becoming available while the corresponding
submission is still pending. The command-list reset is deliberately interleaved
between the real-Execute boundary and the post-submit notification. The observable
assertion is that the next recorded sample must use a different query index. Normal
submission ordering, failed queue signals and device removal are separate controls.
It also covers the same command-list object being re-recorded before the delayed
notification, a completed command list replayed on a different queue, and bounded
pool exhaustion followed by completion. A small test-local adapter keeps the original
post-submit API runnable for the failing baseline and uses the production submission
transaction when that API is available; it contains no ownership logic.

Build from a Visual Studio x64 developer shell at the repository root:

```powershell
cl.exe /nologo /std:c++20 /EHsc /W4 /Itests\nr_cpu_timing_audit /IOptiScaler /Fotests\nr_cpu_timing_audit\TimingResetTests.obj /Fetests\nr_cpu_timing_audit\TimingResetTests.exe tests\nr_cpu_timing_audit\TimingResetTests.cpp ole32.lib
.\tests\nr_cpu_timing_audit\TimingResetTests.exe
```

These tests validate NR's ownership bookkeeping. They do not emulate GPU scheduling,
establish an invalid timestamp resolve, or reproduce the Cyberpunk watchdog crash.

Upstream `dc5611d6` baseline, observed with MSVC `/W4` during this port:

```text
normal order: first_query=0, pending_next_query=2: PASS
reset before notification: first_query=0, pending_next_query=0: FAIL
signal failure: first_query=0, next_query=2: PASS
device removal: first_query=0, next_query=2: PASS
same object, new recording: old_query=0, new_query=0, next_query=0: FAIL
replay on another queue: first_query=0, pending_next_query=0: FAIL
pool exhaustion: pending=8, distinct=1, skipped=1, resumed=1: PASS
completed timing readable before reset; replayable slot retained: FAIL
readback once/replay/recycle: first=1, replay=1, total=2: FAIL
```

Compilation succeeded without warnings; the baseline executable exited 1. The
failure requires no model retirement or resource rebuild. Whether any of these
orderings occurred in the preserved game session remains unknown.

Patched results: all nine CPU cases pass with MSVC `/W4`, no compiler warnings,
executable exit 0. The seventh pool-exhaustion control remains green. The added eighth
case verifies that a completed timestamp is still readable before command-list Reset,
while its replayable query slot remains unavailable for reuse; this compatibility
case was observed RED then GREEN during implementation. Its hand-authored payload is
1,000 ticks at 1,000,000 ticks/second, so the independent expected duration is 1 ms.

The timer now delegates per-sample completion/replay/reset ownership to the actual
GpuLifetime tracker. Standalone GPU timing smoke builds must also link that tracker
translation unit and ole32.lib, and include the OptiScaler directory. No GPU/WARP smoke
was executed in this task.

Review follow-up adds a ninth behavioral case covering repeated readback, replay
fence gating, completed-slot recycling, new timing publication, and ClearLast.
The real timer originally mapped 12 times for the first completed submission,
24 cumulatively after replay, and 42 overall. With a per-sample consumed flag,
re-armed by each captured replay, those counts are exactly 1, 2, and 3. The existing
lastSequence ordering remains intact: old recordings cannot replace newer displayed
timings, and ClearLast is not repopulated by older samples. All nine cases pass.
