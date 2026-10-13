# Select live performance history by rendered-frame progress

Verify that the renderer completed the requested number of frames after settling a camera,
and that its history generation stayed unchanged through history retrieval, before publishing
that camera's samples. An estimated sleep is useful for pacing requests; it is not proof of
sample ownership.

While diagnosing #1594, the acceptance script labelled the last 120 history entries as a new
camera's window after only 66 new recorder frames had been captured. The previous camera's
samples necessarily remained in that window. The reproduction is retained in
[sampling-window-before.json](../testing/evidence/cpu-scope-ring-1594/sampling-window-before.json).

Two plausible guards also failed source review:

- `liveness.frameIndex` counts editor updates before throttle skips. History counts renderer
  frames. A ready endpoint does not prove every intervening update rendered.
- `liveness.ticking` becomes false after a 750 ms gap. A genuine one-second hitch can therefore
  look stalled immediately after it completes. Explicit iconification/readiness and bounded
  renderer progress distinguish this from a parked loop.

Use `renderFrameNumber` and `renderFrameGeneration` from `olo_perf_snapshot`. Wait for sufficient
progress with occasional window-level probes, then require the same generation on
`olo_perf_frame_history`. Reject unwritten entries. A reset can clear history while the native
renderer frame number continues increasing, so a decreasing-number check alone is insufficient.

Keep recorder scope frames and legacy history as separate populations unless matching sample
IDs were recorded. Legacy GPU values can describe older resolved queries; use the recorder's
`sameFrameGpuMs` and explicit validity for attribution. Record an actual camera-event frame
marker before naming a particular scope frame as the cut itself.

The producer lifecycle contract and ten Python regressions cover throttling, long hitches,
history resets, unwritten slots, and bounded stalls. Measurements and limitations are in the
[#1594 diagnosis report](../analysis/cpu-scope-ring-1594.md).
