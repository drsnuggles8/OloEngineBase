# Making a system that MOVES entities run on a worker thread

Discovered building the flocking / boids system (issue #731). Read this before
marking any new gameplay system `.Parallelizable()` — especially one whose whole
job is to move things.

## The contradiction you will hit

The audit table in `Scene::GetGameplayScheduler` ends with:

> Navigation / MorphEval are pinned main-thread: TransformComponent writes / GL
> vertex-buffer upload.

So a system whose output is a `TransformComponent` write cannot be marked. But
"the expensive AI/steering/crowd system should run on a worker" is a completely
reasonable requirement, and the two are only in conflict if you model the system
as one node.

**Split it along the write boundary, not along a subsystem boundary:**

| Node | Runs | Reads | Writes |
|---|---|---|---|
| `<Feature>Steering` — the expensive solve | worker (`.Parallelizable()`) | transforms, its own scratch | *only its own component's runtime fields* |
| `<Feature>Movement` — integrate + move | game thread (unmarked) | the steering channel | `TransformComponent` |

The two are joined by a **channel of their own** (`kBoidSteering`), not by
`kLocalTransforms`. That is the whole trick: the RAW edge on that channel is
what guarantees the movement node cannot start until the worker task has been
joined, and it is what keeps the steering node free of any transform-write
declaration that would make the marking a lie.

Both nodes are cheap to place:

- The **steering** node belongs *inside* the existing marked cluster
  (`Abilities` / `Audio` / `ParticlesCPU`). Registered outside it, an unmarked
  system sits between it and every other marked system, so it is dispatched to a
  worker that the game thread immediately waits on — technically "on a worker",
  practically pointless.
- The **movement** node belongs *last*, after every transform reader. Its
  `ReadsWrites(kLocalTransforms)` then picks up write-after-read edges from all
  of them, so it can never yank a pose out from under a system mid-tick. The
  cost is the same one Navigation already documents: the move composes into the
  world matrix on the **next** tick's `PropagateTransforms`.

## Pin it with edges, never positions

`SystemSchedulerTest` has a canonical-order test, and it is *not* enough: the
registration-order tie-break satisfies a position check even when the edge is
missing, which is exactly the gap that becomes a race under the parallel
executor. Assert `DependsOn` in both directions — the edges that must exist
(movement after steering, steering after the physics fence) **and** the ones
that must not (steering unordered against every other marked system, which is
what earns it the mark).

## Determinism: specify the ORDER, not just the result

If the parallel system accumulates floats over a neighbour set — and a steering
system does, three times over — then "returns the right set" is not enough. A
spatial structure whose traversal order can vary run-to-run silently changes the
rounding, and the whole simulation diverges. So:

- Give the acceleration structure a **documented visit order** and test it
  (`FlockSpatialHash` sweeps cells lexicographically and, within a cell,
  ascending item index — a stable counting-sort scatter, not a hash-map walk or
  a linked list built by insertion).
- Snapshot the world once at the top of the pass and steer against the frozen
  copy. Otherwise an agent's answer depends on how far through the loop you got.
- Any per-agent cap ("only fold in the nearest N") is only reproducible because
  the visit order is — say so where the cap is written.
- If the system needs no RNG at all, say **that** in the audit entry. "No RNG"
  is a stronger statement than "uses the seeded stream", and it is the one #452
  actually wants to hear.

## Two traps specific to writing the determinism test

1. **Do not reset the scene by destroying and recreating the entities.** EnTT
   recycles entity ids from a LIFO free list, so the rebuilt set iterates in a
   different order; the snapshot indices shuffle and the float accumulation
   order changes. The test then fails for a reason that has nothing to do with
   the code under test. Re-seed the *existing* entities in place instead.
2. **Start real workers, or the parallel run is the sequential run.** With no
   `FScheduler` workers running, `Tasks::Launch` degrades to inline execution,
   so a sequential-vs-parallel comparison passes vacuously. Call
   `LowLevelTasks::FScheduler::Get().StartWorkers()` in `SetUpTestSuite` (the
   `SystemSchedulerParallelTest` fixture is the pattern) and toggle
   `SystemScheduler::SetParallelExecutionEnabled` around the two runs.

## Before adding a fifth spatial structure

The engine has four now, and the differences are real, not accidental:

| Structure | Shape | Keyed by | Why not reuse it |
|---|---|---|---|
| `Scene/SpatialAcceleration.h` `SceneSpatialIndex` | unbounded hash grid | `UUID` | Indexes **every** entity; results need an entity-map lookup + component fetch per neighbour, and one global cell size. |
| `Fluid/CPUFluidSolver` | **dense** grid, head/next lists | index | Dense only works over a bounded domain; agents roam. |
| `Networking/Replication/SpatialGrid` | fixed-size hash grid | `UUID` | Sized to network relevance distance, for interest management. |
| `AI/Flocking/FlockSpatialHash` | unbounded hash grid, CSR | index into the caller's SoA | — |

Note that the #731 issue text claims "a grep finds no spatial hash anywhere in
`OloEngine/src`". That was wrong when it was written — **three** already
existed. Check before repeating a gap claim from an issue, and count properly
when you correct one: the first draft of this very table said "three", having
missed the networking grid that `SpatialAcceleration.h` cross-references by
name.
