# Follow cameras, character controllers, and who a scene query can actually see

Discovered building the reusable player + camera rig (issue #645). Three of
these bit during that work; the fourth is the design note that made the whole
component set smaller.

## 1. A Jolt `CharacterVirtual`'s inner body carries NO user data — so every UUID-keyed filter silently ignores it

`EntityExclusionBodyFilter::ShouldCollideLocked` resolves a body to an entity
through `JPH::Body::GetUserData()`, and treats `0` as *"unknown body, allow the
collision"*. Rigid bodies get that field stamped at creation
(`JoltBody::CreateBody` → `mUserData`). The character controller's **inner
body** — the one Jolt creates itself from `CharacterVirtualSettings::
mInnerBodyShape`, which exists for character-vs-character collision — did not.

So `RayCastInfo::m_ExcludedEntities` could never exclude a character
controller. Two consequences, both of which look like a different bug:

- A camera spring arm whose pivot sits inside the player capsule collapses to
  its minimum every frame. `RayCastSettings::mTreatConvexAsSolid` defaults on,
  so a ray starting inside a convex shape reports a hit at distance 0 — it reads
  as "the pull-in logic is too aggressive", not as "the exclusion did nothing".
- `PerceptionSystem`'s documented *"the perceiver's own body must not block its
  eyes"* exclusion was a no-op for any character-controlled perceiver.

Fixed by stamping the UUID on the inner body right after
`new JPH::CharacterVirtual(...)`. **The general rule: if you add a body to the
physics world by any path other than `JoltBody`, set its user data, or it is
invisible to every entity-level filter in the engine.** The failure is always
silent — the filter allows, never rejects.

Testing it needs a real physics world; a kernel test of the pull-in math passes
with the bug fully present. The assertion that catches it is "with nothing but
the player in front of the camera, the boom stays at full length".

## 2. A follow camera must run LAST, and a position check will not tell you it doesn't

A camera placed from a pre-physics pose still follows its target — it just
trails by one tick. In steady state (target stationary, or target and camera
both integrated at constant velocity) the offset is *identical* to the correct
one, so a "camera is 4 m behind the player" assertion passes either way.

The seam has to be pinned two ways:

- As **scheduler edges**, not positions: `DependsOn("CameraRig", "PhysicsFence")`,
  `…"PropagateTransforms"`, and against every post-propagate transform writer
  (`Navigation`, `BoidMovement`). The registration-order tie-break satisfies a
  position check with the edges missing — the standing lesson from
  `docs/agent-rules/parallelizable-mover-systems.md`.
- As a **moving-target** functional assertion: tick with the target actually
  walking and assert the offset every frame with a tolerance TIGHTER than one
  tick's travel (at 3 m/s and 1/60 s that is 5 cm, so assert 2 cm).

The mirror-image constraint applies to the input half: it must run **before**
`PhysicsKick`, or the wish velocity it hands the character controller is
integrated a tick late. Same shape as the queue-before-step contract Boat /
Aircraft / Fluid already declare.

## 3. Mouse look is a displacement; movement is a rate

A fixed-step tick runs 0..N times per rendered frame. Anything derived from an
absolute pointer position must therefore be consumed **exactly once per frame**,
not once per tick: read the delta and rebase the stored position, so the second
and later ticks of the same frame see zero, and a frame with no tick at all
leaves the delta outstanding rather than dropping it.

The practical tell is the `dt`: a look delta must never be multiplied by it, a
walk speed always must. Getting this backwards produces a camera whose
sensitivity changes with frame rate — which feels like "the mouse is wrong" and
survives every single-`dt` test.

### Deriving a delta from an absolute position needs a teleport guard

Rebasing on a cursor-mode change is **not** enough. The pointer's window-relative
position also jumps discontinuously when the window is minimized, restored, moved
or resized — and the cursor mode never changes across any of those, so a
`m_HasLastMousePos`-style latch that only resets on a mode change never fires.
The engine cannot help here: `Events/Event.h`'s `WindowFocus` is an unused enum
value with no GLFW callback behind it, so nothing surfaces focus or iconify at
all.

The result is one enormous sample that slams the view into a pitch limit. Bound
it in **degrees of implied rotation** (delta × sensitivity), not pixels, so the
threshold tracks the rig's own sensitivity instead of assuming one; and **drop**
the offending sample rather than clamping it, because for a teleport zero is the
correct answer whereas a clamp still spins the view by the whole bound. Rebase
the stored position even on a rejected sample, or the same oversized delta is
re-derived every tick and the rig latches against the limit forever.

Only the **device** path gets this treatment. A script- or network-driven rig
assigns its look input directly and must be taken at its word — there the value
is an authored displacement, and a replayed input command has to reproduce
exactly what it recorded.

**The same trap hides in `Input::IsKeyJustPressed`.** That flag is snapshotted
once per rendered *frame* (`Input::Update`, called from `Application::Run`), so
inside a fixed-step system it reads true on **every** tick of that frame. A
one-shot action gated on it therefore fires N times per press, where N is the
sub-step count — at 240 Hz on a 60 Hz display, a jump impulse four times over,
i.e. the character leaps higher the faster the machine. A fixed-step consumer
must derive the edge itself from the *level* (`IsKeyPressed` plus a stored
previous state), exactly as it must derive the mouse delta from the absolute
position. Treat every `IsKeyJustPressed` call inside `SimulateRuntimeStep` as
suspect.

The same applies to smoothing. Use `1 - exp(-dt/tau)`, never a fixed per-frame
lerp factor: exponential smoothing composes exactly
(`exp(-dt/2τ)·exp(-dt/2τ) == exp(-dt/τ)`), so one step at `dt` and two at `dt/2`
land in the same place — and that identity is directly assertable, which the
naive version fails.

## 4. Prefer a parameter over a mode enum — and check whether one already subsumes the other

The issue asked for a first-person rig and a third-person follow rig. Modelled
as a mode enum they are two code paths, two sets of valid fields, and a set of
combinations that mean nothing (`Mode = FirstPerson` + `BoomLength = 4`).

But first person *is* third person with a zero-length boom: the camera sits at
the pivot, pitch rotates it in place, and the collision probe has nothing to
shorten. One component, no invalid states, and anything in between (a short
over-the-shoulder boom) is supported for free. The "two templates" then live as
two sets of field values in one preset header, which the shipped prefabs, the
docs and the tests all read — so they cannot drift.

Where two behaviours genuinely differ, express the difference itself rather than
a label: the first/third-person *body* difference is
`m_YawBodyWithLook` vs `m_FaceMoveDirection`, two independent booleans, not a
mode. Pin their mutual exclusivity in a test on the presets — one of them wins
when both are set, so a preset setting both would silently ignore one.

## 5. A two-entity rig cannot be one prefab

A prefab has a single root and instantiates with fresh UUIDs. A camera that
references its target by UUID therefore cannot ship in the same prefab as that
target — the reference would point at the authoring-time UUID, which no longer
exists. Ship the single-entity half as a prefab and the wired pair as a demo
**scene**; say which in the guide rather than shipping a prefab that silently
comes up unwired.
