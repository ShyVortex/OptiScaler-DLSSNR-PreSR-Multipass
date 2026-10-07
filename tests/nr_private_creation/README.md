# Private feature creation readiness (CPU only)

Run `tests/nr_private_creation/run.ps1` from PowerShell. The runner locates the
Visual Studio x64 tools and builds into
`outputs/optiscaler-denoise-first-20261006/private-creation` by default. No D3D12
library is linked and no graphics device or game process is created.

`PrivateCreationTests.cpp` compiles the real `PrivateFeatureCreation` helper and
the unmodified production `GpuLifetime.cpp`. It reuses the deterministic COM,
queue and fence doubles from `nr_cpu_submission_audit/SubmissionResetTests.cpp`;
only the external GPU boundary is fake. Nineteen cases cover exact-list fence
completion, unrelated lists/queues, unsubmitted reset/destruction, Reset after
Execute but before Complete, destroyed/reused list and helper addresses, nested
queues, replay, fence creation/signal failure, device removal, abandonment,
quarantine, single-shot recording, retirement, empty inputs, and concurrent
Reset/Complete, and GPU progress between readiness/discard fence polls.

`run.ps1 -BaselineOnly` is an intentionally failing negative control. It reads
the audited baseline `73fab132` using `git show`, mechanically extracts the
actual DeferredSr epoch gate, and compiles that gate into its consumer fixture.
The real tracker proves the creation recording is incomplete in three cases:
no submission, another list submitted, and creation discarded. The baseline
gate incorrectly permits evaluation in all three after the epoch advances.
Expected exit code: 1; its red output is saved separately from the green run.

Create one helper per feature per generation, call `Record` once after successful
NGX creation and after recording resource ownership, capture `BeginSubmission`
before actual Execute, and complete its token afterward. Reset and quarantine
notifications must reach this helper. Do not record later evaluation lists.
The helper provides readiness only; the generation's resource tracker must
separately retain features and textures.

Unsubmitted Reset/destruction yields `Discarded() && Idle()`. A captured pending
submission cannot be discarded by Reset. Failed or abandoned submissions and
removed-device fences remain quarantined: Ready, Discarded and Idle are all
false. Call `FinishSubmitted` only after owner retirement excludes replay.
The helper never waits for the GPU or uses a frame/time threshold.

`run.ps1 -GateOnly` compiles the current production DeferredSr readiness gate;
seven cases demonstrate blocked evaluation without exact completion and failure
after an unsubmitted discard. `run.ps1 -RetirementOnly` compiles its production
retirement method with the real helper and resource tracker, including GUID-keyed
command-list watches. Its two cases require old helpers to remain reachable until
resource completion, and quarantined retired generations to remain registered.
The pre-change retirement method fails both (saved under `retirement-red`).

`run.ps1 -DestructorOnly` extracts the actual destructor ownership predicate and
refreshes the actual `ReadyToDestroy` method using the existing State audit runner.
Real trackers demonstrate parent completion alongside independent private signal
failure. The predicate must retain ownership for Deferred/Denoise resource
uncertainty, private creation uncertainty, and registered retired generations.
Controls require ordinary idle cleanup and retention after a failed picture wait.
The earlier parent-only predicate fails the four private-ownership cases (saved
under `destructor-red`). This checks the cleanup decision, not a GPU fault or UAF.

These tests establish CPU bookkeeping behavior, not GPU/driver scheduling or
game compatibility. The baseline extraction uses stable region markers; update
the runner explicitly if the audited revision or gate layout changes.
