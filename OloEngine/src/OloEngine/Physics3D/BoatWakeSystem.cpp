#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/BoatWakeSystem.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Physics3D/WaterProbe.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Below this the horizontal heading is meaningless (the hull is pointing
        // straight up or down) — the same guard, and the same threshold, that
        // BoatSystem uses to reject a degenerate attitude.
        constexpr f32 kMinHorizontalForward = 1.0e-3f;

        // Fallbacks for a hull whose collider gives no usable extent.
        constexpr f32 kDefaultBeamMetres = 2.4f;
        constexpr f32 kDefaultLengthMetres = 6.0f;

        [[nodiscard("the finiteness result must be used")]] bool IsFinite2(const glm::vec2& v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y);
        }

        [[nodiscard("the finiteness result must be used")]] bool IsFinite3(const glm::vec3& v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        /// Hull beam and length in metres, from the Jolt collider's local
        /// bounds. Falls back to a launch-sized default when the shape is
        /// missing or degenerate — a wake sized off a garbage AABB would be
        /// either invisible or a hundred metres wide, and the second one is
        /// much harder to notice as a bug than the first.
        void HullExtents(const Ref<JoltBody>& body, f32& beam, f32& length)
        {
            beam = kDefaultBeamMetres;
            length = kDefaultLengthMetres;

            const JPH::RefConst<JPH::Shape> shape = body->GetShape();
            if (!shape)
                return;

            const JPH::AABox bounds = shape->GetLocalBounds();
            const JPH::Vec3 size = bounds.GetSize();
            const f32 x = size.GetX();
            const f32 z = size.GetZ();
            if (std::isfinite(x) && x > 0.05f && std::isfinite(z) && z > 0.05f)
            {
                beam = std::clamp(x, 0.2f, 60.0f);
                length = std::clamp(z, 0.2f, 300.0f);
            }
        }
    } // namespace

    void BoatWakeSystem::OnUpdate(Scene* scene, TMap<u64, BoatWakeTrail>& history, f32 simulationTime,
                                  f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !std::isfinite(simulationTime) || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        JoltScene* jolt = scene->GetPhysicsScene();
        if (!jolt)
            return;

        auto view = scene->GetAllEntitiesWith<TransformComponent, BoatComponent, Rigidbody3DComponent>();
        if (view.begin() == view.end())
            return;

        // The same enabled water tiles BuoyancySystem floats the hulls on and
        // BoatSystem drives them through. A boat that is not over water leaves
        // no wake, and going through WaterProbe rather than a height guess is
        // what keeps that judgement consistent with the rest of the boat stack.
        const std::vector<WaterProbe::Volume> waters = WaterProbe::CollectEnabledVolumes(scene);
        if (waters.empty())
            return;

        for (auto e : view)
        {
            Entity entity{ e, scene };
            const auto& boat = entity.GetComponent<BoatComponent>();
            if (!boat.m_Enabled)
                continue;

            Ref<JoltBody> body = jolt->GetBody(entity);
            if (!body)
                continue;

            const glm::vec3 bodyPos = body->GetPosition();
            if (!IsFinite3(bodyPos))
                continue;

            const u64 uuid = entity.GetUUID();
            BoatWakeTrail& trail = history.FindOrAdd(uuid, BoatWakeTrail{});

            if (!WaterProbe::FindVolumeAt(waters, bodyPos))
            {
                // Beached or airborne. Drop the history rather than keep it:
                // resuming from a stale sample would splat one enormous capsule
                // bridging wherever the boat left the water and wherever it
                // re-entered.
                trail.Clear();
                continue;
            }

            const glm::quat rot = body->GetRotation();
            const glm::vec3 hullForward = rot * glm::vec3(0.0f, 0.0f, 1.0f);
            if (!IsFinite3(hullForward))
                continue;

            glm::vec2 forwardXZ(hullForward.x, hullForward.z);
            const f32 forwardLen = glm::length(forwardXZ);
            if (!(forwardLen > kMinHorizontalForward))
                continue;
            forwardXZ /= forwardLen;

            const glm::vec3 velocity = body->GetLinearVelocity();
            if (!IsFinite3(velocity))
                continue;
            const glm::vec2 velocityXZ(velocity.x, velocity.z);
            const f32 forwardSpeed = glm::dot(velocityXZ, forwardXZ);

            const glm::vec2 posXZ(bodyPos.x, bodyPos.z);
            if (!IsFinite2(posXZ))
                continue;

            const BoatWakeSample previous = trail.At(0);

            BoatWakeSample sample;
            sample.m_WorldXZ = posXZ;
            sample.m_ForwardXZ = forwardXZ;
            sample.m_ForwardSpeed = forwardSpeed;
            sample.m_TimeSeconds = simulationTime;
            sample.m_Valid = true;
            trail.Push(sample);

            f32 beam = kDefaultBeamMetres;
            f32 length = kDefaultLengthMetres;
            HullExtents(body, beam, length);

            // Speed gate. |speed| so a boat backing up churns too — a reversing
            // propeller makes more foam than a forward one, not less.
            const f32 speedGate =
                glm::clamp((std::abs(forwardSpeed) - kMinSpeedMetresPerSecond) /
                               std::max(kFullSpeedMetresPerSecond - kMinSpeedMetresPerSecond, 1.0e-3f),
                           0.0f, 1.0f);

            // Starboard in a right-handed frame with +Y up is forward x up,
            // i.e. forward rotated -90 degrees about Y. Getting this backwards
            // mirrors the V-arms, which on a symmetric wake looks completely
            // correct — see BoatSystem.cpp and issue #897.
            const glm::vec2 starboardXZ(-forwardXZ.y, forwardXZ.x);

            // --- 1. Hull churn: the swept capsule -----------------------------
            if (speedGate > 0.0f)
            {
                WaterDisturbanceSplat hull;
                // A first sample has no predecessor; degenerate the capsule to a
                // point rather than sweeping from (0,0), which would paint a
                // line from the world origin to the boat.
                hull.m_From = previous.m_Valid ? previous.m_WorldXZ : posXZ;
                hull.m_To = posXZ;
                hull.m_Radius = beam * 0.55f;
                hull.m_Strength = 0.35f + 0.5f * speedGate;
                hull.m_Softness = 1.4f;
                hull.m_TimeSeconds = simulationTime;
                (void)WaterDisturbanceSystem::SubmitSplat(hull);
            }

            // --- 2. The diverging V-arms --------------------------------------
            // Laid at the pose that is now kArmAgeSeconds old, offset laterally
            // by an amount that grows with that age. See the header for why the
            // offset has to come from picking an older sample rather than from
            // re-stamping a newer one.
            const BoatWakeSample armSample = trail.AtAge(simulationTime, kArmAgeSeconds);
            if (armSample.m_Valid)
            {
                const f32 armAge = simulationTime - armSample.m_TimeSeconds;
                const f32 armGate = glm::clamp(
                    (std::abs(armSample.m_ForwardSpeed) - kMinSpeedMetresPerSecond) /
                        std::max(kFullSpeedMetresPerSecond - kMinSpeedMetresPerSecond, 1.0e-3f),
                    0.0f, 1.0f);
                if (armGate > 0.0f)
                {
                    const glm::vec2 armStarboard(-armSample.m_ForwardXZ.y, armSample.m_ForwardXZ.x);
                    const f32 offset = beam * 0.5f + kArmSpreadMetresPerSecond * armAge * armGate;
                    // The arms are narrower and weaker than the hull churn, and
                    // fade with the spread — a real Kelvin arm thins as it
                    // diverges.
                    const f32 armStrength = (0.25f + 0.35f * armGate) /
                                            (1.0f + 0.6f * kArmSpreadMetresPerSecond * armAge);
                    for (const f32 side : { -1.0f, 1.0f })
                    {
                        WaterDisturbanceSplat arm;
                        arm.m_From = armSample.m_WorldXZ + armStarboard * (offset * side);
                        arm.m_To = arm.m_From;
                        arm.m_Radius = std::max(beam * 0.3f, 0.4f);
                        arm.m_Strength = armStrength;
                        arm.m_Softness = 2.0f;
                        arm.m_TimeSeconds = simulationTime;
                        (void)WaterDisturbanceSystem::SubmitSplat(arm);
                    }
                }
            }

            // --- 3. Propeller wash --------------------------------------------
            // Gated on THROTTLE, not speed: a boat holding station against a
            // current, or pinned against a jetty at full power, is churning hard
            // while moving at zero — which the speed gate above would report as
            // no wake at all.
            const f32 throttle =
                std::isfinite(boat.m_ThrottleInput) ? std::clamp(std::abs(boat.m_ThrottleInput), 0.0f, 1.0f) : 0.0f;
            if (throttle > 0.05f)
            {
                const f32 sternOffset =
                    std::isfinite(boat.m_ThrustOffsetZ) ? std::clamp(boat.m_ThrustOffsetZ, -length, length) : -length * 0.4f;
                WaterDisturbanceSplat prop;
                prop.m_From = posXZ + forwardXZ * sternOffset;
                prop.m_To = prop.m_From;
                prop.m_Radius = std::max(beam * 0.45f, 0.5f);
                prop.m_Strength = 0.3f + 0.6f * throttle;
                prop.m_Softness = 1.1f;
                prop.m_TimeSeconds = simulationTime;
                (void)WaterDisturbanceSystem::SubmitSplat(prop);
            }
        }
    }
} // namespace OloEngine
