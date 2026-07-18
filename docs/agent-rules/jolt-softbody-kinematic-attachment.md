# Kinematically attaching a Jolt soft body to a skeleton bone (issue #460 cape slice)

Welding cloth vertices to an animated bone (a cape's top edge to a character's chest) has three
non-obvious pitfalls. All three are load-bearing — get one wrong and the cape detaches, jitters, or
freezes rigid, and *unit tests still pass* (the classic "green but wrong" cloth failure).

## 1. Drive a pinned vertex by VELOCITY, not position

`JPH::SoftBodyVertex` documents: *"at run-time you should only modify the inverse mass and/or velocity
of a vertex to control the soft body. Modifying the position can lead to missed collisions."* A pinned
particle carries `mInvMass == 0`, so gravity/damping never touch its velocity — and
`SoftBodyMotionProperties::IntegratePositions` advances **every** vertex (pinned included) by
`mVelocity * dt`. So set

```
velocity = (targetWorld - currentWorld) / dt
```

once before the step and the vertex lands exactly on the target across the frame's sub-steps, carrying
its free neighbours along with a physically-correct "whip". `dt` = the frame delta the step will
advance; picking it slightly large just under-shoots and self-corrects next frame — never overshoots.
Re-set it every frame (the velocity persists on a pinned vertex, so a stale value would keep moving it).

Velocities/positions are stored **relative to the body's centre of mass**. For a cloth the COM frame
carries no rotation (created identity; soft bodies only *translate* their COM in `UpdateSoftBodyState`),
but convert anyway (`comTransform.Multiply3x3Transposed(worldVel)`) so it stays correct if that changes.
Wake the body afterwards (`BodyInterface::ActivateBody`, **outside** the `BodyLockWrite` scope — it takes
its own locks) or a settled/slept cape ignores the drive.

## 2. A bone's world transform is `entityWorld × skeleton.m_GlobalTransforms[boneIndex]`

`AnimationSystem::Update` writes the posed bone matrices into `Skeleton::m_GlobalTransforms` (model
space, **before** the inverse-bind-pose used for skinning) — it does **not** write bone-entity
`TransformComponent`s. So for an `AnimationStateComponent`-driven character the bone *entities* are stale;
the authoritative, per-tick-current source is the skeleton's global transforms. Resolve the bone by
**name** (`Skeleton::m_BoneNames`) → index once at bind, then each tick:
`boneWorld = Scene::GetWorldTransform(charEntity) × skeleton->m_GlobalTransforms[boneIndex]`.
An empty/unresolved bone name should fall back to the entity's own world transform (the socket case), not
go inert. Capture each welded vertex's rest position in bone-local space at bind
(`inverse(boneWorld_bind) × vertexWorld_bind`) so it stays welded at that offset.

## 3. Timing — drive BEFORE the step, on the game thread

The drive must run after animation has posed the skeleton for the tick and *before* the physics world
step integrates the velocities — i.e. next to `ClothWindSystem::OnUpdate` in `StepPhysics` /
`KickPhysicsStep` (Jolt resets applied forces after `PhysicsSystem::Update`, same "queue before the step"
contract as buoyancy/wind). Because it reads ECS + skeleton state it must stay on the **game thread**
(the kick, not the physics-shadow or a `.Parallelizable()` worker). The character entity's cached
`WorldTransformComponent` is one tick stale at kick time (PropagateTransforms runs after the fence) — the
sub-frame lag is invisible for a walking character; don't try to "fix" it by reordering physics.

Behaviour is pinned headlessly by `ClothSkeletonAttachTest` (follow + free-part-still-sags + a no-attach
control) and on-screen by `ClothCapeVisualEvidenceTest` (a cape trailing a swinging box, two angles).

## Related: shifting a cloth for a floating-origin rebase is the OPPOSITE lever

For a one-time origin shift (not a per-frame bone weld), do **not** apply the velocity-drive above and
do **not** add `delta` to every vertex — the vertices are COM-relative, so a whole-cloud translation
comes from a single `BodyInterface::SetPosition(GetPosition() + delta)` on the soft body's COM. See
[floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) §1 (issue #613).
