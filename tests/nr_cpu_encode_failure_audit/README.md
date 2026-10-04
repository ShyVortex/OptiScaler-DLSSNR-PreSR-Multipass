# Encode dispatch failure regression

Run `./tests/nr_cpu_encode_failure_audit/run.ps1` in a Visual Studio x64 developer PowerShell.
The runner extracts the actual `EncodeInput` method and declaration. CPU boundary
fixtures make individual dispatches fail and track resource states and successful
writes. No graphics runtime, GPU, WARP device, or game code is executed.

Cases cover optional exposure-meter failure, required encode/downsample failure,
supersample fallback failure, successful reduced/supersampled encoding, and targets
with and without UAV support. Every recorded transition is checked against the
resource's current state. Failed required passes must return false, clear model input,
and restore touched scratch/target states; failed metering must skip readback.

Observed baseline: 208 checks, 22 failures. After failure propagation: 207 checks,
0 failures. Check counts differ because valid early exits change which transitions
are recorded and checked.

The baseline adapter treats the old void return as success so the same fixture can
demonstrate its missing failure contract without rewriting production logic. The
Run integration is reviewed and compiled separately; these tests do not execute Run
or prove GPU synchronization. The bounded brace extractor must be updated if future
comments or strings introduce unbalanced braces. Build products stay in ignored `build/`.
