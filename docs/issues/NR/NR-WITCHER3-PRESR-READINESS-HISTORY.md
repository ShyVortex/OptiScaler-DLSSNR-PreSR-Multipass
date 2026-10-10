# Ordinary DX12 NR creation and history correctness

Assessment against Shy main `635c34e9` and issue #25. These conditional defects
are verified with CPU production-backed fixtures; they are not proof of the
reporter's continuous Witcher 3 flicker or of a GPU hang's root cause.

## Creation readiness

Ordinary model creation previously accepted a changed presentation epoch as
readiness, even when its exact command recording was unsubmitted or discarded.
Conversely, discarded unsubmitted creation could remain permanently in warmup.

Use the existing per-generation `PrivateFeatureCreation` tracker beside resource
lifetime. Readiness requires its actual completion probe. Recreate only when the
creation recording is proven discarded. Normal submitted resets retain the
feature; failed/abandoned/quarantined submissions and device removal do not count
as discard proof. Current and retired trackers receive submission/reset events,
and creation completion precedes owner completion.

`tests/nr_model_creation/run.ps1` compiles the complete production proxy and GPU
lifetime code, with typed NGX and CPU COM/fence boundaries. Its 15 cases cover
epoch-only admission, wrong lists, delayed completion, independent owners,
discarded destruction/reset, retirement, and unresolved submission failures.
`-BaselineRevision 635c34e9` runs those same cases against the old proxy. This
replaces a duplicated simulation test that asserted the incorrect epoch rule.

## Same-owner NR history

Checkbox and hotkey enable transitions stamp a shared, mutex-protected history
generation. Each DX12 owner consumes it once, setting its existing temporal Reset
without retrying/rebuilding model features. Observed disabled intervals also set
Reset, so re-enabling cannot silently reuse pre-disable history. Stable frames
do not reset. Retry and capture generations remain independent. The change is
DX12-only; it does not alter Vulkan scheduling or feature lifetime.

`tests/nr_history_controls/run.ps1` compiles the actual consumption body and
checks observed and unseen Off/On, independent owners, stable history, initial
Reset, and existing retry/capture behavior. Its boundaries do not render pixels.

## Game reconstruction history

The existing accepted-route identity distinguished raw input (0) from private
denoise-first stages (1–3), but treated successful ordinary Pre-SR NR as raw.
Track actual ordinary Color substitution as route 4. Entry/fallback changes
temporarily set the game's Reset; accepted evaluation commits the route, rejected
evaluation leaves the transition pending for retry. Stable routes retain history.
Caller Reset and raw game jitter are restored/preserved by the existing scope.
NR still precedes game reconstruction when configured Pre-SR; no FG order changes.

The production-extracted `tests/nr_game_history` suite covers ordinary entry,
fallback, stable frames, rejected transitions, retry, all private route crossings,
caller Reset and jitter restoration. No new telemetry or user options are added.

No model/runtime, MFG provider or policy, jitter, or default NR placement changes.
Local hardware is RTX 4090/Ada. Neither Witcher 3 nor an Intel GPU is available;
CP2077 testing cannot establish Witcher or Intel-provider compatibility.
