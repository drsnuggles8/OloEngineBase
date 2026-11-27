# A world-space consumer takes a `WorldPose`, never a raw vec3/quat

Rules for code that places something in the world from an entity's transform: a physics body, an
emitter, a light, a probe, a navigation agent. Types: [`Scene/Pose.h`](../../OloEngine/src/OloEngine/Scene/Pose.h).
Contract test: `WorldPoseContractTest` ([source](../../OloEngine/tests/Scene/WorldPoseContractTest.cpp)).
Issue #1797.

## Rules

1. **`TransformComponent` is parent-local.** `Translation`, `GetRotation()`, `Scale` and
   `GetTransform()` are the entity's pose in its parent's frame. They equal the world pose only for a
   root. A test with only root entities cannot tell the two apart, which is how #1714 (bodies,
   emitters, probes, fog, streaming volumes) and #1707 (lights) shipped with green suites.
2. **A new world-space placement API takes `const WorldPose&`.** `WorldPose` is not an aggregate
   and does not convert from `LocalPose`, a `glm::mat4` or loose vectors, so
   `Place(tc.Translation, tc.GetRotation())` and `Place({ tc.Translation, ... })` do not compile.
   The migrated boundary is physics placement: `JoltBody::CreateBodySettings` and
   `Scene::SetPhysicsWorldPose`.
3. **Get a `WorldPose` from `Scene::ComputeWorldTRS(entity)`.** Only `Scene` constructs one from an
   entity. A solver that simulates in world space (a Jolt body or character controller) vouches for
   its own readback with `WorldPose::FromSimulation`. `WorldPoseContractTest` pins the files that
   call it: a new caller must read from a world-space source and add itself there with the reason.
4. **A pose expressed in the parent frame is a `LocalPose`.** The world-to-local writeback
   helpers in `Scene.cpp` (`ToLocalPhysicsPose`, `ToLocalBox2DPose`) return one. Before #1797 they
   returned the world type while holding a parent-local position.
5. **Choose `Compute*` or `Get*` by when the code runs.**

   | API | Reads | Valid |
   |---|---|---|
   | `ComputeWorldTRS`, `ComputeWorldTransform`, `ComputeWorldPosition` | the parent chain, composed now | always, including an entity created or reparented this tick |
   | `GetWorldTransform`, `GetWorldPosition` | `WorldTransformComponent`, written by `PropagateWorldTransforms` | after this tick's propagation pass; before the first pass they return the **local** value |

   Placement at spawn or between ticks (body creation, script teleports, editor tools) uses
   `Compute*`. Per-frame render and submission loops that run after propagation use `Get*`.
6. **Know what a `WorldPose` cannot hold.** Position is the composed translation, exact under any
   affine parent. Rotation is the composed basis made orthonormal: X kept, Y orthogonalised,
   Z = X x Y. Scale is the column lengths, with Z negated for a mirror. Under shear (a nonuniform
   parent above a rotated child) the rotation and scale are an approximation, so a consumer that
   needs the exact frame uses `ComputeWorldTransform`. Under a parent that collapses one axis, the
   two surviving axes still define the rotation. When fewer survive there is no world rotation,
   and `ComputeWorldTRS` keeps the authored local rotation.
7. **Writeback under a singular parent leaves the local pose alone.** `TrySetWorldPosition` /
   `TrySetWorldRotation` return `false`, and `SetPhysicsWorldPose` logs once through
   `ReportPhysicsWriteback`. Physics keeps world authority until the frame recovers (#1778).
8. **Do not add a second propagation or cache.** `PropagateWorldTransforms` is the one hierarchy
   sweep. `WorldPose` is a 40-byte, trivially copyable value with the same layout as the struct it
   replaced, so the boundary adds no runtime work.

## Why a strong type, and what it does not cover

Three mechanisms were compared on the physics boundary:

- **`LocalPose` / `WorldPose` types (chosen).** The swap fails at compile time at the call site,
  and `static_assert` traits in a unit test keep it failing. The cost is one header and a changed
  signature on two functions. Existing readers of `ComputeWorldTRS(...).Position` are unchanged.
- **Named transform functions** (`TransformPoint`, `TransformNormal`). These fix point versus
  normal confusion, not local versus world: both arguments are still `glm::vec3`.
- **A static rule** (a clang-query or pre-commit regex for `.Translation` passed into a placement
  call). It needs the clang tooling in CI. A regex misses an alias (`const auto& p = tc.Translation;`)
  and flags root-only code that is correct.

The type does not stop someone from assigning a field of an existing pose
(`pose.Position = tc.Translation`). The fields stay public so the existing readers compile unchanged.
No recorded bug took that form: every one passed the wrong source into a placement call. The other
`glm::mat4` consumers (render submission, audio, probes, lights) were fixed per subsystem in #1771
and #1806 and are not migrated. Migrate one when it next changes.
