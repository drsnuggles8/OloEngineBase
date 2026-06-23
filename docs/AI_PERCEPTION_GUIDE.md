# AI Sight Perception

OloEngine's AI module lets NPCs *sense* the world so their decision logic
(behaviour trees, FSMs, GOAP, Lua) can react to what they see. The first sense
shipped is **sight**: a forward cone (range + field of view) with an optional
physics line-of-sight check.

| Piece | Where | Role |
|-------|-------|------|
| `PerceptionComponent` | [`AI/AIComponents.h`](../OloEngine/src/OloEngine/AI/AIComponents.h) | the "eyes" — a sight cone on an entity |
| `PerceptibleComponent` | [`AI/AIComponents.h`](../OloEngine/src/OloEngine/AI/AIComponents.h) | a stimulus marker — "this entity can be seen" |
| `PerceptionSystem` | [`AI/Perception/PerceptionSystem.{h,cpp}`](../OloEngine/src/OloEngine/AI/Perception/PerceptionSystem.h) | the per-tick sensor pass |
| `PerceptionMath::IsInSightCone` | [`AI/Perception/PerceptionMath.h`](../OloEngine/src/OloEngine/AI/Perception/PerceptionMath.h) | the pure range + FOV predicate |
| `BTCanSeeTarget` | [`AI/BehaviorTree/BTPerceptionNodes.h`](../OloEngine/src/OloEngine/AI/BehaviorTree/BTPerceptionNodes.h) | behaviour-tree condition/guard |

`PerceptionSystem::OnUpdate` runs from `Scene::OnUpdateRuntime` **immediately
before `AISystem::OnUpdate`**, so the behaviour tree / FSM / GOAP tick consumes
fresh sensor data the same frame.

## Who sees whom

A perceiver only ever considers entities carrying a `PerceptibleComponent`, so
that marker doubles as the candidate set and the team filter:

- `PerceptibleComponent::Team` — faction id. With
  `PerceptionComponent::DetectSameTeam == false` (the default) a perceiver
  ignores perceptibles whose `Team` equals its own `PerceiverTeam` (allies).
- `PerceptibleComponent::IsPerceptible` — toggle off for stealth / cloaking
  without removing the component.

## The sight cone

`PerceptionComponent` authors the cone:

| Field | Default | Meaning |
|-------|---------|---------|
| `SightRange` | `15` | max distance a target can be seen (metres) |
| `FovDegrees` | `90` | **full** angular width of the cone |
| `EyeOffset` | `(0, 1.7, 0)` | local-space eye position (eye height) |
| `RequireLineOfSight` | `true` | reject targets occluded by physics geometry |
| `PerceiverTeam` | `0` | this sensor's faction id |
| `DetectSameTeam` | `false` | also notice same-team perceptibles |

A target is **in the cone** iff it is within `SightRange` of the eye AND the
angle between the look direction (the entity's local **-Z**, engine forward
convention) and the direction to the target is at most `FovDegrees / 2`. This is
[`PerceptionMath::IsInSightCone`](../OloEngine/src/OloEngine/AI/Perception/PerceptionMath.h),
a pure header function shared by the system and its unit tests:

```cpp
in-cone  <=>  dot(normalize(forward), normalize(target - eye))
                  >= cos(radians(fovDegrees / 2))
```

When `RequireLineOfSight` is set, a candidate that passes range + FOV is then
ray-cast from the eye to the target through `Scene::GetPhysicsScene()`
(Jolt). The perceiver and the target are excluded from the ray, so only a
*third* body in between counts as an occluder. With no live physics scene the
line is treated as clear (sight degrades to see-through rather than blind),
which is the useful default in headless / editor-stopped contexts.

The nearest passing target wins.

## Reading the result

Each tick `PerceptionSystem` writes the runtime result onto the
`PerceptionComponent` (not serialized — recomputed every frame):

- `HasVisibleTarget` (bool), `VisibleTarget` (UUID of the nearest seen target)
- `LastKnownPosition` / `HasLastKnownPosition` — sticky memory of where a target
  was last seen, retained after it leaves view
- `TimeSinceLastSeen` — seconds since a target was last visible (alertness signal)

It also **mirrors** the result into whichever AI blackboards the entity carries
(`BehaviorTreeComponent` / `StateMachineComponent` / `GoapAgentComponent`) under
the keys in
[`PerceptionKeys`](../OloEngine/src/OloEngine/AI/Perception/PerceptionSystem.h):
`Perception.CanSeeTarget` (bool), `Perception.Target` (UUID),
`Perception.LastKnownPosition` (vec3). FSM transition predicates, GOAP sensors
and Lua scripts read those.

### Behaviour tree

`BTCanSeeTarget` reads the component directly. With a child it is a **guard**
(the child ticks only while a target is in sight — patrol → chase); with no
child it is a leaf condition under a `Sequence`/`Selector`. It is registered as
`"CanSeeTarget"` in `AIRegistry`, so authored trees can use it by name.

### Lua

Both components are script-exposed. The authored sight config is read/write;
the per-tick result is read-only:

```lua
local p = entity:GetComponent("PerceptionComponent")
if p.hasVisibleTarget then
    -- p.visibleTarget (u64 UUID), p.lastKnownPosition (vec3)
end
```

## Tests

- [`PerceptionMathTest.cpp`](../OloEngine/tests/AI/PerceptionMathTest.cpp) —
  unit test pinning the range + FOV cone predicate (boundaries, behind,
  narrow/full FOV, eye offset).
- [`PerceptionDetectsTargetViaSceneTickTest.cpp`](../OloEngine/tests/Functional/AI/PerceptionDetectsTargetViaSceneTickTest.cpp)
  — Functional test driving real `Scene::OnUpdateRuntime`: in-cone, out-of-range,
  behind, team filter, cloak, last-known-position memory, and a `BTCanSeeTarget`
  tree reacting.
- [`PerceptionLineOfSightBlockedByWallViaSceneTickTest.cpp`](../OloEngine/tests/Functional/AI/PerceptionLineOfSightBlockedByWallViaSceneTickTest.cpp)
  — Functional test of the perception↔Physics3D LOS seam (a wall blocks sight;
  disabling `RequireLineOfSight` on the same geometry restores it).

## Scope / follow-ups

Only **sight** ships today. Natural extensions: hearing / stimulus events,
peripheral-vision falloff, a perception *memory* decay curve, and behaviour-tree
*Services* that poll perception on an interval.
