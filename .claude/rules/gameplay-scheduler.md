---
paths:
  - "OloEngine/src/OloEngine/Scene/SystemScheduler.{h,cpp}"
  - "OloEngine/src/**/*System.{h,cpp}"
  - "OloEngine/src/OloEngine/Gameplay/**"
  - "OloEngine/src/OloEngine/AI/**"
---

# Gameplay scheduler (issue #453)

Per-tick systems register with declared reads and writes, and the order is derived from them.
Declare the real data flow, not a position. Marking a system `Parallelizable()` needs a
thread-safety audit and an EnTT storage pre-warm. Rules:
[notes-gameplay-physics-nav.md §11](../../docs/agent-rules/notes-gameplay-physics-nav.md),
[notes-core-and-threading.md §16](../../docs/agent-rules/notes-core-and-threading.md).

Simulate mode skips the gameplay scheduler: test scheduler systems in Play.
