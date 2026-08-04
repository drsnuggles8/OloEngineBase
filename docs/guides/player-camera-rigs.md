# Player + camera rigs (first-person / third-person follow)

The engine ships a reusable player and camera rig so a new project does not have
to re-invent mouse-look, movement feel, and camera follow in script. Two
components do the whole job:

| Component | Goes on | Owns |
|---|---|---|
| `PlayerRigComponent` | the **player** entity (the one with `CharacterController3DComponent`) | look angles, movement intent, body facing |
| `CameraRigComponent` | the **camera** entity (the one with `CameraComponent`) | pivot, spring arm, collision pull-in, smoothing, head bob |

Both are ordinary serialized components: they show up in the inspector, survive
scene save/load and save-games, are MCP-writable, and are bound to Lua.

---

## First person is a zero-length boom

There is deliberately **no first-person / third-person mode enum**. A
first-person rig is a `CameraRigComponent` whose `BoomLength` is `0`: the camera
sits at the pivot (the eye), pitch rotates it in place, and the collision probe
has nothing to shorten. A third-person rig is the same component with a non-zero
boom.

That means the "two templates" are two sets of field values, not two code paths
— so there is no combination of settings that is invalid, and anything in
between (a very short over-the-shoulder boom) is just as supported.

The canonical values live in one place,
[`Gameplay/PlayerRig/PlayerRigPresets.h`](../../OloEngine/src/OloEngine/Gameplay/PlayerRig/PlayerRigPresets.h):

```cpp
#include "OloEngine/Gameplay/PlayerRig/PlayerRigPresets.h"

player.AddComponent<PlayerRigComponent>(PlayerRigPresets::FirstPersonPlayer());
camera.AddComponent<CameraRigComponent>(PlayerRigPresets::FirstPersonCamera(player.GetUUID()));
```

…and `ThirdPersonPlayer()` / `ThirdPersonCamera()` for the other one. A preset
is a plain value, so compose with it rather than being locked into it:

```cpp
auto rig = PlayerRigPresets::ThirdPersonPlayer();
rig.m_WalkSpeed = 6.0f;
player.AddComponent<PlayerRigComponent>(rig);
```

---

## Demo scenes

Two scenes in `SandboxProject` show the finished result — open one, hit Play,
and copy the two entities into your own scene:

- `Assets/Scenes/PlayerRigFirstPerson.olo` — eye-height camera, head bob, steps
  to walk up and pillars to walk around.
- `Assets/Scenes/PlayerRigThirdPerson.olo` — 4 m boom with collision pull-in, a
  corridor of pillars to walk through (watch the arm shorten and ease back out)
  and a ramp.

There are also two `.oloprefab` files under `Assets/Prefabs/` holding the
**player half** of each rig. The camera half is not in them on purpose: the
camera references its target by UUID, and a prefab instantiates with fresh
UUIDs, so a self-contained two-entity rig prefab would come out pointing at
nothing. Drop the player prefab in, add a camera, and set its `Target`.

---

## Wiring a rig by hand

1. **Player entity** — give it a `CapsuleCollider3DComponent` *first*, then a
   `CharacterController3DComponent` (the collider must exist when the controller
   resolves its shape), then a `PlayerRigComponent`.
2. **Camera entity** — a `CameraComponent` with `Primary` set, plus a
   `CameraRigComponent` whose `Target` is the player's UUID.
3. **The camera must be a ROOT entity, not a child of the player.** The rig
   writes an absolute world pose; parenting it would apply the player's
   transform a second time. (The rig warns once in the log if you do this.)

Jump power comes from `CharacterController3DComponent::JumpPower`, not from the
rig — one number, one owner.

---

## Tuning

### `PlayerRigComponent`

| Field | What it does |
|---|---|
| `LookSensitivity` | degrees of rotation per unit of look input (one pixel of mouse motion, with device input). Also sets the teleport-rejection threshold — see below |
| `InvertLookY` | flips pitch only |
| `MinPitchDeg` / `MaxPitchDeg` | look clamp. Kept inside ±90 so the look direction can never go parallel to world up |
| `WalkSpeed` | ground speed in m/s at full deflection |
| `SprintMultiplier` | speed scale while sprint is held |
| `AirControl` | fraction of `WalkSpeed` still steerable while airborne — **only has an effect if `CharacterController3DComponent::ControlMovementInAir` is set** |
| `MoveRelativeToLook` | move relative to where you're looking (usual) vs. relative to the body's current facing |
| `YawBodyWithLook` | first person: the body turns with the camera |
| `FaceMoveDirection` / `TurnRateDeg` | third person: the body turns toward where it's *walking*. Ignored while `YawBodyWithLook` is set — that one is absolute and would fight it |
| `UseDeviceInput` / `CaptureCursor` | read the live keyboard/mouse, and lock the cursor while doing so |
| `YawDeg` / `PitchDeg` | the live look angles. **Persisted**: a scene authors the starting facing here, and a loaded save restores where the player was looking |

Default device bindings: `WASD` to move, `Shift` to sprint, `Space` to jump,
mouse to look.

**Cursor capture:** with `CaptureCursor` set, the pointer is hidden and locked
while Play is running. **`Escape` releases it; clicking back into the window
re-captures it** — otherwise an editor Play session would pin an invisible
cursor you cannot click your way out of. Leaving Play always hands the cursor
back (`Scene::OnRuntimeStop`).

