#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/BoatSystem.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Physics3D/WaterProbe.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Below this the "horizontal forward" projection is meaningless (the hull
        // is pointing straight up or down) — skip thrust rather than normalizing
        // a near-zero vector into garbage.
        constexpr f32 kMinHorizontalForward = 1.0e-3f;

        [[nodiscard("finiteness result must be used")]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        /// Fraction of full effect for a point sitting `surfaceY - pointY` metres
        /// below the water, ramped over `immersionDepth`. 0 = clear of the water.
        [[nodiscard("immersion fraction must be used")]] f32 ImmersionFraction(f32 surfaceY, f32 pointY, f32 immersionDepth)
        {
            const f32 depth = surfaceY - pointY;
            if (!(depth > 0.0f)) // the negation also rejects NaN
                return 0.0f;
            return std::clamp(depth / immersionDepth, 0.0f, 1.0f);
        }
    } // namespace

    void BoatSystem::OnUpdate(Scene* scene, f32 rawTime, [[maybe_unused]] f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !std::isfinite(rawTime))
            return;

        JoltScene* jolt = scene->GetPhysicsScene();
        if (!jolt)
            return;

        // Cheap early-out before paying for the water-tile resolution.
        auto view = scene->GetAllEntitiesWith<TransformComponent, BoatComponent, Rigidbody3DComponent>();
        if (view.begin() == view.end())
            return;

        // The same enabled water tiles BuoyancySystem floats the hulls on.
        const std::vector<WaterProbe::Volume> waters = WaterProbe::CollectEnabledVolumes(scene);
        if (waters.empty())
            return;

        for (auto e : view)
        {
            Entity entity{ e, scene };
            const auto& boat = entity.GetComponent<BoatComponent>();
            if (!boat.m_Enabled)
                continue;

            if (entity.GetComponent<Rigidbody3DComponent>().m_Type != BodyType3D::Dynamic)
                continue;

            Ref<JoltBody> body = jolt->GetBody(entity);
            if (!body || !body->IsDynamic())
                continue;

            const glm::vec3 bodyPos = body->GetPosition();
            if (!IsFinite(bodyPos))
                continue;

            const WaterProbe::Volume* water = WaterProbe::FindVolumeAt(waters, bodyPos);
            if (!water)
                continue; // out over dry land — a beached boat just sits there

            // --- Sanitize tunables (defence in depth against bad serialized /
            // scripted data; every one of these can reach us as a raw field) ---
            const f32 maxThrust = (std::isfinite(boat.m_MaxThrust) && boat.m_MaxThrust > 0.0f) ? boat.m_MaxThrust : 0.0f;
            const f32 thrustOffsetZ = std::isfinite(boat.m_ThrustOffsetZ) ? boat.m_ThrustOffsetZ : -2.0f;
            const f32 thrustOffsetY = std::isfinite(boat.m_ThrustOffsetY) ? boat.m_ThrustOffsetY : -0.3f;
            const f32 maxRudderTorque = (std::isfinite(boat.m_MaxRudderTorque) && boat.m_MaxRudderTorque > 0.0f) ? boat.m_MaxRudderTorque : 0.0f;
            const f32 rudderSpeed = (std::isfinite(boat.m_RudderAuthoritySpeed) && boat.m_RudderAuthoritySpeed > 1.0e-2f) ? boat.m_RudderAuthoritySpeed : 4.0f;
            const f32 lateralDrag = std::isfinite(boat.m_LateralDrag) ? std::max(boat.m_LateralDrag, 0.0f) : 0.0f;
            const f32 forwardDrag = std::isfinite(boat.m_ForwardDrag) ? std::max(boat.m_ForwardDrag, 0.0f) : 0.0f;
            const f32 yawDrag = std::isfinite(boat.m_YawDrag) ? std::max(boat.m_YawDrag, 0.0f) : 0.0f;
            const f32 immersionDepth = (std::isfinite(boat.m_ImmersionDepth) && boat.m_ImmersionDepth > 1.0e-3f) ? boat.m_ImmersionDepth : 0.5f;
            const f32 throttle = std::isfinite(boat.m_ThrottleInput) ? std::clamp(boat.m_ThrottleInput, -1.0f, 1.0f) : 0.0f;
            const f32 steer = std::isfinite(boat.m_SteerInput) ? std::clamp(boat.m_SteerInput, -1.0f, 1.0f) : 0.0f;

            const glm::quat rot = body->GetRotation();
            const glm::vec3 hullForward = rot * glm::vec3(0.0f, 0.0f, 1.0f);
            if (!IsFinite(hullForward))
                continue;

            // Thrust and hull drag act in the horizontal plane. A propeller
            // really pushes along its shaft, but letting a pitching hull aim its
            // thrust vertically is how a boat "planes into orbit" — the failure
            // mode this model exists to avoid. The off-centre application point
            // still produces the bow-up trim under power.
            glm::vec3 forwardFlat(hullForward.x, 0.0f, hullForward.z);
            const f32 forwardFlatLen = glm::length(forwardFlat);
            if (!(forwardFlatLen > kMinHorizontalForward))
                continue; // hull pointing straight up/down — no meaningful heading
            forwardFlat /= forwardFlatLen;
            // Right-handed with +Y up: starboard == forward x up, i.e. forward
            // rotated -90° about Y. For a +Z-forward hull that is local -X —
            // NOT +X, which is the port beam. See Scene/EntityFacing.h; getting
            // this backwards was half of issue #897.
            const glm::vec3 starboardFlat(-forwardFlat.z, 0.0f, forwardFlat.x);

            const glm::vec3 linVel = body->GetLinearVelocity();
            if (!IsFinite(linVel))
                continue;
            const f32 forwardSpeed = glm::dot(linVel, forwardFlat);

            const f32 mass = body->GetMass();
            const f32 dragMassScale = (std::isfinite(mass) && mass > 0.0f) ? mass : 1.0f;

            // --- Propeller thrust ---------------------------------------------
            // Immersion is sampled at the propeller itself, not the hull centre:
            // a stern lifted clear by a wave should lose bite, which is what
            // makes a boat feel like it is on water rather than on rails.
            const glm::vec3 thrustPoint = bodyPos + rot * glm::vec3(0.0f, thrustOffsetY, thrustOffsetZ);
            f32 propImmersion = 0.0f;
            if (IsFinite(thrustPoint))
            {
                const f32 propSurfaceY = WaterProbe::SampleSurfaceY(*water, glm::vec2(thrustPoint.x, thrustPoint.z), rawTime);
                if (std::isfinite(propSurfaceY))
                    propImmersion = ImmersionFraction(propSurfaceY, thrustPoint.y, immersionDepth);
            }

            if (propImmersion > 0.0f && maxThrust > 0.0f && std::abs(throttle) > 1.0e-4f)
            {
                const glm::vec3 thrust = forwardFlat * (maxThrust * throttle * propImmersion);
                if (IsFinite(thrust))
                    body->AddForce(thrust, thrustPoint, EForceMode::Force);
            }

            // --- Rudder --------------------------------------------------------
            // Authority is SIGNED by forward speed, so backing up reverses the
            // rudder (a real boat's stern swings the other way astern) and a boat
            // at rest cannot pivot on the spot. Yaw is taken about WORLD up, not
            // the hull's own up: a heeled hull yawing about its tilted axis would
            // feed roll, and buoyancy — not the rudder — owns the roll axis.
            const f32 rudderAuthority = std::clamp(forwardSpeed / rudderSpeed, -1.0f, 1.0f);

            // Hull immersion is measured at the KEEL, not at the body origin. A
            // boat in equilibrium floats with its origin AT the waterline, so
            // sampling there would leave hull drag flickering on and off with
            // every passing wave — the hull would stop tracking exactly when it
            // is behaving most normally. The buoyancy box already describes the
            // submerged hull, so take its bottom face when there is one; the
            // drive's own depth is the fallback for a boat authored without a
            // BuoyancyComponent.
            f32 keelOffsetY = std::min(thrustOffsetY, 0.0f);
            if (entity.HasComponent<BuoyancyComponent>())
            {
                const f32 probeHalfY = entity.GetComponent<BuoyancyComponent>().m_ProbeExtents.y;
                if (std::isfinite(probeHalfY) && probeHalfY > 0.0f)
                    keelOffsetY = -probeHalfY;
            }
            const glm::vec3 keelPoint = bodyPos + rot * glm::vec3(0.0f, keelOffsetY, 0.0f);
            f32 hullImmersion = 0.0f;
            if (IsFinite(keelPoint))
            {
                const f32 hullSurfaceY = WaterProbe::SampleSurfaceY(*water, glm::vec2(keelPoint.x, keelPoint.z), rawTime);
                if (std::isfinite(hullSurfaceY))
                    hullImmersion = ImmersionFraction(hullSurfaceY, keelPoint.y, immersionDepth);
            }

            if (maxRudderTorque > 0.0f && std::abs(steer) > 1.0e-4f && propImmersion > 0.0f)
            {
                // +Y torque takes the hull's +Z bow toward +X, which is the PORT
                // beam — so a starboard helm order (steer > 0) is a NEGATIVE
                // torque about world up. Issue #897: this was positive, and a
                // player pressing "starboard" turned to port.
                const f32 yawTorque = -maxRudderTorque * steer * rudderAuthority * propImmersion;
                if (std::isfinite(yawTorque))
                    body->AddTorque(glm::vec3(0.0f, yawTorque, 0.0f));
            }

            if (hullImmersion <= 0.0f)
                continue; // hull clear of the water — nothing left to damp

            // --- Hull drag -----------------------------------------------------
            // Split the horizontal velocity into along-hull and across-hull
            // parts so the two can be damped at very different rates: that
            // asymmetry IS the hull. Mass-scaling makes the coefficients
            // mass-independent decay rates (same convention as
            // BuoyancyComponent's drag) and keeps Jolt's integration stable.
            {
                const glm::vec3 horizontalVel(linVel.x, 0.0f, linVel.z);
                const f32 alongSpeed = glm::dot(horizontalVel, forwardFlat);
                const f32 acrossSpeed = glm::dot(horizontalVel, starboardFlat);

                const glm::vec3 dragForce =
                    forwardFlat * (-alongSpeed * forwardDrag * hullImmersion * dragMassScale) +
                    starboardFlat * (-acrossSpeed * lateralDrag * hullImmersion * dragMassScale);
                if (IsFinite(dragForce) && glm::dot(dragForce, dragForce) > 0.0f)
                    body->AddForce(dragForce, EForceMode::Force);
            }

            if (yawDrag > 0.0f)
            {
                const glm::vec3 angVel = body->GetAngularVelocity();
                if (IsFinite(angVel))
                {
                    // Only the yaw axis: pitch/roll damping is BuoyancyComponent's
                    // m_AngularDrag, and damping them twice would deaden the
                    // wave response the buoyancy probes exist to produce.
                    const f32 yawTorque = -angVel.y * yawDrag * hullImmersion * dragMassScale;
                    if (std::isfinite(yawTorque))
                        body->AddTorque(glm::vec3(0.0f, yawTorque, 0.0f));
                }
            }
        }
    }
} // namespace OloEngine
