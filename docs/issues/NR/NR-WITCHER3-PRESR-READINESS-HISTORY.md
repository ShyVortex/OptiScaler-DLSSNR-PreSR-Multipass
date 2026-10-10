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

No model/runtime, MFG provider or policy, jitter, or default NR placement changes.
Local hardware is RTX 4090/Ada. Neither Witcher 3 nor an Intel GPU is available;
CP2077 testing cannot establish Witcher or Intel-provider compatibility.
