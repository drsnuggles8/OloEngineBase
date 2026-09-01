# Subsystem notes — gameplay, physics, navigation, terrain

Accumulated gotchas from dialogue/quest, inventory, Jolt joints and ragdolls, Detour navigation,
and terrain/foliage work. Reference notes, not failure postmortems — see [README.md](README.md).

Salvaged from worktree-scoped memory (see `docs/process/task-loop.md` Phase 7 for why that is now
the wrong destination).

---

## 1. Two `JPH::PhysicsSystem` instances exist; only one is ever stepped

`Physics3DSystem` (`Physics3D/Physics3DSystem.{h,cpp}`) owns an `m_PhysicsSystem` that **nothing
ever calls `Update()` on**. The system actually simulated is `JoltScene::m_JoltSystem`, stepped
from `JoltScene::Simulate()`.

Before #540 this meant a project's `PhysicsSettings` — gravity, solver iterations, Baumgarte, sleep
thresholds, fixed timestep — reached the dead system and had **zero effect** on the simulation,
while `SetSettings()` logged "Physics settings applied successfully". Only
`MaxBodies`/`MaxBodyPairs`/`MaxContactConstraints` reached the live sim, because
`JoltScene::InitializeJolt()` read them directly at init.

**Before wiring any new `PhysicsSettings` field, confirm it reaches
`JoltScene::InitializeJolt()`** — not just `Physics3DSystem::UpdatePhysicsSystemSettings()`, where
the plausible-looking helper lives. There is no live-update path for either; settings are read once
at init.

## 2. The physics lifecycle path is `Scene::`, not `JoltScene::`

`Scene::OnPhysics3DStart()` / `OnPhysics3DStop()` are canonical — they iterate the registry
directly. `JoltScene::OnRuntimeStart()` / `OnRuntimeStop()` are **dead code** with no caller in
`src/`, `OloRuntime` or `OloEditor`. `FunctionalTest::EnablePhysics3D()` calls the `Scene::` pair.

Any new physics-lifecycle hook goes in `Scene::OnPhysics3DStart/Stop`. Two related rules:

- `OnComponentAdded<Rigidbody3DComponent>` / `<PhysicsJoint3DComponent>` create the Jolt body or
  constraint **immediately** once `m_JoltScene->IsInitialized()`. To author bodies/colliders/joints
  at start without redundant creation and "already exists" warnings, run the authoring pass
  **before** `m_JoltScene->Initialize()` — the hooks then no-op and the normal loops build
  everything once.
- **Add the collider component before the `Rigidbody3DComponent`**, so the body resolves its shape.
  A body with no collider silently falls back to a 1×1×1 box (with a warning) via
  `JoltShapes::CreateShapeForEntity`.

## 3. Adding a `JointType3D` value or a joint field — three footguns the checklist misses

1. **Two hardcoded upper-bound guards must be bumped**, or the new enum value silently fails to
   deserialize and to set from Lua. There is no `Count` sentinel; both reference the literal last
   enumerator:
   - `Scene/SceneSerializer.cpp` — `jointTypeInt <= static_cast<i32>(JointType3D::<last>)`
   - `Scripting/Lua/LuaScriptGlue.cpp` — `v <= static_cast<int>(JointType3D::<last>)`
2. **SaveGame fields append as a new tail.** `SaveGameComponentSerializer::Serialize` for this
   component is a chain of `if (ar.AtEnd())` back-compat tails. A new field goes *after* the last
   one: an `ar <<` in the save branch **and** a matching `if (ar.AtEnd()) {defaults} else {ar << …}`
   probe plus sanitize in the load branch. Miss the probe and old save-games crash or misread.
3. **Pulley fixed points are authored in WORLD space** (`m_PulleyFixedPointA/B`), unlike every other
   anchor on the component, which is body-local — they are level-fixed hooks. Jolt's
   `mMaxLength = -1` is an "auto = current length" sentinel (a rope that contracts but cannot
   extend); **preserve negatives through sanitize**.

