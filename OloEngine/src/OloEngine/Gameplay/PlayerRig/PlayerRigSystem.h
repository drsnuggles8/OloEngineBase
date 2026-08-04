#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    class Scene;
    struct PlayerRigComponent;
    struct CameraRigComponent;

    // =========================================================================
    // PlayerRigSystem — turns per-tick input intent into character motion
    // (issue #645).
    //
    // ── Where it runs, and why that is the whole design ──────────────────────
    // Registered as the "PlayerRig" node in Scene::GetGameplayScheduler, which
    // means it runs inside SimulateRuntimeStep at the FIXED step, not at the
    // display rate. Scene::RenderRuntime's opt-in fly-camera is the deliberate
    // counter-example and says so: it is a debug viewing aid driven by the
    // variable frame delta, whereas "gameplay cameras moved by scripts/physics
    // live in SimulateRuntimeStep and stay frame-rate-independent".
    //
    // It is ordered BEFORE PhysicsKick (a write-after-read edge on the local
    // transforms channel), which is the seam that makes the same tick's physics
    // step integrate the motion this tick's input asked for. Move it after the
    // fence and every input would land one tick late — the "controls feel
    // laggy in a way no unit test notices" failure the boat/aircraft systems
    // already document.
    //
    // Game-thread pinned: it reads live device input, writes TransformComponent
    // rotations and touches the Jolt character controller. Do NOT mark it
    // .Parallelizable().
    //
    // ── The look delta is a displacement ─────────────────────────────────────
    // A fixed-step tick can run 0..N times per rendered frame. Mouse motion is
    // a displacement, not a rate, so it must be consumed exactly ONCE per frame
    // however many ticks run: the first tick of a frame reads the accumulated
    // delta and rebases m_LastMousePos, so the remaining ticks see zero. A
    // frame with no tick at all loses nothing — the delta simply stays
    // outstanding until the next one. This is why the look delta is never
    // multiplied by dt while the movement speed always is.
    // =========================================================================
    class PlayerRigSystem
    {
      public:
        // Game thread only. Samples device input (per rig, when enabled),
        // integrates the look angles, and drives each rig's character
        // controller — or its TransformComponent when it has none.
        static void Step(Scene* scene, f32 dt);

        // Hand the OS cursor back and clear the "user pressed Escape" latch.
        // Called by Scene::OnRuntimeStop: a rig with m_CaptureCursor locks and
        // hides the pointer for mouse-look, and leaving that in place after
        // Play would strand the editor with an invisible, pinned cursor.
        //
        // Process-global state, because CursorMode is — there is one OS cursor,
        // not one per scene or per rig.
        static void ReleaseCursorCapture();

        // ── Kernels, exposed for unit tests ──────────────────────────────────

        // Fold a raw look delta into `rig`'s yaw/pitch. Yaw wraps into
        // [-180, 180) so a rig can spin indefinitely without the angle growing
        // without bound (and losing f32 precision); pitch clamps to the rig's
        // authored limits. Non-finite input is ignored rather than poisoning
        // the angles.
        static void ApplyLookDelta(PlayerRigComponent& rig, const glm::vec2& lookDelta);

        // Wrap `degrees` into the half-open range [-180, 180) — so exactly one
        // of the two equivalent spellings of "half a turn" survives, and it is
        // -180. Which endpoint is included is arbitrary (they are the same
        // direction); what matters is that callers and tests agree on it.
        [[nodiscard]] static f32 WrapDegrees(f32 degrees);

        // Yaw-only orientation about world +Y for `yawDeg`. The pivot and the
        // movement basis both use this rather than the full look rotation, so
        // looking up or down never tilts the ground plane the player walks on.
        [[nodiscard]] static glm::quat YawRotation(f32 yawDeg);

        // Full look orientation (yaw then pitch), the pose a first-person
        // camera takes and the basis m_MoveRelativeToLook builds from.
        [[nodiscard]] static glm::quat LookRotation(f32 yawDeg, f32 pitchDeg);

        // Horizontal wish direction for `moveInput` (x = strafe, y = forward)
        // in the yaw frame of `yawDeg`. Returns a unit vector, or zero for zero
        // / non-finite input. The input magnitude is clamped to 1 first, so a
        // diagonal is not faster than a straight line while a half-deflected
        // analogue stick still walks at half speed.
        [[nodiscard]] static glm::vec3 WishDirection(const glm::vec2& moveInput, f32 yawDeg);

        // Magnitude of `moveInput`, clamped to 1 (the speed scale that pairs
        // with WishDirection).
        [[nodiscard]] static f32 MoveMagnitude(const glm::vec2& moveInput);

        // Rotate `currentYawDeg` toward `targetYawDeg` by at most
        // `turnRateDeg * dt`, taking the shorter way round. Used by
        // m_FaceMoveDirection.
        [[nodiscard]] static f32 TurnTowardsYaw(f32 currentYawDeg, f32 targetYawDeg, f32 turnRateDeg, f32 dt);
    };

    // =========================================================================
    // CameraRigSystem — places the camera on a collision-aware spring arm
    // (issue #645).
    //
    // Registered as the "CameraRig" node, LAST in the schedule: it must observe
    // the target's FINAL pose for the tick, which means after the physics
    // fence, after PropagateTransforms composed the world matrices, and after
    // the post-propagate transform writers (Navigation, BoidMovement) — a
    // camera that reads a pre-physics pose lags its target by a tick, and that
    // is exactly the judder this rig exists to avoid.
    //
    // Game-thread pinned: writes TransformComponent and issues a Jolt scene
    // query. Not .Parallelizable().
    // =========================================================================
    class CameraRigSystem
    {
      public:
        // Game thread only. Places every CameraRigComponent entity.
        static void Step(Scene* scene, f32 dt);

        // ── Kernels, exposed for unit tests ──────────────────────────────────

        // Advance a spring arm one tick.
        //
        // `allowedLength` is what the collision probe permits right now,
        // `currentLength` the arm as of last tick. Shortening is INSTANT — the
        // frames an eased pull-in would spend inside the wall are the artefact
        // the probe exists to prevent. Lengthening is rate-limited to
        // `returnSpeed` m/s so the camera glides back out instead of popping.
        // A non-positive `returnSpeed` makes both directions instant.
        //
        // Linear in dt, so two half-steps and one full step give the same
        // result exactly.
        [[nodiscard]] static f32 AdvanceBoom(f32 currentLength, f32 allowedLength, f32 returnSpeed, f32 dt);

        // Exponential smoothing toward `target` with time constant
        // `smoothTime` (seconds). A non-positive time constant snaps.
        //
        // Uses 1 - exp(-dt/tau) rather than a fixed per-frame lerp factor
        // precisely so that halving dt and taking two steps lands in the same
        // place: exp(-dt/2tau) * exp(-dt/2tau) == exp(-dt/tau). That identity
        // is what "the camera follows the same at 30 and 144 fps" means, and it
        // is pinned by a test.
        [[nodiscard]] static glm::vec3 SmoothTowards(const glm::vec3& current, const glm::vec3& target,
                                                     f32 smoothTime, f32 dt);

        // Boom length the probe permits given a hit at `hitDistance` along the
        // arm. Backs off by `probeRadius` so the near plane clears the surface,
        // floors the result at `minLength`, and never returns more than
        // `desiredLength`.
        [[nodiscard]] static f32 ClearanceFromHit(f32 hitDistance, f32 desiredLength, f32 probeRadius, f32 minLength);
    };
} // namespace OloEngine
