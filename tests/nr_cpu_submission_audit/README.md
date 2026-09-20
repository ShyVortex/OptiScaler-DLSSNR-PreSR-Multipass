# CPU-only submission/reset regression

Audited upstream revision: `dc5611d62dac84dc657198d2508a2c7385df651d`
(the head of OptiScaler neural-rendering PR #1158 when this test was added).

`SubmissionResetTests.cpp` includes the production
`DlssNr_GpuLifetime.cpp`. Only the external COM, queue, and fence boundary is
replaced with deterministic CPU objects. It does not link D3D12, create a
graphics device, or execute GPU work.

Build in a Visual Studio x64 developer shell from the repository root:

```powershell
cl.exe /nologo /std:c++20 /EHsc /W4 /Itests\nr_cpu_submission_audit `
  /Fotests\nr_cpu_submission_audit\SubmissionResetTests.obj `
  /Fetests\nr_cpu_submission_audit\SubmissionResetTests.exe `
  tests\nr_cpu_submission_audit\SubmissionResetTests.cpp ole32.lib
.\tests\nr_cpu_submission_audit\SubmissionResetTests.exe
```

The baseline fails when Reset closes a recording after the simulated real
Execute boundary but before the post-submit notification. The transaction path
captures the exact generation before Execute and completes it after Execute.
Cases also cover nested submissions, generation reuse, replay on another queue,
failed signals, device removal, abandoned tokens, and idempotent completion.

This establishes the bookkeeping defect and its CPU-level correction. It does
not establish that this ordering caused a game hang, reproduce a driver failure,
or validate GPU scheduling.

`run-state-allocation.ps1` mechanically extracts the production state
submission and retired-enlarger collection methods. Its CPU fixture injects
`std::bad_alloc` at every observed allocation site and checks that owner,
initialization, copy, and hold pins remain balanced. It also verifies invalid
initialization selection, retired metadata pins, and collector retry after an
allocation failure:

```powershell
.\tests\nr_cpu_submission_audit\run-state-allocation.ps1
```

The extraction helper counts braces and therefore must be updated if braces are
introduced in comments or string literals within either extracted method.
