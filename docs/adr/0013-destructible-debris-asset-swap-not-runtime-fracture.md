# Destructible geometry spawns pre-authored debris chunks — it does not fracture meshes at runtime

Issue #459 ("Physics: destructible geometry — runtime fracture / breakable
objects / debris") asks for *"a breakable object [that] shatters into physical
debris on sufficient damage; debris settles and is cleaned up within budget."*
The issue body sketches two ways to get there, and this ADR picks one before any
code is written, because the choice has long consequences for the asset
pipeline, the physics load, and determinism.

Nothing structural about the engine changes here. The decision records *what the
runtime does on break* and *what it deliberately does not do*.

---

## The two designs

**A. Runtime fracture.** On break, take the object's mesh and compute a Voronoi
(or clip-plane) shatter *at runtime*: generate N new convex chunk meshes,
build a collision hull for each, upload vertex buffers, and spawn them. This is
what "runtime fracture" literally names.

**B. Pre-authored debris, swapped in.** The breakable object references a
debris **chunk mesh** (authored offline — a Voronoi shatter is an *authoring*
step, not a runtime one). On break, the runtime spawns N physical debris
entities that render the chunk mesh (or, absent an authored chunk, reuse the
object's own mesh, or a primitive) and destroys/hides the original. The runtime
does no computational geometry at all: it is a **swap-and-spawn**, not a
shatter.

## Decision

**Design B.** The break path spawns pre-authored debris; it never fractures a
mesh at runtime.

## Why

1. **The acceptance criterion is about debris behaviour, not geometry
   synthesis.** "Shatters into physical debris … settles … cleaned up within
   budget" is satisfied fully by design B. Runtime mesh generation is effort
   spent on a sub-goal the criterion never states.

2. **Runtime fracture is a computational-geometry subsystem, not a feature
   slice.** Robust Voronoi clipping of an arbitrary (possibly non-convex,
   possibly non-manifold) mesh, convex decomposition of the fragments, per-chunk
   inertia — each is its own multi-week problem with its own failure modes
   (degenerate fragments, self-intersections, exploding triangle counts). None
   of it is what #459 was scored for (`craft 8`, a Physics × Assets × Gameplay ×
   ECS integration task), and shipping a fragile half of it would be worse than
   shipping none.

3. **Determinism and budget are trivial in B and hard in A.** Design B's debris
   count is authored (`ChunkCount`), so the budget cap and the settle/cleanup
   policy are simple counting problems with a deterministic RNG only for scatter
   direction. Design A's fragment count is a function of the mesh and the cut
   planes — variable, hard to bound, hard to reproduce across machines.

4. **It renders and simulates through paths that already exist.** Debris is an
   ECS entity with a `MeshComponent` + `Rigidbody3DComponent` + a box collider on
   the already-plumbed **`Debris` physics layer**
   (`Physics3D/JoltLayerInterface.h`, `ObjectLayers::DEBRIS = 4`, which the
   character controller already ignores). No new render pass, material, shader,
   or asset type. This also keeps the change inside the "no rendering work"
   constraint the task was picked under.

5. **Authoring a fracture is where a fracture belongs.** A Voronoi shatter tool
   is a perfectly good thing to have — as an *offline importer/DCC* step that
   produces a chunk mesh asset. Design B is forward-compatible with that: the
   day such a tool exists, its output is exactly the `ChunkMesh` handle the
   `DestructibleComponent` already references. Design A would have to be thrown
   away to get there.

## What "debris chunk" resolves to, in priority order

The `DestructibleComponent` carries an optional `AssetHandle ChunkMesh`. On
break, each debris piece's `MeshComponent` is set from the first of:

1. `ChunkMesh` if it resolves to a `MeshSource` (the authored pre-fractured
   chunk — the intended production path);
2. else the broken entity's own `MeshComponent::m_MeshSource`, scaled down (a
   crate becomes N small crate-ish lumps — a reasonable default with zero
   authoring);
3. else a `MeshPrimitive::Cube` (always visible, never asset-dependent — the
   path headless tests and un-authored objects take).

Physics never depends on the visual: every debris piece gets a box collider
sized from `ChunkScale`, so settling and cleanup are testable with no GPU and no
asset manager.

## Consequences

- The trigger **consumes existing damage/break seams** rather than inventing a
  parallel damage path: a public `DestructibleSystem::ApplyDamage`, plus bus
  subscriptions to `EntityKilledEvent` (combat kill) and `JointBrokeEvent`
  (`Physics3D/PhysicsEvents.h`, written for exactly this). See
  `docs/agent-rules/destructible-debris.md` for the wiring and its gotchas.
- **Structural connectivity / progressive collapse** (a wall that loses
  integrity as neighbours break) is listed as *optional* in the issue and is
  **out of scope** for this PR. It is a genuinely separate feature (a
  connectivity graph + a solver) and is filed as its own scored issue rather
  than left as a partial in #459.
- If runtime fracture is ever wanted, it enters as an **authoring/import** tool
  feeding the `ChunkMesh` handle — not as a runtime code path. This ADR should
  be revisited (not reversed) at that point.

## Status

Accepted. Implemented by the `DestructibleComponent` / `DebrisComponent` +
`DestructibleSystem` slice for #459.