**Pointer teleports are rejected, not applied.** A device sample implying more
than `PlayerRigSystem::kMaxLookDegreesPerSample` (180°) of rotation is read as
the pointer having jumped rather than the player having flicked, and is dropped.
This covers minimize/restore, and window moves and resizes — all of which move
the cursor's window-relative position without changing the cursor *mode*, so the
first-sample latch never fires for them. Because the bound is in degrees, it
scales with `LookSensitivity`: the same pixel jump is legitimate for a
low-sensitivity rig and a teleport for a high-sensitivity one. Only the device
path is filtered — a script- or network-driven rig's look input is taken as
authored.

### `CameraRigComponent`

| Field | What it does |
|---|---|
| `Target` | UUID of the entity to follow. `0`, or a UUID that doesn't resolve, leaves the camera alone |
| `PivotOffset` | offset from the target's origin in the target's **yaw** frame (x right, y up, z back). Yaw only, so looking up doesn't swing the eye around |
| `BoomLength` | metres behind the pivot. **`0` is first person** |
| `CollisionEnabled` | cast a ray along the boom and shorten it to what fits |
| `ProbeRadius` | keep-out padding, so the near plane clears the surface |
| `MinBoomLength` | floor for the pulled-in arm, so it doesn't collapse into the character's head in a tight corner |
| `BoomReturnSpeed` | how fast (m/s) the arm extends back out once the obstruction clears. Pulling **in** is always instant |
| `PositionSmoothTime` | exponential smoothing time constant, in seconds. `0` is rigid (what first person wants); 0.05–0.15 suits a third-person follow |
| `HeadBobAmplitude` / `HeadBobFrequency` | vertical bob, in metres and cycles **per metre travelled**. `0` amplitude disables it |
| `FallbackPitchDeg` | pitch used when the target has **no** `PlayerRigComponent` — i.e. when following a vehicle or a prop, where yaw comes from the target's own facing |

---

## Driving a rig from script or from the network

`UseDeviceInput` is the switch. With it **cleared**, nothing reads the keyboard
or mouse; instead whoever wants to drive the rig writes its intent fields, and
the rig turns them into character motion at the fixed step. That is the same
path a replay, a network input command, or a headless test uses.

```lua
local rig = entity:GetComponent("PlayerRigComponent")
rig.useDeviceInput = false
rig.moveInput = vec2(0, 1)      -- x = strafe, y = forward, magnitude clamped to 1
rig.lookInput = vec2(dx, dy)    -- a DISPLACEMENT for this tick, not a rate
rig.sprint = false
rig.jump = jumpPressed          -- edge-triggered: the rig consumes it
```

Two things to know:

- **`moveInput` is level-triggered, `lookInput` and `jump` are not.** The rig
  consumes and clears the look delta and the jump request every tick, so an
  external driver must re-assert them each tick. `moveInput` persists until you
  change it.
- **Drive intent, never the transform.** Writing a player's `TransformComponent`
  from script fights the character controller; writing intent does not.

Reading back, `planarSpeed`, `grounded` and `currentBoomLength` are read-only —
they are recomputed every tick, so a write would be silently discarded.

---

## Where the rig runs, and why it matters

The rig is two nodes in the gameplay schedule
(`Scene::GetGameplayScheduler`), not one, and the split is about **ordering**,
not parallelism:

- **`PlayerRig`** runs early — after `Scripts` (so a script-driven rig's intent
  is honoured the same tick) and **before `PhysicsKick`** (so the same tick's
  physics step integrates the wish velocity). If that second edge were lost,
  every input would land a tick late: the character would still move, just
  laggily, which no assertion on the rig's math could see.
- **`CameraRig`** runs **last** — after the physics fence, after the
  world-matrix compose, and after the post-propagate transform writers
  (`Navigation`, `BoidMovement`). A camera placed from a pre-physics pose still
  "follows"; it just trails its target by one tick, which reads as judder.

Both are game-thread pinned (`TransformComponent` writes, live device input, a
Jolt query) and must not be marked `.Parallelizable()`.

This is also why the rig is engine code rather than a script: it runs inside
`SimulateRuntimeStep` at the fixed step, so it is frame-rate independent by
construction. The opt-in fly-camera in `Scene::RenderRuntime` is the deliberate
counter-example — it is a debug viewing aid driven by the variable frame delta,
and it says so in its own comment.

### The look delta is a displacement

A fixed-step tick can run zero or several times per rendered frame. Mouse motion
is a displacement, not a rate, so it is consumed exactly **once** per frame
however many ticks run: the first tick reads the accumulated delta and rebases,
and the rest see zero. A frame with no tick loses nothing — the delta stays
outstanding until the next one. This is why the look delta is never multiplied
by `dt` while the movement speed always is.

---

## Verifying a change to the rig

- `OloEngine-Tests --gtest_filter=PlayerRig*:CameraRig*` — the kernel math
  (look clamping and wrap, movement basis, spring-arm advance, frame-rate
  independence) and the shipped presets.
- `--gtest_filter=*RigDrivesCharacter*:*SpringArm*` — the Functional tests that
  drive a real `Scene::OnUpdateRuntime` with Jolt running: input reaching the
  character in the same tick, the boom shortening against a wall and easing back
  out, and the boom *not* collapsing onto the player's own capsule.
- `--gtest_filter=SystemSchedulerTest.*` — the ordering seams above, asserted as
  dependency edges rather than positions.
- Then actually play one of the two demo scenes. Camera feel is not something a
  test tells you.

---

## Known limits

- The boom probe is a **ray**, not a sphere cast, so a thin obstruction can slip
  between rays on a fast orbit. `ProbeRadius` pads the stop distance but does
  not widen the probe.
- The probe hits anything with a collider, including triggers. If you want the
  camera to ignore a volume, put it on a layer the query mask excludes.
- There is no camera-collision "fade the character out when very close"
  behaviour; `MinBoomLength` just stops the arm short.
- Head bob is vertical only.