> **Gear / RackAndPinion are body-to-body v1, without drift correction.** Each entity has exactly
> one `PhysicsJoint3DComponent`, so there is no way to reference the companion Hinge/Slider entities
> that Jolt's `SetConstraints` wants for numerical-drift cancellation. Referencing two other joint
> entities is its own design. **Testing trick:** the coupling constraint alone demonstrates the
> velocity/position relationship without companions — disable gravity, start the driving body
> purely on-axis (pure-Z spin for gear, pure-X slide for rack), then read the driven body's rotation
> from the `TransformComponent` quaternion (`2*atan2(q.z, q.w)` for a pure-Z spin).

`PathConstraint` is deferred — it needs a variable-length spline serialized as a point list, a
heavier authoring problem.

## 4. Ragdolls are expanded into ordinary ECS physics, not `JPH::Ragdoll`

`JoltScene::CreateRagdoll` expands `RagdollComponent` into components the existing Rigidbody3D /
collider / PhysicsJoint3D infrastructure already realises:

- The skeleton comes from `m_Skeleton` (runtime `Ref`, not serialized) or the `SkeletonComponent`
  on `m_SkeletonEntity` (`0` = self). Bone entities are found by tag == bone name via
  `BoneEntityUtils::FindBoneEntityIds`.
- **Pass A** — bones without a `Rigidbody3DComponent` get a generated Dynamic body plus a
  `SphereCollider3DComponent`. A pre-authored body is kept, so **authoring the root bone Static
  anchors the ragdoll to the world**.
- **Pass B** — each parent→child link gets a `SwingTwist` joint on the **child**,
  `CollideConnected=false`, **pivoting at the parent bone's origin**
  (`localAnchorA = invChildRot*(parentPos−childPos)`, `localAnchorB = 0`, twist along the bone).
  A single COM-centred bone with pivot == COM would not swing under gravity.
- `DestroyAllRagdolls` (called first in `Scene::OnPhysics3DStop`) removes exactly the generated
  entities, restoring the authored scene.

Not done, deliberately: physics→animation pose write-back, blend modes, mid-game enable/disable,
per-bone capsule fitting, anatomically-correct joint placement, and nested-bone world-transform
resolution (bone transforms are read as world-space — a flat layout, matching how JoltBody already
interprets `TransformComponent.Translation`).

## 5. Detour returns a partial path as *success*

`dtNavMeshQuery::findPath` returns a path to the **nearest reachable** poly and sets
`DT_PARTIAL_RESULT` when the target is off-navmesh or in a disconnected region. It is not a
failure. `NavMeshQuery::FindPath` used to check only `dtStatusFailed || npolys == 0`, so an agent
walked to the closest point while `BTMoveTo` — checking arrival against the raw, never-reached
target — re-issued the same target and returned `Running` **forever**.

The shape of the fix, and the contract for any future nav consumer:

- `FindPath` returns `enum class FindPathResult { Failed, Partial, Complete }`, with Partial
  detected via `dtStatusDetail(status, DT_PARTIAL_RESULT)`.
- `NavAgentComponent` carries a runtime `m_TargetUnreachable` flag (`OLO_SERIALIZE(Skip)`).
- `NavigationSystem` latches it on Failed/Partial, **keeps `m_HasTarget` set** so the terminal state
  is observable, and stops recomputing — otherwise it re-runs `FindPath` every frame.
- `BTMoveTo` checks `m_HasTarget && m_TargetUnreachable` first and returns `Failure`.
- Exposed as C# `NavAgentComponent.IsTargetUnreachable` and Lua `targetUnreachable`.

**A nav "target set" flow must have a terminal path for "cannot reach".** Checking only `HasPath`
or arrival distance is the bug. Equally, "did the agent reach the goal?" must be tested by the last
corner's distance to the goal, never by the `FindPath` bool.

`NavMeshGenerator::Generate` is public and had no `isfinite` / `max > min` bounds check before
`rcCalcGridSize` — garbage or inverted bounds meant a huge grid allocation. One was added.

> Historical note: these memories flag "`CrowdManager::AddAgent` has zero callers" as an open
> follow-up. **That was since fixed** — see
> [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md); a valid navmesh now routes
> every `NavAgentComponent` through the crowd follower.

## 6. Off-mesh links

