# Owner aggregation exception boundary

Run `./tests/nr_cpu_owner_aggregation_audit/run.ps1` in a Visual Studio x64 developer PowerShell.
The runner extracts the actual namespace-level `BeginFinishedPictureSubmission` and
`NrNotificationScope` from `DlssNr_Dx12.cpp`, and includes the actual `GpuSubmission`
header. Only the child owners and graphics type declarations are fixture boundaries.

The regression checks that all child completion/abandonment callbacks run while the
real recursive registry mutex and production notification scope remain active. It
covers successful completion, a primary exception, a further cleanup exception,
outer-token destruction, original-exception preservation, and exactly-once draining.
A separate CPU thread probes the mutex with nonblocking `try_lock`; no GPU or graphics
runtime is created or loaded.

Observed before protected draining: 57 checks, 12 failures, all for callbacks running
without the registry lock or notification scope. With the catch/drain/rethrow fix:
57 checks, 0 failures. Both versions compile the extracted production code.

Without protected draining, a throwing child unwinds the outer registry lock before
the captured vector destroys its remaining tokens. Their callbacks can then take a
State lock without the registry lock. If cleanup destroys an NGX object which reenters
a queue/reset hook while another thread holds the registry lock and waits for that
State, the reversed lock order can deadlock. This fixture verifies the prerequisite
lock/scope invariant, not actual NGX reentrancy or a Windows watchdog failure.

Brace extraction is deliberately bounded to these ordinary declarations; braces in
future comments or string literals would require updating the extractor. Generated
includes, objects, and executables stay in ignored `build/`.
