# Floating-origin rebase — the four subsystems that hold world-space state outside the ECS (issue #613)

`Scene::RebaseOrigin(shift)` (the floating-origin / origin-rebasing mechanism, issue #429, first
slice in PR #612) shifts every root `TransformComponent`, all Jolt rigid bodies + static terrain +
character controllers, and Box2D bodies back toward the origin when the camera drifts past
`WorldOriginSettings::RebaseThreshold`. Anything that stores **absolute world-space state outside
that set** silently desyncs on a rebase — no crash, geometry just slowly drifts from
physics/nav/render. Issue #613 closed the four known gaps. Each needed a *different* mechanism; the
non-obvious findings are below so the next person touching a world-space cache knows what a rebase
must do to it.

The umbrella invariant: a rebase is a **pure uniform translation** — after it, every position is
`old + shift` and nothing has moved *relative* to anything else. Velocities, rotations, and
constraint errors are unchanged. Every fix below preserves exactly that.

## 1. Cloth soft bodies — shift the body COM, and the CPU cache too

A JPH soft body **is** a `JPH::Body`. `SoftBodyVertex::mPosition` is documented as *"relative to the
center of mass of the soft body"*, so moving the body's origin translates every particle by `delta`.
The **same** `bodyInterface.SetPosition(id, GetPosition(id) + joltDelta, DontActivate)` used for rigid
bodies is correct and uniform on a cloth — no per-vertex walk. (`GetPosition`/`SetPosition` are exact
inverses, both add/remove `rotation * shapeCOM`, so it's a clean `+= delta` on the COM; the
COM-relative representation stays self-consistent across the next step.) Jolt has **no**
`PhysicsSystem::ShiftOrigin`, so this is a manual loop over `JoltScene::m_Cloths` in `ShiftOrigin`.

Do **not** iterate `SoftBodyMotionProperties::GetVertices()` adding `delta` to each — the vertices are
COM-relative, so that plus the auto-recenter nets to no world movement. The lever is the COM.

The second, easy-to-miss half: `Scene::m_ClothRuntime[*].m_Positions` is an **absolute-world** CPU copy
of the cloth vertices (refreshed each `PostPhysicsSync`, read by the render mesh and headless tests).
`RebaseOrigin` must shift that cache by `delta` too (step 4), or a frame rendered between the rebase
and the next physics sync shows a one-frame cloth pop. `m_Normals` are directions — a translation
leaves them unchanged.

## 2. World-anchored constraints — `NotifyShapeChanged` for the exact shift; rebuild only the pulley

A constraint anchored to an **absolute world point** (a pulley's two fixed pivots, or a single-body
joint realised against `JPH::Body::sFixedToWorld`) holds a world anchor the body shift leaves behind,
stretching the joint. Before #613 this *deferred the whole rebase* (`HasWorldAnchoredConstraints`).

The exact fix for the single-body-to-world joints (Fixed/Point/Distance/Hinge/Slider/Cone/SwingTwist/
SixDOF/**Path**): call `constraint->NotifyShapeChanged(JPH::Body::sFixedToWorld.GetID(), -joltDelta)`.
Each of those types overrides `NotifyShapeChanged` to do `mLocalSpacePositionN -= inDeltaCOM` (Path
shifts `mPathToBodyN` translation), and the world body's COM transform is identity, so its
local-space anchor **is** the absolute anchor — passing `-delta` moves it by exactly `+delta`. The
body-side anchor already moved with its body (COM-relative), so both anchors end up shifted by the
same delta: the constraint error is unchanged (no solver snap) and the joint's rest state, motors and
warm-start impulses survive. This is far better than destroy+rebuild, which would re-pin a compliant
world-anchored joint (a pendulum on a fixed point) to the body's *drifted* current position and lose
the tether. Passing `sFixedToWorld.GetID()` self-matches whichever of body1/body2 is the world body
(it's a reserved id no real body shares).

**The pulley is the exception:** its `mFixedPoint1/2` are private with no runtime setter, and its
`NotifyShapeChanged` only moves the body-attach points, not the pivots. So a pulley must be rebuilt:
shift the authored `PhysicsJoint3DComponent::m_PulleyFixedPointA/B` by `delta`, then
`DestroyConstraint` + `CreateConstraint`. `CreateConstraint` re-derives the two body-attach anchors
from the already-shifted `TransformComponent`s, so all four pulley points move by the same delta and
the `[min,max]` rope-length span is preserved. `HasWorldAnchoredConstraints` is kept for
diagnostics/tests but no longer gates the rebase. Vehicles and two-real-body joints need nothing —
they translate implicitly. Ordering matters: this pass runs **after** the body shift in `ShiftOrigin`,
so `CreateConstraint` reads shifted transforms.

## 3. NavMesh / crowd — rebake, don't translate (Detour's tile origin is unreachable)

Detour has **no in-place navmesh translate**, and you cannot hand-roll one: `dtNavMesh::m_orig` (the
tile-grid origin that `calcTileLoc` feeds into **every** spatial query — `findNearestPoly`,
`queryPolygons`) is `private`, has no setter, and is stored *separately* from the public
`dtNavMeshParams::orig` (const-cast on `getParams()` mutates the wrong copy). You could `const_cast`
the tile `verts`/`detailVerts`, but with a stale `m_orig` every query resolves to the wrong (or a
non-existent) tile. So an origin shift **regenerates** the mesh.

`Scene::RebaseNavigation(shift)` (called from `RebaseOrigin`): shift the bake inputs that live in
absolute world space but are **not** `TransformComponent`s (so the root-transform shift didn't move
them) — each `NavMeshBoundsComponent::m_Min/m_Max` and its `OffMeshLink` endpoints — plus each
`NavAgentComponent::m_TargetPosition`; then `BakeNavMesh()` regenerates from the (already-shifted)
scene geometry and `SetNavMesh` rebuilds the query + crowd. `NavMeshGenerator` composes world
positions from each entity's **own** `TransformComponent`, so the bake reads the new frame even though
this runs before `PropagateWorldTransforms`.

Two traps: (a) `SetNavMesh` resets per-agent runtime state including `m_HasTarget` — so **save the
(shifted) targets before the rebake and restore them after**, or every agent stops. (b) Per
`crowd-manager-follower-parity.md`, a valid navmesh means agents are driven by the **crowd** follower;
`SetNavMesh` zeroes `m_CrowdAgentId`, so `NavigationSystem` re-registers everyone next tick against the
regenerated mesh. Cost: one full Recast bake per rebase — bounded (the bake covers the local
`NavMeshBoundsComponent` region, not the whole 50 km² world) and rare (every ~2 km of travel), but
it's the one rebase step that isn't O(cheap); measure it if nav-heavy scenes hitch.

Trap (c) — **UBSan**: Detour's crowd runs in the rebased frame, and its spatial hashes multiply a
cell/tile index by a large prime (`DetourProximityGrid::hashPos2` `x*73856093`, and the navmesh tile
hash), which overflows `int` for any coordinate more than ~174 m from the origin — a *benign* wrap
(immediately masked into a hash bucket) but flagged as `signed-integer-overflow`. This is inherent to
running a `dtCrowd` at large coordinates (up to the rebase threshold, or at a rebaked shifted frame),
not specific to a test. Since we don't patch vendored recast, `OloEngine/vendor/CMakeLists.txt`
de-instruments *just* that one UBSan check on the recast targets (target-level `-fno-sanitize=signed-integer-overflow`
wins over the directory-level `-fsanitize=undefined`), leaving the other UBSan checks active.

## 4. Terrain streaming — make `WorldOrigin` entity-relative, and it's rebase-free

`TerrainTile::WorldOrigin` was set at tile-request time and **never read** — so every streamed tile
rendered *stacked* at the terrain entity's transform (a real bug independent of any rebase). Fold it
into the per-tile draw: `tileModel = entityTransform * translate(tile->WorldOrigin)` at every draw
site (both `DrawTerrainPatch` and `AddTerrainShadowCaster`, tess and non-tess).

Make it **entity-relative** by feeding the streamer an entity-local camera position
(`cameraPosition - entityTranslation`) so the tile grid — and thus `WorldOrigin` — is anchored to the
entity, not to world zero. Then a rebase needs to touch **nothing** for terrain: the entity
translation and camera both gain `delta`, the entity-local camera is invariant (the streamer never
sees the shift), and the shifted entity transform carries every tile. For the common identity-entity
case this is behaviour-identical to the old world-anchored feed, plus the un-stacking fix.

**Consistency trap:** the non-tessellated cull frustum (`MakeTerrainLocalCullInputs`) must be built
**per tile from `tileModel`**, not once from the bare entity transform — otherwise a tile offset by
`WorldOrigin` is culled against the wrong position while being drawn at the right one, and streamed
tiles wink in/out. (For the single-tile path `WorldOrigin == 0`, so `tileModel ==` the entity
transform and the frustum is unchanged.) Streamed terrain has no Jolt collision bodies yet (per-tile
collision is an open follow-up), so there's no physics side to keep in agreement.

## Guards

- `OloEngine/tests/Functional/Scene/WorldOriginRebasePhysicsTest.cpp` — cloth (rigid vertex shift +
  no CPU-cache pop), world-anchored Fixed joint (applies, anchor follows, body not yanked), pulley
  (pivots shifted, rebuilt, bodies sane), and a many-rebase traversal asserting absolute-trajectory
  *continuity* (a missed shift shows as a ~`|shift|` jump).
- `OloEngine/tests/Functional/Navigation/WorldOriginRebaseNavTest.cpp` — an agent reaches its target
  in absolute space *across* a rebase (navmesh/crowd/target rebased consistently).
- `OloEngine/tests/Rendering/PropertyTests/WorldOriginRebaseVisualEvidenceTest.cpp` — no visible pop at
  50 km, and no pop across a multi-rebase traversal (RMSE between consecutive frames).
- `OloEngine/tests/Scene/WorldOriginRebaseBenchmarkTest.cpp` — rebase cost vs. entity count.
- Terrain streaming has no headless guard (async/file-based); verify the un-stacking + rebase in the
  editor.
