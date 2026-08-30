#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/BoatWakeSystem.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Physics3D/WaterProbe.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
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

        // Drop last tick's wake-shape records BEFORE any of the early-outs
        // below (issue #968). A frame with no physics scene, no boats or no
        // water must publish an EMPTY record set, not last frame's — otherwise
        // a boat that beaches, or a scene whose water is switched off, leaves
        // its hull footprint pressed into the sea until something else happens
        // to overwrite the slot, and buoyancy keeps floating on a wake that is
        // no longer being made.
        WaterWakeSystem::BeginFrame();

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
            // propeller makes more foam than a forward one, not less. Shared by
            // the hull sweep and every V-arm segment, each of which gates on the
            // speed the hull had at ITS sample's moment rather than on the
            // speed right now.
            const auto speedGate = [](f32 speed)
            {
                return glm::clamp((std::abs(speed) - kMinSpeedMetresPerSecond) /
                                      std::max(kFullSpeedMetresPerSecond - kMinSpeedMetresPerSecond, 1.0e-3f),
                                  0.0f, 1.0f);
            };
            const f32 hullGate = speedGate(forwardSpeed);

            // Starboard in a right-handed frame with +Y up is forward x up,
            // i.e. forward rotated -90 degrees about Y. Getting this backwards
            // mirrors the V-arms, which on a symmetric wake looks completely
            // correct — see BoatSystem.cpp and issue #897.
            const glm::vec2 starboardXZ(-forwardXZ.y, forwardXZ.x);

            // --- 1. Hull churn: the swept capsule -----------------------------
            if (hullGate > 0.0f)
            {
                WaterDisturbanceSplat hull;
                // A first sample has no predecessor; degenerate the capsule to a
                // point rather than sweeping from (0,0), which would paint a
                // line from the world origin to the boat.
                hull.m_From = previous.m_Valid ? previous.m_WorldXZ : posXZ;
                hull.m_To = posXZ;
                hull.m_Radius = beam * 0.55f;
                hull.m_Strength = 0.35f + 0.5f * hullGate;
                hull.m_Softness = 1.4f;
                hull.m_TimeSeconds = simulationTime;
                (void)WaterDisturbanceSystem::SubmitSplat(hull);
            }

            // The wake-shape record for this hull (issue #968), filled from the
            // same arm loop below and submitted once it has run. `HullExtents`
            // reports full beam and length; the shape record wants halves.
            WaterWakeHullDesc shapeDesc{};
            shapeDesc.m_CentreXZ = posXZ;
            shapeDesc.m_ForwardXZ = forwardXZ;
            shapeDesc.m_HalfBeam = beam * 0.5f;
            shapeDesc.m_HalfLength = length * 0.5f;
            shapeDesc.m_Speed = forwardSpeed;
            shapeDesc.m_Gate = hullGate;
            u32 shapeSampleCount = 0;

            // --- 2. The diverging V-arms --------------------------------------
            // Sample the hull history at several AGES and join consecutive ones
            // with a capsule per side, offsetting each end by its own sample's
            // age. The age range is what makes the V spread — see the header for
            // why a single age silently produces parallel lines instead.
            {
                BoatWakeSample prevSample{};
                f32 prevOffset = 0.0f;
                f32 prevGate = 0.0f;
                bool prevValid = false;

                for (u32 i = 0; i < kArmAgeSamples; ++i)
                {
                    const f32 t =
                        (kArmAgeSamples > 1u) ? static_cast<f32>(i) / static_cast<f32>(kArmAgeSamples - 1u) : 0.0f;
                    const f32 wantAge = kArmAgeMinSeconds + t * (kArmAgeMaxSeconds - kArmAgeMinSeconds);

                    const BoatWakeSample s = trail.AtAge(simulationTime, wantAge);
                    if (!s.m_Valid)
                    {
                        // History does not reach back this far yet (a boat that
                        // just got under way, or a ring that has wrapped). Break
                        // the chain rather than joining across the gap, which
                        // would draw one long arm through water the hull never
                        // crossed. Ages only increase, so nothing later can be
                        // valid either.
                        break;
                    }

                    const f32 age = simulationTime - s.m_TimeSeconds;
                    const f32 gate = speedGate(s.m_ForwardSpeed);
                    // The Kelvin law (WaterWake.h section 4): the lateral offset
                    // grows with the DISTANCE run, not the time elapsed, so the
                    // arms open at 19.47 degrees whatever the throttle. The
                    // constant 1.6 m/s spread this replaced gave 15 degrees at
                    // 6 m/s and 5 at 18, i.e. a wake that visibly narrowed as
                    // the boat accelerated.
                    const f32 offset = ArmOffset(beam * 0.5f, s.m_ForwardSpeed, gate, age);

                    // The same pose feeds the wake SHAPE record (issue #968).
                    // Collected here rather than in a second pass over the trail
                    // so the foam arm and the height ridge cannot be laid from
                    // different samples.
                    if (shapeSampleCount < WaterWake::kMaxArmSamples)
                    {
                        WaterWakeArmSample& shape = shapeDesc.m_Arms[shapeSampleCount++];
                        shape.m_CentreXZ = s.m_WorldXZ;
                        shape.m_ForwardXZ = s.m_ForwardXZ;
                        shape.m_AgeSeconds = age;
                        shape.m_Speed = s.m_ForwardSpeed;
                        shape.m_Gate = gate;
                    }

                    if (prevValid && gate > 0.0f && prevGate > 0.0f)
                    {
                        const glm::vec2 prevStarboard(-prevSample.m_ForwardXZ.y, prevSample.m_ForwardXZ.x);
                        const glm::vec2 curStarboard(-s.m_ForwardXZ.y, s.m_ForwardXZ.x);
                        // The arms are narrower and weaker than the hull churn,
                        // and fade as they spread — a real Kelvin arm thins as
                        // it diverges. Keyed on the OLDER end so a segment never
                        // reads brighter than the one ahead of it.
                        const f32 armStrength =
                            (0.25f + 0.35f * gate) / (1.0f + kArmFoamDecayPerSecond * age);
                        for (const f32 side : { -1.0f, 1.0f })
                        {
                            WaterDisturbanceSplat arm;
                            arm.m_From = prevSample.m_WorldXZ + prevStarboard * (prevOffset * side);
                            arm.m_To = s.m_WorldXZ + curStarboard * (offset * side);
                            arm.m_Radius = std::max(beam * 0.3f, 0.4f);
                            arm.m_Strength = armStrength;
                            arm.m_Softness = 2.0f;
                            arm.m_TimeSeconds = simulationTime;
                            (void)WaterDisturbanceSystem::SubmitSplat(arm);
                        }
                    }

                    prevSample = s;
                    prevOffset = offset;
                    prevGate = gate;
                    prevValid = true;
                }
            }

            // Publish the shape. Submitted for EVERY boat over water, including
            // one that is stopped: the hull footprint suppression and its own
            // displacement are not speed-gated (a moored boat still keeps the
            // sea out of its cockpit), and only the bow/stern/arm amplitudes
            // fade to nothing with the gate. Submitting only moving boats is
            // what would make a stopped hull start clipping water through the
            // deck — which is the acceptance criterion this whole feature is for.
            shapeDesc.m_ArmSampleCount = shapeSampleCount;
            (void)WaterWakeSystem::SubmitHull(shapeDesc);

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
