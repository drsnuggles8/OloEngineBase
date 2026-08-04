#include "OloEnginePCH.h"

#include "OloEngine/Gameplay/PlayerRig/PlayerRigSystem.h"

#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace OloEngine
{
    namespace
    {
        constexpr glm::vec3 kWorldUp{ 0.0f, 1.0f, 0.0f };
        // Engine convention: -Z is forward, +X is right (matches the fly-camera
        // basis in Scene::RenderRuntime and glm::quatLookAt in FlockingSystem).
        constexpr glm::vec3 kLocalForward{ 0.0f, 0.0f, -1.0f };
        constexpr glm::vec3 kLocalRight{ 1.0f, 0.0f, 0.0f };
        constexpr glm::vec3 kLocalBackward{ 0.0f, 0.0f, 1.0f };

        // Below this a direction is meaningless and normalizing produces NaN.
        constexpr f32 kDegenerateLengthSq = 1.0e-12f;

        // Escape releases the captured cursor; clicking back into the window
        // re-captures it. Without this an editor Play session with a
        // capturing rig would pin an invisible pointer with no way out, which
        // is exactly the state you cannot click your way out of.
        //
        // Process-global rather than per-scene or per-rig, because CursorMode
        // is: there is one OS cursor. Scene::OnRuntimeStop clears it via
        // ReleaseCursorCapture().
        bool s_CursorReleasedByUser = false;

        // Sample the keyboard/mouse into a rig's per-tick intent fields.
        //
        // Only ever called on the FIRST tick of a frame in practice, because
        // the mouse delta rebases m_LastMousePos: a second tick in the same
        // frame reads the same absolute position and therefore a zero delta.
        // The key states are level-triggered and legitimately re-read (holding
        // W for two ticks should move twice as far).
        void SampleDeviceInput(PlayerRigComponent& rig)
        {
            glm::vec2 move{ 0.0f, 0.0f };
            if (Input::IsKeyPressed(Key::W))
                move.y += 1.0f;
            if (Input::IsKeyPressed(Key::S))
                move.y -= 1.0f;
            if (Input::IsKeyPressed(Key::D))
                move.x += 1.0f;
            if (Input::IsKeyPressed(Key::A))
                move.x -= 1.0f;
            rig.m_MoveInput = move;

            rig.m_SprintInput = Input::IsKeyPressed(Key::LeftShift);

            // Derive the jump EDGE from the key level rather than using
            // Input::IsKeyJustPressed. That flag is snapshotted once per
            // rendered frame, so it stays true across every fixed tick of that
            // frame: at 240 Hz on a 60 Hz display it would request four jumps
            // per press, and the character would leap higher the faster the
            // machine. Tracking the previous sampled level per rig makes it
            // exactly one jump per press at any tick rate.
            //
            // OR rather than assign: a script may have requested a jump this
            // tick too, and the device read must not silently cancel it.
            const bool jumpKeyDown = Input::IsKeyPressed(Key::Space);
            rig.m_JumpInput = rig.m_JumpInput || (jumpKeyDown && !rig.m_JumpKeyWasDown);
            rig.m_JumpKeyWasDown = jumpKeyDown;

            const glm::vec2 mouse = Input::GetMousePosition();
            if (!Math::IsFinite(mouse))
            {
                rig.m_LookInput = glm::vec2(0.0f);
                return;
            }

            // First sample after enabling the rig (or after a cursor-mode
            // change teleported the pointer) must not be interpreted as a
            // several-hundred-pixel flick. SampleLookDelta also rejects the
            // discontinuity m_HasLastMousePos cannot see — a window
            // minimize/restore/move, which jumps the pointer's window-relative
            // position without changing the cursor mode.
            rig.m_LookInput = PlayerRigSystem::SampleLookDelta(rig.m_LastMousePos, mouse, rig.m_LookSensitivity,
                                                               rig.m_HasLastMousePos);

            // Rebase unconditionally, INCLUDING on a rejected sample: the new
            // position is where the pointer now is, so the next tick measures
            // from there and resumes normally. Keeping the stale base would
            // re-derive the same oversized delta every tick and latch the rig
            // against a pitch limit.
            rig.m_LastMousePos = mouse;
            rig.m_HasLastMousePos = true;
        }

        // World-space translation of an entity's composed transform. Falls back
        // to the local translation when the entity has no world matrix yet,
        // which is what Scene::GetWorldTransform already does internally.
        [[nodiscard]] glm::vec3 WorldPosition(const Scene& scene, entt::entity entity)
        {
            const glm::mat4 world = scene.GetWorldTransform(entity);
            return glm::vec3(world[3]);
        }

        // Yaw (degrees about world +Y) of an orientation, defined as the exact
        // inverse of PlayerRigSystem::YawRotation.
        //
        // Deliberately not glm::yaw(): that lives in GTX (experimental) and
        // decomposes via Euler angles with its own axis-order convention, which
        // is a second, silently-different definition of "yaw" sitting next to
        // this file's. Recovering it from the rotated forward vector can only
        // agree with YawRotation by construction.
        [[nodiscard]] f32 YawDegreesFromRotation(const glm::quat& rotation)
        {
            // YawRotation(t) maps -Z to (-sin t, 0, -cos t), so atan2(-x, -z)
            // is its inverse.
            const glm::vec3 forward = rotation * kLocalForward;
            if (!Math::IsFinite(forward))
                return 0.0f;
            const f32 planarLengthSq = forward.x * forward.x + forward.z * forward.z;
            if (planarLengthSq <= kDegenerateLengthSq)
                return 0.0f; // looking straight up/down — yaw is undefined
            return glm::degrees(std::atan2(-forward.x, -forward.z));
        }
    } // namespace

    // =========================================================================
    // PlayerRigSystem
    // =========================================================================

    f32 PlayerRigSystem::WrapDegrees(f32 degrees)
    {
        if (!std::isfinite(degrees))
            return 0.0f;

        degrees = std::fmod(degrees + 180.0f, 360.0f);
        if (degrees < 0.0f)
            degrees += 360.0f;
        return degrees - 180.0f;
    }

    glm::quat PlayerRigSystem::YawRotation(f32 yawDeg)
    {
        return glm::angleAxis(glm::radians(std::isfinite(yawDeg) ? yawDeg : 0.0f), kWorldUp);
    }

    glm::quat PlayerRigSystem::LookRotation(f32 yawDeg, f32 pitchDeg)
    {
        const f32 pitch = std::isfinite(pitchDeg) ? pitchDeg : 0.0f;
        return YawRotation(yawDeg) * glm::angleAxis(glm::radians(pitch), kLocalRight);
    }

    glm::vec2 PlayerRigSystem::SampleLookDelta(const glm::vec2& previous, const glm::vec2& current,
                                               const f32 lookSensitivity, const bool hasPrevious)
    {
        if (!hasPrevious || !Math::IsFinite(previous) || !Math::IsFinite(current))
            return glm::vec2(0.0f);

        const glm::vec2 delta = current - previous;
        if (!Math::IsFinite(delta))
            return glm::vec2(0.0f);

        // Measure the jump in the units the limit is expressed in — degrees of
        // resulting rotation, not pixels — so the guard tracks the rig's own
        // sensitivity instead of assuming one. A rig with a zero or non-finite
        // sensitivity rotates by nothing anyway, so nothing needs rejecting
        // (and ApplyLookDelta drops the non-finite case outright).
        const f32 sensitivity = std::abs(lookSensitivity);
        if (!std::isfinite(sensitivity) || sensitivity <= 0.0f)
            return delta;

        // Compare squared, against a squared bound, to keep the hot path free
        // of a sqrt: length(delta) * sensitivity > limit.
        const f32 maxPixels = kMaxLookDegreesPerSample / sensitivity;
        if (glm::dot(delta, delta) > (maxPixels * maxPixels))
            return glm::vec2(0.0f);

        return delta;
    }

    void PlayerRigSystem::ApplyLookDelta(PlayerRigComponent& rig, const glm::vec2& lookDelta)
    {
        if (!Math::IsFinite(lookDelta) || !std::isfinite(rig.m_LookSensitivity))
            return;

        // Screen +X is right and +Y is DOWN. Moving the mouse right should yaw
        // right (a negative rotation about +Y with -Z forward), and moving it
        // up (negative screen Y) should pitch up.
        const f32 yawDelta = -lookDelta.x * rig.m_LookSensitivity;
        f32 pitchDelta = -lookDelta.y * rig.m_LookSensitivity;
        if (rig.m_InvertLookY)
            pitchDelta = -pitchDelta;

        rig.m_YawDeg = WrapDegrees(rig.m_YawDeg + yawDelta);

        // Tolerate a scene that authored the limits the wrong way round rather
        // than producing an empty range std::clamp would assert on.
        f32 minPitch = std::isfinite(rig.m_MinPitchDeg) ? rig.m_MinPitchDeg : -89.9f;
        f32 maxPitch = std::isfinite(rig.m_MaxPitchDeg) ? rig.m_MaxPitchDeg : 89.9f;
        if (minPitch > maxPitch)
            std::swap(minPitch, maxPitch);

        const f32 pitch = std::isfinite(rig.m_PitchDeg) ? rig.m_PitchDeg : 0.0f;
        rig.m_PitchDeg = std::clamp(pitch + pitchDelta, minPitch, maxPitch);
    }

    f32 PlayerRigSystem::MoveMagnitude(const glm::vec2& moveInput)
    {
        if (!Math::IsFinite(moveInput))
            return 0.0f;
        const f32 lengthSq = glm::dot(moveInput, moveInput);
        if (lengthSq <= kDegenerateLengthSq)
            return 0.0f;
        return std::min(std::sqrt(lengthSq), 1.0f);
    }

    glm::vec3 PlayerRigSystem::WishDirection(const glm::vec2& moveInput, f32 yawDeg)
    {
        if (!Math::IsFinite(moveInput))
            return glm::vec3(0.0f);

        const f32 lengthSq = glm::dot(moveInput, moveInput);
        if (lengthSq <= kDegenerateLengthSq)
            return glm::vec3(0.0f);

        const glm::quat yaw = YawRotation(yawDeg);
        const glm::vec3 forward = yaw * kLocalForward;
        const glm::vec3 right = yaw * kLocalRight;

        const glm::vec2 unit = moveInput / std::sqrt(lengthSq);
        glm::vec3 direction = right * unit.x + forward * unit.y;
        direction.y = 0.0f;

        const f32 dirLengthSq = glm::dot(direction, direction);
        if (dirLengthSq <= kDegenerateLengthSq)
            return glm::vec3(0.0f);
        return direction / std::sqrt(dirLengthSq);
    }

    f32 PlayerRigSystem::TurnTowardsYaw(f32 currentYawDeg, f32 targetYawDeg, f32 turnRateDeg, f32 dt)
    {
        if (!std::isfinite(currentYawDeg))
            currentYawDeg = 0.0f;
        if (!std::isfinite(targetYawDeg) || !std::isfinite(turnRateDeg) || !std::isfinite(dt) || dt <= 0.0f)
            return WrapDegrees(currentYawDeg);

        // Shortest signed way round, so turning from 179° to -179° is a 2°
        // nudge rather than a 358° spin.
        const f32 delta = WrapDegrees(targetYawDeg - currentYawDeg);
        if (turnRateDeg <= 0.0f)
            return WrapDegrees(currentYawDeg);

        const f32 maxStep = turnRateDeg * dt;
        const f32 step = std::clamp(delta, -maxStep, maxStep);
        return WrapDegrees(currentYawDeg + step);
    }

    void PlayerRigSystem::Step(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (scene == nullptr || !std::isfinite(dt) || dt <= 0.0f)
            return;

        JoltScene* physics = scene->GetPhysicsScene();

        // Whether ANY rig wants the pointer captured. Applied once after the
        // loop rather than per rig: SetCursorMode is a process-wide window
        // setting, so N rigs must not fight over it, and a scene with no rig at
        // all must not clobber whatever mode the host chose.
        bool wantsCapture = false;
        bool sawDeviceRig = false;

        // Resolve the release latch BEFORE sampling, so pressing Escape stops
        // the player on the same tick it frees the cursor rather than one
        // later. Polling these with no window in play is harmless — Input
        // reports nothing pressed.
        if (Input::IsKeyJustPressed(Key::Escape))
        {
            s_CursorReleasedByUser = true;
        }
        else if (s_CursorReleasedByUser && Input::IsMouseButtonPressed(Mouse::ButtonLeft))
        {
            s_CursorReleasedByUser = false;
        }

        for (auto view = scene->GetAllEntitiesWith<TransformComponent, PlayerRigComponent>();
             const auto entity : view)
        {
            auto& transform = view.get<TransformComponent>(entity);
            auto& rig = view.get<PlayerRigComponent>(entity);

            if (rig.m_UseDeviceInput)
            {
                sawDeviceRig = true;
                wantsCapture = wantsCapture || rig.m_CaptureCursor;

                if (rig.m_CaptureCursor && s_CursorReleasedByUser)
                {
                    // Cursor handed back to the OS: the player must stop dead
                    // rather than keep walking (and keep looking at wherever
                    // the now-visible pointer wanders) while the user is busy
                    // with the editor.
                    rig.m_MoveInput = glm::vec2(0.0f);
                    rig.m_LookInput = glm::vec2(0.0f);
                    rig.m_SprintInput = false;
                    rig.m_JumpInput = false;
                    // Re-capture must not read the intervening pointer travel
                    // as one enormous look flick, nor treat a jump key that was
                    // already held while the cursor was free as a fresh press.
                    rig.m_HasLastMousePos = false;
                    rig.m_JumpKeyWasDown = true;
                }
                else
                {
                    SampleDeviceInput(rig);
                }
            }

            ApplyLookDelta(rig, rig.m_LookInput);
            // Consumed. A device rig re-fills it next tick; an externally
            // driven one must re-assert its intent each tick, exactly like a
            // network input command.
            rig.m_LookInput = glm::vec2(0.0f);

            const f32 magnitude = MoveMagnitude(rig.m_MoveInput);
            const f32 bodyYaw = rig.m_MoveRelativeToLook ? rig.m_YawDeg
                                                         : YawDegreesFromRotation(transform.GetRotation());
            const glm::vec3 wishDir = WishDirection(rig.m_MoveInput, bodyYaw);

            const f32 sprint = rig.m_SprintInput ? std::max(1.0f, rig.m_SprintMultiplier) : 1.0f;
            const f32 groundSpeed = std::max(0.0f, rig.m_WalkSpeed) * sprint * magnitude;

            Entity owner{ entity, scene };
            // Non-const Ref: Ref<T>::operator-> propagates constness, so a
            // `const Ref` here would make every setter call below ill-formed.
            Ref<JoltCharacterController> controller =
                physics != nullptr ? physics->GetCharacterController(owner) : nullptr;

            const bool grounded = controller ? controller->IsGrounded() : true;
            rig.m_Grounded = grounded;

            // Air control only scales the wish velocity; whether the controller
            // honours ANY steering while airborne is
            // CharacterController3DComponent::m_ControlMovementInAir, which is
            // the engine's existing contract and not this rig's to override.
            const f32 speed = grounded ? groundSpeed : groundSpeed * std::clamp(rig.m_AirControl, 0.0f, 1.0f);
            const glm::vec3 wishVelocity = wishDir * speed;
            rig.m_PlanarSpeed = speed;

            if (controller)
            {
                // The controller ADDS this desired velocity to gravity / ground
                // velocity each step (JoltCharacterController::ApplyGravityAndJump)
                // and never clears it, so it must be re-asserted every tick —
                // including as zero when there is no input, or the player
                // slides forever.
                controller->SetLinearVelocity(wishVelocity);

                if (rig.m_JumpInput)
                {
                    // Jump power is the character controller's own authored
                    // field rather than a second copy on the rig: two sources
                    // of truth for the same number is exactly the divergence
                    // the serializer's Clamp-vs-Reject lesson warns about.
                    const f32 jumpPower = owner.HasComponent<CharacterController3DComponent>()
                                              ? owner.GetComponent<CharacterController3DComponent>().m_JumpPower
                                              : 5.0f;
                    controller->Jump(jumpPower);
                }
            }
            else
            {
                // No character controller: integrate the wish velocity straight
                // into the transform. Deliberately collision-free — the rig is
                // still useful for a spectator/flycam-style player, and pulling
                // in a physics dependency the entity never opted into would be
                // worse than doing the obvious thing.
                if (Math::IsFinite(transform.Translation) && Math::IsFinite(wishVelocity))
                    transform.Translation += wishVelocity * dt;
            }

            // Jump is edge-triggered: consume it whether or not a controller
            // was there to honour it, so it can't fire spuriously later.
            rig.m_JumpInput = false;

            // ── Body facing ──────────────────────────────────────────────────
            if (rig.m_YawBodyWithLook)
            {
                const glm::quat bodyRotation = YawRotation(rig.m_YawDeg);
                if (controller)
                    controller->SetRotation(bodyRotation);
                else
                    transform.SetRotation(bodyRotation);
            }
            else if (rig.m_FaceMoveDirection && glm::dot(wishDir, wishDir) > kDegenerateLengthSq)
            {
                // The yaw whose forward (-Z) points along wishDir — the same
                // inverse YawDegreesFromRotation uses.
                const f32 targetYaw = glm::degrees(std::atan2(-wishDir.x, -wishDir.z));
                const f32 currentYaw = YawDegreesFromRotation(transform.GetRotation());
                const f32 newYaw = TurnTowardsYaw(currentYaw, targetYaw, rig.m_TurnRateDeg, dt);
                const glm::quat bodyRotation = YawRotation(newYaw);
                if (controller)
                    controller->SetRotation(bodyRotation);
                else
                    transform.SetRotation(bodyRotation);
            }
        }

        if (sawDeviceRig)
        {
            const CursorMode desired =
                (wantsCapture && !s_CursorReleasedByUser) ? CursorMode::Locked : CursorMode::Normal;
            if (Input::GetCursorMode() != desired)
            {
                Input::SetCursorMode(desired);
                // Locking teleports the pointer, so the next sample must not
                // read that jump as a look flick. Rebasing every device rig is
                // cheaper and more obviously correct than trying to predict
                // where the OS will put the cursor.
                for (auto view = scene->GetAllEntitiesWith<PlayerRigComponent>(); const auto entity : view)
                    view.get<PlayerRigComponent>(entity).m_HasLastMousePos = false;
            }
        }
    }

    void PlayerRigSystem::ReleaseCursorCapture()
    {
        s_CursorReleasedByUser = false;
        if (Input::GetCursorMode() != CursorMode::Normal)
        {
            Input::SetCursorMode(CursorMode::Normal);
        }
    }

    // =========================================================================
    // CameraRigSystem
    // =========================================================================

    f32 CameraRigSystem::ClearanceFromHit(f32 hitDistance, f32 desiredLength, f32 probeRadius, f32 minLength)
    {
        if (!std::isfinite(hitDistance) || !std::isfinite(desiredLength))
            return std::isfinite(desiredLength) ? desiredLength : 0.0f;

        const f32 radius = std::isfinite(probeRadius) ? std::max(0.0f, probeRadius) : 0.0f;
        const f32 floorLength = std::isfinite(minLength) ? std::max(0.0f, minLength) : 0.0f;

        const f32 clearance = hitDistance - radius;
        // The floor wins over the clearance (the camera stops collapsing onto
        // the pivot in a tight corner) but never over the authored boom — a
        // first-person rig with m_BoomLength 0 must stay at 0 whatever
        // m_MinBoomLength says.
        return std::min(desiredLength, std::max(floorLength, clearance));
    }

    f32 CameraRigSystem::AdvanceBoom(f32 currentLength, f32 allowedLength, f32 returnSpeed, f32 dt)
    {
        if (!std::isfinite(allowedLength))
            return std::isfinite(currentLength) ? currentLength : 0.0f;
        if (!std::isfinite(currentLength))
            return allowedLength;

        // Shortening is instant: easing in would leave the camera inside the
        // obstruction for those frames, which is the artefact the probe exists
        // to prevent.
        if (allowedLength <= currentLength)
            return allowedLength;

        if (!std::isfinite(returnSpeed) || returnSpeed <= 0.0f || !std::isfinite(dt) || dt <= 0.0f)
            return allowedLength;

        return std::min(allowedLength, currentLength + returnSpeed * dt);
    }

    glm::vec3 CameraRigSystem::SmoothTowards(const glm::vec3& current, const glm::vec3& target, f32 smoothTime, f32 dt)
    {
        if (!Math::IsFinite(target))
            return Math::IsFinite(current) ? current : glm::vec3(0.0f);
        if (!Math::IsFinite(current))
            return target;
        if (!std::isfinite(smoothTime) || smoothTime <= 0.0f || !std::isfinite(dt) || dt <= 0.0f)
            return target;

        const f32 alpha = 1.0f - std::exp(-dt / smoothTime);
        return current + (target - current) * alpha;
    }

    void CameraRigSystem::Step(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (scene == nullptr || !std::isfinite(dt) || dt <= 0.0f)
            return;

        JoltScene* physics = scene->GetPhysicsScene();

        for (auto view = scene->GetAllEntitiesWith<TransformComponent, CameraRigComponent>();
             const auto entity : view)
        {
            auto& rig = view.get<CameraRigComponent>(entity);
            if (rig.m_Target == 0)
                continue;

            Entity cameraEntity{ entity, scene };

            auto targetEntity = scene->TryGetEntityWithUUID(rig.m_Target);
            if (!targetEntity.has_value())
                continue;

            Entity target = *targetEntity;
            const glm::vec3 targetPosition = WorldPosition(*scene, static_cast<entt::entity>(target));
            if (!Math::IsFinite(targetPosition))
                continue;

            // ── Look angles ──────────────────────────────────────────────────
            // The player rig is the single owner of yaw/pitch for the whole rig
            // (see PlayerRigComponents.h). Following something that is not a
            // player — a vehicle, a prop — falls back to the target's own
            // facing plus an authored pitch.
            f32 yawDeg = 0.0f;
            f32 pitchDeg = rig.m_FallbackPitchDeg;
            if (target.HasComponent<PlayerRigComponent>())
            {
                const auto& playerRig = target.GetComponent<PlayerRigComponent>();
                yawDeg = playerRig.m_YawDeg;
                pitchDeg = playerRig.m_PitchDeg;
            }
            else if (target.HasComponent<TransformComponent>())
            {
                yawDeg = YawDegreesFromRotation(target.GetComponent<TransformComponent>().GetRotation());
            }

            const glm::quat yawRotation = PlayerRigSystem::YawRotation(yawDeg);
            const glm::quat lookRotation = PlayerRigSystem::LookRotation(yawDeg, pitchDeg);

            // Pivot rides the target in its YAW frame only — pitching the pivot
            // with the look would swing the eye through an arc every time the
            // player glanced up.
            const glm::vec3 pivotOffset = Math::IsFinite(rig.m_PivotOffset) ? rig.m_PivotOffset : glm::vec3(0.0f);
            const glm::vec3 pivot = targetPosition + yawRotation * pivotOffset;

            // ── Spring arm ───────────────────────────────────────────────────
            const f32 desiredBoom = std::isfinite(rig.m_BoomLength) ? std::max(0.0f, rig.m_BoomLength) : 0.0f;
            const glm::vec3 boomDir = glm::normalize(lookRotation * kLocalBackward);

            f32 allowedBoom = desiredBoom;
            if (rig.m_CollisionEnabled && desiredBoom > 0.0f && physics != nullptr && Math::IsFinite(boomDir))
            {
                const f32 probeRadius = std::isfinite(rig.m_ProbeRadius) ? std::max(0.0f, rig.m_ProbeRadius) : 0.0f;

                RayCastInfo ray;
                ray.m_Origin = pivot;
                ray.m_Direction = boomDir;
                ray.m_MaxDistance = desiredBoom + probeRadius;
                // Exclude BOTH endpoints of the arm. Without the target the
                // boom instantly collapses onto the player's own capsule — the
                // pivot sits inside it — and without the camera entity a
                // collider authored on the camera itself would occlude it.
                ray.m_ExcludedEntities.push_back(rig.m_Target);
                ray.m_ExcludedEntities.push_back(cameraEntity.GetUUID());

                if (SceneQueryHit hit; physics->CastRay(ray, hit) && hit.HasHit())
                    allowedBoom = ClearanceFromHit(hit.m_Distance, desiredBoom, probeRadius, rig.m_MinBoomLength);
            }

            if (!rig.m_Initialized)
            {
                rig.m_CurrentBoomLength = allowedBoom;
            }
            else
            {
                rig.m_CurrentBoomLength =
                    AdvanceBoom(rig.m_CurrentBoomLength, allowedBoom, rig.m_BoomReturnSpeed, dt);
            }

            glm::vec3 cameraPosition = pivot + boomDir * rig.m_CurrentBoomLength;

            // ── Head bob ─────────────────────────────────────────────────────
            // Phase advances with DISTANCE, not time, so the bob is locked to
            // the stride: it stops dead when the player does, and it does not
            // change frequency with the frame rate or the number of sub-steps.
            const f32 bobAmplitude = std::isfinite(rig.m_HeadBobAmplitude) ? std::max(0.0f, rig.m_HeadBobAmplitude) : 0.0f;
            if (rig.m_Initialized && bobAmplitude > 0.0f)
            {
                const glm::vec3 travel = targetPosition - rig.m_PrevTargetPosition;
                const f32 planarDistance = std::sqrt(travel.x * travel.x + travel.z * travel.z);
                const f32 frequency = std::isfinite(rig.m_HeadBobFrequency) ? std::max(0.0f, rig.m_HeadBobFrequency) : 0.0f;
                // A non-finite phase LATCHES: fmod(NaN + x, 2pi) is NaN, so is
                // sin(NaN), so the camera position goes NaN and the finiteness
                // guard below skips the transform write — for every subsequent
                // tick, not just this one. The camera would freeze permanently
                // and never recover on its own. Re-seed instead of accumulating.
                if (!std::isfinite(rig.m_BobPhase))
                {
                    rig.m_BobPhase = 0.0f;
                }
                if (std::isfinite(planarDistance))
                {
                    rig.m_BobPhase = std::fmod(rig.m_BobPhase + planarDistance * frequency * glm::two_pi<f32>(),
                                               glm::two_pi<f32>());
                    cameraPosition += kWorldUp * (std::sin(rig.m_BobPhase) * bobAmplitude);
                }
            }

            // ── Smoothing ────────────────────────────────────────────────────
            if (!rig.m_Initialized)
            {
                rig.m_SmoothedPosition = cameraPosition;
            }
            else
            {
                rig.m_SmoothedPosition =
                    SmoothTowards(rig.m_SmoothedPosition, cameraPosition, rig.m_PositionSmoothTime, dt);
            }

            rig.m_PrevTargetPosition = targetPosition;
            rig.m_Initialized = true;

            if (!Math::IsFinite(rig.m_SmoothedPosition))
                continue;

            // The rig computes an absolute world pose, so the camera entity is
            // expected to be a ROOT entity — parenting it would compose that
            // pose with the parent's transform a second time. Warned once
            // rather than per frame (a per-frame log is not "degrades
            // gracefully", it is a flood).
            auto& transform = view.get<TransformComponent>(entity);
            if (cameraEntity.GetParentUUID() != 0)
            {
                static bool s_WarnedAboutParentedRig = false;
                if (!s_WarnedAboutParentedRig)
                {
                    s_WarnedAboutParentedRig = true;
                    OLO_CORE_WARN("[CameraRig] Camera entity has a parent; the rig writes an absolute world pose, "
                                  "so the parent transform will be applied twice. Make the camera a root entity.");
                }
            }

            transform.Translation = rig.m_SmoothedPosition;
            transform.SetRotation(lookRotation);
        }
    }
} // namespace OloEngine
