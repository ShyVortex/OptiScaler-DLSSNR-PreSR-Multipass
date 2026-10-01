# NR GPU resource lifetime

A Cyberpunk session reached 34.03 GB VRAM use as NR time rose from roughly 4 to 23 ms. Logs showed unresolved recordings at teardown, supporting memory pressure as a contributor without attributing every allocation to NR.

Resources retire only after recordings close and submitted fences complete:

- A private COM notification closes destroyed command-list recordings without retaining/dereferencing the list. Destruction alone does not prove GPU completion.
- Replaced owners keep receiving notifications. NR-owned presentation lists close at retirement; replayable game lists still require reset/destruction and completion.
- Codec-only shaders register for submissions; finished-picture captures share parent tracking. Drained parents release discarded private generations even without a written timestamp. Hooks precede feature-creation recording.

Notifications use stable owner snapshots and defer destruction. Child retirement is allowed; unresolved teardown work survives until process exit.

Preparation happens before the application's queue call. If allocation fails,
the hook still submits the application batch, so all matching owner generations
are quarantined before the registry lock is released, including owners not yet
visited. The fallback does not allocate or collect; it invalidates unsubmitted
finished-picture/enlarger/hold metadata rather than advertising those inputs as
ready. A failed preparation can retain storage until process exit. Notification
cleanup defers failed collection instead of throwing from a scope destructor.

Tracker operations retain their shared implementation through collection and
mutex unlock, including when a retirement callback destroys the wrapper. The
destructor's unresolved-retention decision uses that same tracker lock.

## Finished-picture copy slots

Each finished-picture slot also tracks its producer recording with `GpuLifetime`.
Every execution, including replay after presentation or cancellation, contributes
its actual queue completion. Texture reuse and normal/held composition require
producer reset/destruction and completion of all captured executions. This prevents
a later replay from overwriting inputs while late composition reads them. The
original per-slot fence still protects the late commands.

Acquisition reclaims unsubmitted captures after recording destruction or reset,
without waiting for a signal that was never promised. Failed late-command closure
remains explicitly quarantined. Submission metadata requires a captured producer
recording, so an unrelated command list at a reused address cannot claim an old slot.

This rule can delay or skip finished-picture processing when a game retains
replayable command lists. The status explains the wait and suggests pre-SR if it
persists. No new queue or CPU waits are added, and the NR pre-SR route does not use
these slots. CPU regressions exercise production ownership and selection; they do
not validate GPU scheduling, visual results, or game-specific compatibility.

## Reentrant destruction

A Cyberpunk access violation was traced to retired-vector compaction. Calling NGX destruction inside `std::erase_if` could re-enter hooks and mutate that vector.

The collector now removes completed callbacks into a separate batch before invoking them. Nested collection is guarded; later batches drain callback-created retirements once. An idle query cannot allow tracker destruction while callbacks are active.

The production WARP regression covers 64 destroyed unsubmitted lists, blocked submitted work, later reclamation, replay, wrapped identities, multiple queues and reentrant retirement of 64 callbacks. The old collector fails that reentrant case. These checks do not prove long-session VRAM stability; see [game limits](NR-UPSTREAM-REVIEW.md).

## Concurrent command-list reset

Starfield's v0.8.0 crash report identifies a null recording entry in `ResetRecording`. The ordinary buffer-transition path could mutate the tracker outside the state lock used by reset notifications, including after NR initialization failed.

Tracker operations now serialize internally. Buffer creation/transitions, codec dispatch and frame-hold entry also follow registry -> state -> tracker lock order. No GPU wait was added. A four-worker WARP test races recording/reset/retirement against collection and checks all 8,000 callbacks run exactly once; it crashes before the fix and passes afterward. Starfield itself remains untested locally. Its separate `0xBAD0000B` NR initialization error is not resolved by this fix.
