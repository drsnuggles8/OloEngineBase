# Destructible objects and debris (issue #459)

The runtime destruction feature: a `DestructibleComponent` shatters into physical
`DebrisComponent` chunks on sufficient damage; debris settles under physics and is
cleaned up within a hard budget. Read this before touching `DestructibleSystem`,
adding a debris body, or wiring a new break trigger. The strategy decision (why
we spawn pre-authored debris instead of fracturing meshes at runtime) is
[ADR 0013](../adr/0013-destructible-debris-asset-swap-not-runtime-fracture.md).

---

## The two physics "layer" systems are different, and only ONE reaches Jolt's DEBRIS

This is the trap the whole feature pivots on. There are **two unrelated layer
numberings**, and the obvious API uses the wrong one:

- **Jolt object layers** — `ObjectLayers::{NON_MOVING=0, MOVING=1, TRIGGER=2,
  CHARACTER=3, DEBRIS=4}` (`Physics3D/JoltLayerInterface.h`). This is what Jolt's
  broadphase collision filter (`ObjectLayerPairFilter`) actually consults, and
  it's what `JPH::Body::GetObjectLayer()` returns.
- **`CollisionLayers::{… Debris=7}`** (`Physics3D/Physics3DTypes.h`) — a separate
  user-facing numbering stored in `Rigidbody3DComponent::m_LayerID` and mapped
  through `PhysicsLayerManager`.

`JoltBody::SetCollisionLayer(CollisionLayers::Debris /*7*/)` does **NOT** put a
body on `ObjectLayers::DEBRIS`. It calls `GetObjectLayerForCollider(7, …)`, which
either maps to a *PhysicsLayerManager user layer* (`NUM_LAYERS + 7`) or falls
back to `MOVING` — never to the built-in `ObjectLayers::DEBRIS = 4`. A debris
body set that way collides with the player like any dynamic body.

**How debris is actually kept off the player.** The `ObjectLayerPairFilter`
(`JoltLayerInterface.h`) does not pair `CHARACTER` with `DEBRIS` —
`ShouldCollideDirectional` lists `CHARACTER → {NON_MOVING, MOVING, TRIGGER}` and
`DEBRIS → {NON_MOVING, MOVING}`, so neither direction includes the other. The
character controller drives its `ExtendedUpdate` through exactly that filter
(`GetDefaultLayerFilter(CHARACTER)` in `JoltCharacterController.cpp`), so it never
even *queries* debris bodies. That is the operative mechanism. (The controller
*also* carries an `m_IgnoreCollisionLayers` contact-validate mask whose bit 4 —
nominally `CollisionLayers::Trigger`, numerically also Jolt `ObjectLayers::DEBRIS
= 4` — would reject a debris contact, but the pair filter already stopped the
query one step earlier, so that mask is a redundant second guard, not the reason.)

The correct call is therefore a **raw Jolt object-layer write**:
`JoltBody::SetToDebrisLayer()` → `bodyInterface.SetObjectLayer(id,
ObjectLayers::DEBRIS)`. It deliberately bypasses `SetCollisionLayer`. If you ever
"simplify" it to `SetCollisionLayer(CollisionLayers::Debris)`, debris silently
starts shoving the player and nothing fails to compile.

## Runtime debris bodies: collider first, then a pre-populated rigidbody

`Scene::OnComponentAdded<Rigidbody3DComponent>` builds the Jolt body *on
component construction*, reading the collider shape and the rigidbody fields at
that instant. So when spawning a debris chunk:

1. `AddComponent<BoxCollider3DComponent>(box)` — **first**.
2. Populate a `Rigidbody3DComponent` template (`m_Type = Dynamic`, `m_Mass`)
   **before** `AddComponent` — the hook reads those fields at construction;
   setting them after leaves a default Static body.
3. Fetch the body (`GetPhysicsScene()->GetBody(entity)`), then
   `SetToDebrisLayer()` + `AddForce(dir * impulse, EForceMode::Impulse)`.

The box collider's half-extents stay `0.5` (a unit box); `JoltShapes` multiplies
them by `TransformComponent::Scale`, so setting the debris transform scale sizes
the collider and the (unit-cube) visual together.

## Break triggers consume existing seams; handlers only flip a flag

The break trigger never invents a damage path. It reacts to three inputs, all
funnelling into `DestructibleComponent::m_PendingBreak`:

