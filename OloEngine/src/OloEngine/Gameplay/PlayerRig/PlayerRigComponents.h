#pragma once

// =============================================================================
// Reusable player + camera rig components (issue #645).
//
// The engine already shipped every primitive a player needs — Jolt's
// CharacterVirtual behind CharacterController3DComponent, CameraComponent,
// scene queries — but no ready-made rig, so every project re-wrote mouse-look,
// movement feel and camera follow in script. These two components are that rig,
// as engine data rather than per-game boilerplate.
//
// ── Why components and not "just a script template" ──────────────────────────
// The collision-aware boom needs a physics raycast every tick, and the whole
// rig must run inside SimulateRuntimeStep at the fixed step (see the comment on
// the display-rate fly-cam in Scene::RenderRuntime: "Gameplay cameras moved by
// scripts/physics live in SimulateRuntimeStep and stay frame-rate-independent").
// Re-implementing that per game in script is exactly the duplication this issue
// exists to remove. Shipping it as components also makes the tuning surface
// serialized, designer-editable in the inspector, MCP-writable, and save-game
// round-tripping for free.
//
// ── First person is a zero-length boom ───────────────────────────────────────
// There is deliberately NO first-person/third-person mode enum. A first-person
// rig is a CameraRigComponent whose m_BoomLength is 0: the camera sits at the
// pivot (the eye), pitch rotates it in place, and the collision probe has
// nothing to shorten. A third-person rig is the same component with a non-zero
// boom. One data model, no invalid combination, and the "two templates" the
// issue asks for are two sets of field values — see PlayerRigPresets.h and the
// two .oloprefab assets in SandboxProject.
// =============================================================================

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/ComponentReflection.h"
#include "OloEngine/Scene/EntityFacing.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // =========================================================================
    // PlayerRigComponent — put this on the PLAYER entity (the one carrying the
    // CharacterController3DComponent).
    //
    // Owns the look angles for the whole rig. Yaw/pitch live here, not on the
    // camera, because they drive BOTH the camera placement and the movement
    // basis ("W walks where I'm looking"), and a single owner is the only way
    // the two can't disagree. CameraRigComponent reads them off its target.
    //
    // ── Input is data, not a device read ─────────────────────────────────────
    // The per-tick intent (m_MoveInput / m_LookInput / m_SprintInput /
    // m_JumpInput) is component state. With m_UseDeviceInput set, PlayerRigSystem
    // fills it from the keyboard/mouse at the top of the tick; with it cleared,
    // whoever wants to drive the rig writes those fields instead — a script, a
    // replay, a network input command, or a headless test. That split is what
    // makes the rig testable without a window, and it is the same shape the
    // networking loop needs (docs/agent-rules/server-authoritative-networking-loop.md:
    // "input commands must be displacements not direction+speed").
    // =========================================================================
    struct PlayerRigComponent
    {
        // ── Look ─────────────────────────────────────────────────────────────
        // Degrees of rotation per unit of m_LookInput. With device input that
        // unit is one pixel of mouse motion, so 0.15 ≈ 400 px per 60° turn.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 10.0f)
        f32 m_LookSensitivity = 0.15f;
        OLO_PROPERTY()
        bool m_InvertLookY = false;
        // Pitch limits in degrees (negative = looking down). Kept just inside
        // ±90 so the look direction can never become parallel to the world up
        // axis, which would make the yaw basis degenerate.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = -89.9f, Max = 89.9f)
        f32 m_MinPitchDeg = -80.0f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = -89.9f, Max = 89.9f)
        f32 m_MaxPitchDeg = 80.0f;

        // ── Movement ─────────────────────────────────────────────────────────
        // Ground speed in m/s at full stick / WASD deflection.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
        f32 m_WalkSpeed = 4.5f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 1.0f, Max = 100.0f)
        f32 m_SprintMultiplier = 1.8f;
        // Fraction of m_WalkSpeed still steerable while airborne. Only has any
        // effect when CharacterController3DComponent::m_ControlMovementInAir is
        // set — the controller discards the desired velocity outright otherwise
        // (JoltCharacterController::ApplyGravityAndJump), which is the engine's
        // existing "no air control" behaviour and not something this rig
        // overrides.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 m_AirControl = 0.35f;

        // Move relative to where the player is LOOKING (the usual first- and
        // third-person feel) rather than relative to the body's current facing.
        OLO_PROPERTY()
        bool m_MoveRelativeToLook = true;

        // ── Body facing ──────────────────────────────────────────────────────
        // First-person: the body yaws with the look, so the capsule (and
        // anything parented to it) always faces the camera direction.
        OLO_PROPERTY()
        bool m_YawBodyWithLook = true;
        // Third-person: the body turns to face the direction it is moving,
        // independently of where the camera is pointed. Ignored while
        // m_YawBodyWithLook is set — that one is absolute and would fight it.
        OLO_PROPERTY()
        bool m_FaceMoveDirection = false;
        // Turn rate for m_FaceMoveDirection, degrees per second.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 3600.0f)
        f32 m_TurnRateDeg = 720.0f;

        // ── Input source ─────────────────────────────────────────────────────
        // Sample the live keyboard/mouse into the intent fields below. Clear it
        // to drive the rig from a script / network / test instead.
        OLO_PROPERTY()
        bool m_UseDeviceInput = true;
        // Lock + hide the OS cursor while the rig is live (the FPS mode). A
        // no-op with no window, so headless hosts and tests are unaffected.
        OLO_PROPERTY()
        bool m_CaptureCursor = true;

        // ── Look state ───────────────────────────────────────────────────────
        // Persisted, not Skip-tagged: a scene authors the initial look
        // direction here, and reloading a save must restore where the player
        // was looking. Degrees; yaw is about +Y, pitch is positive looking up.
        OLO_PROPERTY()
        f32 m_YawDeg = 0.0f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = -89.9f, Max = 89.9f)
        f32 m_PitchDeg = 0.0f;

        // ── Per-tick intent (runtime; never persisted) ───────────────────────
        // x = strafe (+right), y = forward (+forward). Magnitude is clamped to
        // 1 so a diagonal isn't faster than a straight line.
        OLO_PROPERTY()
        OLO_SERIALIZE(Skip)
        glm::vec2 m_MoveInput = { 0.0f, 0.0f };
        // Raw look delta for THIS tick, in the units m_LookSensitivity scales.
        // A displacement, not a rate — never multiply it by dt.
        OLO_PROPERTY()
        OLO_SERIALIZE(Skip)
        glm::vec2 m_LookInput = { 0.0f, 0.0f };
        OLO_PROPERTY()
        OLO_SERIALIZE(Skip)
        bool m_SprintInput = false;
        // Edge-triggered: the system consumes and clears it, so a script can
        // set it once without holding it.
        OLO_PROPERTY()
        OLO_SERIALIZE(Skip)
        bool m_JumpInput = false;

        // ── Derived runtime state (read-only for scripts) ────────────────────
        // Previous absolute mouse position, for the device-input delta. The
        // "has" flag exists so the first tick can't produce a giant delta from
        // a zero-initialised previous position.
        OLO_SERIALIZE(Skip)
        glm::vec2 m_LastMousePos = { 0.0f, 0.0f };
        OLO_SERIALIZE(Skip)
        bool m_HasLastMousePos = false;
        // Previous sampled state of the jump key, so the device path derives
        // the press EDGE itself rather than trusting Input::IsKeyJustPressed.
        // That flag is snapshotted once per rendered FRAME, so it reads true on
        // every fixed tick of that frame — at a high frame rate the rig would
        // request several jumps per press and the character would leap higher
        // the faster the machine. Deriving the edge from the level makes it
        // exactly one jump per press at any tick rate.
        OLO_SERIALIZE(Skip)
        bool m_JumpKeyWasDown = false;
        // Horizontal speed the rig commanded this tick (m/s). CameraRigSystem
        // drives head-bob off the target's actual travel, not off this, so this
        // is purely introspection for scripts/animation.
        //
        // Deliberately NOT OLO_PROPERTY: the generator has no getter-only mode,
        // so annotating these would emit a public C# SETTER for a value
        // PlayerRigSystem overwrites every tick — a write that silently does
        // nothing. Same treatment as BoidComponent's m_SteeringForce /
        // m_NeighborCount, which are read-only introspection in exactly the same
        // way. Lua exposes them properly via sol::readonly.
        OLO_SERIALIZE(Skip)
        f32 m_PlanarSpeed = 0.0f;
        OLO_SERIALIZE(Skip)
        bool m_Grounded = false;

        PlayerRigComponent() = default;
        PlayerRigComponent(const PlayerRigComponent&) = default;

        // Authored fields only. The editor's undo tracking snapshots a
        // component on the idle→edit transition and compares it FRAMES LATER;
        // a whole-struct compare would see the per-tick intent fields churn on
        // every frame of a running simulation, latch the is-editing flag on and
        // never push the undo entry (same lesson as BoidComponent /
        // WeatherStateComponent). PreferValueComparison in SceneHierarchyPanel
        // opts this component onto this operator. It also keeps the editor off
        // the byte-level memcmp path, which would compare the padding bytes
        // around the bool members (SonarCloud cpp:S5000).
        //
        // m_YawDeg / m_PitchDeg ARE compared — they are authored state the
        // inspector exposes, and the rig systems run only under
        // SimulateRuntimeStep, never in edit mode where undo matters.
        auto operator==(const PlayerRigComponent& other) const -> bool
        {
            const bool sameLook = Math::BitwiseEqual(m_LookSensitivity, other.m_LookSensitivity) &&
                                  (m_InvertLookY == other.m_InvertLookY) &&
                                  Math::BitwiseEqual(m_MinPitchDeg, other.m_MinPitchDeg) &&
                                  Math::BitwiseEqual(m_MaxPitchDeg, other.m_MaxPitchDeg);
            const bool sameMovement = Math::BitwiseEqual(m_WalkSpeed, other.m_WalkSpeed) &&
                                      Math::BitwiseEqual(m_SprintMultiplier, other.m_SprintMultiplier) &&
                                      Math::BitwiseEqual(m_AirControl, other.m_AirControl) &&
                                      (m_MoveRelativeToLook == other.m_MoveRelativeToLook);
            const bool sameFacing = (m_YawBodyWithLook == other.m_YawBodyWithLook) &&
                                    (m_FaceMoveDirection == other.m_FaceMoveDirection) &&
                                    Math::BitwiseEqual(m_TurnRateDeg, other.m_TurnRateDeg);
            const bool sameInputSource = (m_UseDeviceInput == other.m_UseDeviceInput) &&
                                         (m_CaptureCursor == other.m_CaptureCursor);
            const bool sameAngles = Math::BitwiseEqual(m_YawDeg, other.m_YawDeg) &&
                                    Math::BitwiseEqual(m_PitchDeg, other.m_PitchDeg);
            return sameLook && sameMovement && sameFacing && sameInputSource && sameAngles;
        }
    };

    // =========================================================================
    // CameraRigComponent — put this on the CAMERA entity (the one carrying the
    // CameraComponent), and point m_Target at the player.
    //
    // A spring arm: the camera hangs a boom behind a pivot attached to the
    // target, and a physics ray from the pivot outwards shortens the boom so
    // the camera never ends up inside geometry. m_BoomLength == 0 collapses the
    // whole thing to a first-person eye at the pivot.
    //
    // The camera entity should be a ROOT entity, not a child of the player: the
    // rig writes an absolute world pose, and parenting it would compose that
    // pose with the player's transform twice.
    // =========================================================================
    struct CameraRigComponent
    {
        // Entity to follow. 0 disables the rig (the camera keeps its authored
        // transform), which is also what happens if the UUID doesn't resolve.
        //
        // No OLO_PROPERTY: UUID is not a type the C# binding generator emits, so
        // the annotation would be dead. That matches every other entity
        // reference in the engine — IKTargetComponent::AimTargetEntity,
        // PhysicsJoint3DComponent::m_ConnectedEntity, ClothComponent::
        // m_AttachmentEntity and friends are all editor + serializer only. Lua
        // does expose it, as an explicit u64 property (see LuaScriptGlue).
        UUID m_Target = 0;

        // Pivot offset from the target's origin, in the target's YAW frame
        // (x = right, y = up, z = backwards). Yaw only, deliberately: pitching
        // or rolling the pivot with the body would swing the eye around when
        // the player looks up.
        //
        // First person: roughly (0, eye height, 0).
        // Third person: a shoulder offset, e.g. (0.5, 1.6, 0).
        OLO_PROPERTY()
        glm::vec3 m_PivotOffset = { 0.0f, 1.7f, 0.0f };

        // Boom length in metres. 0 == first person (camera sits at the pivot).
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
        f32 m_BoomLength = 0.0f;

        // ── Collision-aware pull-in ──────────────────────────────────────────
        OLO_PROPERTY()
        bool m_CollisionEnabled = true;
        // Keep-out padding: the camera stops this far short of whatever the
        // probe hit, so the near plane doesn't clip into the surface.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 10.0f)
        f32 m_ProbeRadius = 0.25f;
        // Floor for the pulled-in boom. Stops the camera collapsing all the way
        // onto the pivot (and into the player's own head) in a tight corner.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
        f32 m_MinBoomLength = 0.4f;
        // Rate (m/s) at which the boom extends back out once the obstruction
        // clears. Pulling IN is always instantaneous — easing in would let the
        // camera spend those frames inside the wall, which is the one artefact
        // the whole mechanism exists to prevent. 0 makes extension instant too.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
        f32 m_BoomReturnSpeed = 6.0f;

        // ── Smoothing ────────────────────────────────────────────────────────
        // Exponential position smoothing time constant, in seconds. 0 is rigid,
        // which is what first person wants; a third-person follow usually wants
        // 0.05–0.15. Exponential rather than lerp-per-frame so the result is
        // frame-rate independent by construction: two half-steps compose to
        // exactly the same blend as one full step.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 10.0f)
        f32 m_PositionSmoothTime = 0.0f;

        // ── Head bob ─────────────────────────────────────────────────────────
        // Vertical bob amplitude in metres; 0 disables it. Phase advances with
        // DISTANCE TRAVELLED, not with time, so the bob stays locked to the
        // stride whatever the frame rate or the walk speed.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 m_HeadBobAmplitude = 0.0f;
        // Bob cycles per metre travelled.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 50.0f)
        f32 m_HeadBobFrequency = 1.1f;

        // Pitch used when the target has no PlayerRigComponent to supply look
        // angles — i.e. when this rig is following something that isn't a
        // player (a vehicle, a prop). Yaw then comes from the target's own
        // facing. Ignored whenever the target does have a PlayerRigComponent.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = -89.9f, Max = 89.9f)
        f32 m_FallbackPitchDeg = -12.0f;

        // Which local axis the TARGET treats as forward, for the same
        // no-PlayerRigComponent case as m_FallbackPitchDeg. Leave it on Auto:
        // EntityFacing::ConventionFor reads the answer off the target's own
        // components, so a rig aimed straight at a boat / car / aircraft picks
        // up the +Z convention with nothing authored (issue #897).
        //
        // The override exists for a target whose components cannot say — the
        // usual case being a proxy transform that carries a vehicle's heading
        // (smoothed, or with a look-astern offset) but has no vehicle component
        // of its own. Before #897 such a proxy had to carry the heading turned
        // through 180 degrees instead, which is a workaround that reads as a
        // scene-authoring mistake.
        //
        // No OLO_PROPERTY: the C# generator does not emit enum properties, so
        // the annotation would be dead — same reasoning as m_Target above. Lua
        // exposes it as an integer property (see LuaScriptGlue).
        //
        // Reject rather than Clamp, for the reason VehicleComponent::m_DriveMode
        // is: this is a discriminated value, and saturating a corrupt 7 to
        // PlusZ would silently pick a DIFFERENT valid convention instead of
        // falling back to the constructor default.
        OLO_SERIALIZE(Reject, Min = 0, Max = 2)
        ForwardConvention m_TargetForward = ForwardConvention::Auto;

        // ── Runtime (recomputed every tick, never persisted) ─────────────────
        // Current (possibly pulled-in) boom length. Read-only introspection —
        // not OLO_PROPERTY, for the same reason as PlayerRigComponent's
        // m_PlanarSpeed: CameraRigSystem rewrites it every tick, so a generated
        // C# setter would be a write that silently does nothing.
        OLO_SERIALIZE(Skip)
        f32 m_CurrentBoomLength = 0.0f;
        OLO_SERIALIZE(Skip)
        glm::vec3 m_SmoothedPosition = { 0.0f, 0.0f, 0.0f };
        OLO_SERIALIZE(Skip)
        f32 m_BobPhase = 0.0f;
        OLO_SERIALIZE(Skip)
        glm::vec3 m_PrevTargetPosition = { 0.0f, 0.0f, 0.0f };
        // False until the rig has placed the camera once. The first tick must
        // adopt the computed pose outright rather than easing toward it from a
        // zero-initialised previous state — otherwise the camera flies in from
        // the world origin, and the boom "extends" from 0 at m_BoomReturnSpeed.
        OLO_SERIALIZE(Skip)
        bool m_Initialized = false;

        CameraRigComponent() = default;
        CameraRigComponent(const CameraRigComponent&) = default;

        // Authored fields only — same reasoning as PlayerRigComponent::operator==.
        auto operator==(const CameraRigComponent& other) const -> bool
        {
            const bool sameTarget = (m_Target == other.m_Target) &&
                                    Math::BitwiseEqual(m_PivotOffset, other.m_PivotOffset) &&
                                    Math::BitwiseEqual(m_BoomLength, other.m_BoomLength);
            const bool sameCollision = (m_CollisionEnabled == other.m_CollisionEnabled) &&
                                       Math::BitwiseEqual(m_ProbeRadius, other.m_ProbeRadius) &&
                                       Math::BitwiseEqual(m_MinBoomLength, other.m_MinBoomLength) &&
                                       Math::BitwiseEqual(m_BoomReturnSpeed, other.m_BoomReturnSpeed);
            const bool sameFeel = Math::BitwiseEqual(m_PositionSmoothTime, other.m_PositionSmoothTime) &&
                                  Math::BitwiseEqual(m_HeadBobAmplitude, other.m_HeadBobAmplitude) &&
                                  Math::BitwiseEqual(m_HeadBobFrequency, other.m_HeadBobFrequency) &&
                                  Math::BitwiseEqual(m_FallbackPitchDeg, other.m_FallbackPitchDeg) &&
                                  (m_TargetForward == other.m_TargetForward);
            return sameTarget && sameCollision && sameFeel;
        }
    };
} // namespace OloEngine