Jump/drop/ladder/teleport connections live as `std::vector<OffMeshLink> m_Links` on the existing
`NavMeshBoundsComponent` — deliberately **not** a new ECS component. `NavMeshGenerator::Generate`
emits each as a Detour `dtOffMeshConnection`.

**The START endpoint must be inside the bake bounds (XZ) and within `AgentMaxClimb` vertically plus
`radius` horizontally of a walkable poly**, or `dtCreateNavMeshData` silently drops the whole link.
Endpoint flags/area are pinned to the walkable poly's (`flags=1`, `RC_WALKABLE_AREA`) so the default
query filter routes across them.

Not built: per-area traversal cost, editor gizmo authoring, and Lua/C# scripting of the link list
(a vector-of-structs isn't `OLO_PROPERTY`-scriptable yet).

## 7. Dialogue handlers carry the speaker's UUID, and must guard component presence

`DialogueSystem` dispatches action and condition nodes to handlers registered via
`RegisterActionHandler` / `RegisterConditionHandler`. The callbacks carry the **UUID of the
speaking entity** first: `void(UUID dialogueEntity, const std::string& name, const std::string& args)`.
UUID rather than `Entity` **deliberately**, to keep the Dialogue header free of the heavy
Scene/Entity/EnTT include — resolve the `Entity` from the scene when needed.

Registration happens in one composition root, `Scene::InitDialogueSystem()`, which covers both the
runtime and the headless harness (`FunctionalTest::EnableDialogue`), so Functional tests exercise
the real path.

To add a new dialogue→subsystem handler, create a `*DialogueBridge` in that subsystem's `Gameplay/`
folder and register it from `Scene::InitDialogueSystem` — the bridge knows both sides, so Dialogue
and the subsystem never include each other.

> **Guard `DialogueComponent` at every use site.** Any method calling
> `entity.GetComponent<DialogueComponent>()` while a live `DialogueStateComponent` may exist must
> `HasComponent` first and fall through to `EndDialogue(entity)` — as `EndDialogue` itself already
> does. `AdvanceDialogue` and `ProcessNode` did not, so removing the component mid-conversation
> (editor button, script, gameplay logic) hit an EnTT assert in Debug and UB in Release.
>
> This generalises: wherever a *state* component's lifetime is meant to track a *definition*
> component's presence but nothing enforces it structurally, a `HasComponent` guard at the point of
> use is cheaper than trying to prevent removal.

## 8. Inventory economy conventions

Trading lives in `Gameplay/Inventory/InventorySystem`. A shop is an entity with
`ItemContainerComponent.IsShop == true`; the trader has an `InventoryComponent`.

- Currency is **integer `i32`**. No floats ever enter pricing — integer plus `i64` overflow guards.
- **Shops have unlimited gold** — only the player side tracks currency, deliberately, to avoid the
  serializer touch-points a currency field on `ItemContainerComponent` would add.
- Sell price is `ItemDefinition::SellPrice` if set, else buyback = `BuyPrice / 2`. Integer division
  **truncates toward zero**, so a `BuyPrice` of 1 sells for 0. Deliberate.
- Trades are **all-or-nothing** — validated against a *copy* of the destination, with currency
  moving only after the item transfer commits. A rejected trade is a no-op on both sides.
- A whole-stack move preserves the original `ItemInstance` (ID, affixes, durability); only a partial
  pull from a larger stack mints a fresh `InstanceID`.

## 9. Terrain erosion: the editor brush is non-deterministic by construction

`Terrain/Editor/TerrainErosion` is a GPU compute shader running one thread per droplet. Many
droplets read-modify-write the same heightmap texels with **no synchronization**, so it cannot be
reused for anything needing a reproducible result.

The generation post-pass therefore has a separate **CPU sequential** port,
`TerrainGenerator::ApplyErosion`, mirroring the shader's droplet physics and PCG RNG exactly —
only the dispatch order differs. It lives in the pure headless `GenerateHeightField` path.

For any terrain feature needing reproducibility (golden renders, deterministic regen, save-game
stability) use the CPU path. Note the droplet model **loses mass** — sediment carried when a droplet
ends is discarded — so repeated iterations flatten relief. Faithful to the brush, not a bug.

## 10. Foliage without an albedo cutout is invisible

`Foliage_Instance.glsl` does `if (texColor.a < AlphaCutoff) discard;` — **the cutout comes from the
albedo texture**. `CommandDispatch::DrawFoliageLayer` binds the diffuse unit only when
`albedoTextureID != 0`, so an empty `AlbedoPath` leaves whatever was last bound: billboards either
fully discard or render as solid garbage quads.

The editor's "+ Add Layer" defaults `AlbedoPath = ""`, so this bites silently — the layer scatters
instances (visible in counts) and nothing appears on screen. Point it at
`assets/textures/grass.png`, which ships under `OloEditor/assets`.

Auto-population (`TerrainGenerator::MakeFoliageLayersFromRules`) sets it on every emitted layer,
takes each layer's placement mask from the matching material rule, and adds **zero new ECS state** —
it reuses the already-serialized `FoliageLayer`, so no cross-binding edits.

> **Two verification gotchas.** The headless `RunEditorFrames` harness does **not** composite the
> foliage draw pass — instances generate but a framebuffer read-back shows only terrain, so assert
> the *instance count*, not pixels, and verify visually in the live editor over MCP. And foliage is
> correctly masked to grass areas, so from a distance sparse billboards blend into the green terrain
> *material* — get close before concluding it didn't render.

## 11. The gameplay system scheduler, and EnTT owning groups (issue #453)

**Rule:** a per-tick gameplay system registers in `Scene::GetGameplayScheduler()` (`Scene/Scene.cpp`)
with `Reads/Writes/After/Before` constraints against the named channel constants
(`GameplayChannel::k*`), never raw strings. The execution order is derived
(`Scene/SystemScheduler.{h,cpp}`: Kahn topological sort, registration order as the tie-break;
duplicate name, dangling reference or cycle throws `SystemSchedulerError` in every build config).
Declare the real data flow, not a position: a missing edge is invisible in sequential order because
the tie-break masks it, and becomes a data race under the parallel executor. The seam test
`SystemSchedulerTest.GameplayScheduleHonoursDocumentedSeams` therefore asserts reachability
(`SystemScheduler::DependsOn`), never positions.

Two parallelism mechanisms with different audit bars:

1. **The physics shadow.** The ECS-free physics world step (Box2D + Jolt) runs as an engine task
   between the `PhysicsKick` and `PhysicsFence` nodes (`Scene::KickPhysicsStep` /
   `FencePhysicsStep`); the ECS-touching phases (contact drain, character/vehicle input, joint
   break, transform sync) were hoisted into kick/fence on the game thread, see the `JoltScene` phase
   methods. Systems registered between kick and fence run on the game thread and need no
   worker-thread audit, only independence from physics and transform state. Current occupants:
   Dialogue, Quest.
2. **`.Parallelizable()` worker dispatch.** Only after a thread-safety audit against the other
   marked systems: no GL/GPU calls, no EnTT structural changes, no `GameplayEventBus` publish, no
   draws from the seeded game-thread RNG stream (#452). Record it in the audit-table comment at the
   schedule builder, and extend the `Scene::Scene()` storage pre-warm list with every component type
   the system's views or groups touch (see
   [notes-core-and-threading.md §16](notes-core-and-threading.md)). Unmarked systems are join-all
   barriers; everything joins before the tick returns.

Bisect lever for both: `OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1` or
`SystemScheduler::SetParallelExecutionEnabled(false)`: same systems, same order, one thread. The
editor Simulate tick keeps the synchronous `Scene::StepPhysics`, which shares the `JoltScene` phases
and `PostPhysicsSync`, so the two paths cannot drift.

**Owning groups.** The hottest multi-component loops use `registry.group<Owned>(entt::get<Observed>)`
for packed iteration. EnTT allows a component to be owned by at most one group (`get<>`-observed
types are unconstrained), so `TransformComponent`, owned by the 2D sprite loop, is borrowed by the
physics-sync, particle and audio groups. Read and update the ownership-map comment at the top of
`Scene/Scene.cpp` before adding an owning group: owning an already-owned component is a runtime
assert, and an owned pool cannot be `.sort()`ed.