- `DestructibleSystem::ApplyDamage(scene, entity, amount)` — the direct API
  (scripts, gameplay).
- `EntityKilledEvent` — a combat kill (published by `GameplayAbilitySystem`).
- `JointBrokeEvent` — a breakable joint giving way (`Physics3D/PhysicsEvents.h`,
  written for exactly this), gated by `m_BreakOnJointBreak`.

The bus subscriptions are registered in `DestructibleSystem::WireEvents`, called
from `Scene::OnRuntimeStart`. **The handlers only set a component flag** — a plain
field write, so they are safe to run mid-iteration inside whatever system
published the event (the structural spawn happens later, on the Destructible
node). Because a headless `FunctionalTest` never calls `OnRuntimeStart`, a test
that exercises these seams must call `WireEvents(&scene)` itself.

## Structural spawn/destroy runs on the node, not the script deferred queue

Debris spawn (bulk `CreateEntity`) and cleanup (bulk `DestroyEntity`) are
structural registry changes. The **script** deferred-command queue
(`docs/agent-rules/script-structural-command-safe-point.md`) exists because
scripts run *mid-iteration* inside `UpdateScripts`. A first-class scheduler node
that controls its own iteration does **not** need that queue — it follows the
`InventorySystem` pattern: accumulate targets during the view walk into local
vectors, finish the walk, *then* spawn/destroy. `DestructibleSystem::OnUpdate` is
strictly phased: (1) collect breaks, (2) age debris + collect expired, (3)
destroy expired debris + broken sources, (4) spawn debris under budget. No
structural change happens while a view is being iterated.

## The budget is a hard invariant, not a target

`kMaxLiveDebris` (256) caps *simultaneously-live* debris. On a break that would
exceed it, the system evicts the **oldest** existing debris first (sorted by
`DebrisComponent::m_Age`), then clamps the spawn count if eviction still can't
free enough. Fresh debris is favoured over stale. The live count therefore never
exceeds the cap even under a burst of many simultaneous breaks in one tick — the
newly-spawned pieces of *this* tick are not eviction candidates (they're not in
the surviving-set snapshot taken in phase 2), so a single tick's breaks fill the
cap exactly and further breaks that tick spawn nothing. That dropped debris is
the budget doing its job, not a bug.

## Scheduler placement

Registered as the unmarked `Destructible` node, `.Reads(kLocalTransforms)`. That
read pins it after every transform writer registered before it — including
`PhysicsFence`, so a `JointBrokeEvent` published inside the fence is visible the
same tick. It **must stay unmarked**: it makes EnTT structural changes, which the
`Parallelizable` audit forbids on a worker thread. Order is pinned by
`SystemSchedulerTest` (the derived order lists it right after `Inventory`, its
resource twin).

## Cross-binding: which touch-points each component needed

- `DestructibleComponent` is **all-trivial** (its only runtime fields carry
  `OLO_SERIALIZE(Skip)`, ranges are `OLO_SERIALIZE(Clamp, …)`), so its scene-YAML
  (de)serialize is **fully generated** — no hand-written `SceneSerializer` block,
  no `kComponentsCustomSerialize` entry. It DOES carry a hand-written
  `SaveGameComponentSerializer::Serialize` overload + `REGISTER_SAVE_COMPONENT`
  (it's persistent gameplay state), and a Lua usertype (scripts deplete `health`
  to trigger a break).
- `DebrisComponent` is runtime-only transient. It's excluded from save-games
  (`kComponentsNotInSaveGame`) — a saved mid-explosion would restore orphaned
  chunks. Everything else (tuple, on-add/remove no-ops, scene YAML) is generated
  and harmless (authored scenes never contain debris).

## Where the contract is pinned

- `OloEngine/tests/Functional/Gameplay/DestructibleDebrisTest.cpp` — shatter fires
  exactly once, `DestroyOnBreak`, debris settles (falls) and is cleaned up within
  lifetime, the budget cap holds under a 400-chunk burst, and the combat-kill /
  joint-break bus seams (and the `m_BreakOnJointBreak` opt-out).
- `OloEngine/tests/ComponentRoundTripTest.cpp` — `DestructibleComponent` scene-YAML
  round-trip, including that the `Skip` runtime flags do **not** persist.
- `OloEngine/tests/Scene/SystemSchedulerTest.cpp` — the `Destructible` node's
  derived order and its `DependsOn("Destructible", "PhysicsFence")` seam.
