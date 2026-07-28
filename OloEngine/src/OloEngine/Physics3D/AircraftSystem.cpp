#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/AircraftSystem.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Below this airspeed there is no meaningful relative wind: skip lift,
        // drag and the weathervane rather than normalizing a near-zero vector.
        // Thrust and rate damping still apply, so a parked aircraft can spool up.
        constexpr f32 kMinAirspeed = 1.0e-2f;
        // Guard for the lift-direction cross product: at extreme sideslip the
        // wing axis is nearly parallel to the airflow and the direction is
        // undefined. Skipping lift there is correct — a wing edge-on to the
        // airflow genuinely produces none.
        constexpr f32 kMinLiftAxisLength = 1.0e-4f;
        // Matches JoltScene's world gravity; only used to express the landing-gear
        // spring rate as a multiple of the aircraft's own weight.
        constexpr f32 kGravity = 9.81f;
        // Slip speed (m/s) at which a tyre reaches its full friction coefficient.
        // Regularising over a small band instead of using sign(slip) is what stops
        // a parked wheel chattering across the zero crossing every tick.
        constexpr f32 kTyreSlipReference = 0.5f;

        [[nodiscard("finiteness result must be used")]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        [[nodiscard("sanitized value must be used")]] f32 SanitizedNonNegative(f32 value, f32 fallback)
        {
            return (std::isfinite(value) && value >= 0.0f) ? value : fallback;
        }

        /// One sprung leg: where it attaches in body-local space, and how far it
        /// can extend before the wheel is off the ground.
        struct GearLeg
        {
            glm::vec3 m_LocalAttach{ 0.0f };
            f32 m_Length = 1.0f;
        };

        /// Lift coefficient at `alpha` radians, with a symmetric post-stall
        /// falloff: linear up to the stall angle, then decaying to zero over the
        /// same angle again. Without the falloff, hauling the nose up would keep
        /// producing MORE lift the harder you pull — the classic toy flight model
        /// that can loop forever and never departs.
        [[nodiscard("lift coefficient must be used")]] f32 LiftCoefficient(f32 alpha, f32 zeroLift, f32 slope, f32 stallRad)
        {
            f32 cl = zeroLift + slope * alpha;
            const f32 absAlpha = std::abs(alpha);
            if (absAlpha > stallRad)
            {
                const f32 over = (absAlpha - stallRad) / stallRad;
                cl *= std::max(0.0f, 1.0f - over);
            }
            return cl;
        }
    } // namespace

    void AircraftSystem::OnUpdate(Scene* scene, [[maybe_unused]] f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
            return;

        JoltScene* jolt = scene->GetPhysicsScene();
        if (!jolt)
            return;

        auto view = scene->GetAllEntitiesWith<TransformComponent, AircraftComponent, Rigidbody3DComponent>();
        for (auto e : view)
        {
            Entity entity{ e, scene };
            const auto& aircraft = entity.GetComponent<AircraftComponent>();
            if (!aircraft.m_Enabled)
                continue;

            if (entity.GetComponent<Rigidbody3DComponent>().m_Type != BodyType3D::Dynamic)
                continue;

            Ref<JoltBody> body = jolt->GetBody(entity);
            if (!body || !body->IsDynamic())
                continue;

            const glm::vec3 linVel = body->GetLinearVelocity();
            const glm::quat rot = body->GetRotation();
            if (!IsFinite(linVel))
                continue;

            const glm::vec3 forward = rot * glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 up = rot * glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 right = rot * glm::vec3(1.0f, 0.0f, 0.0f);
            if (!IsFinite(forward) || !IsFinite(up) || !IsFinite(right))
                continue;

            // --- Sanitize tunables (a script can write any raw field) ---------
            const f32 maxThrust = SanitizedNonNegative(aircraft.m_MaxThrust, 0.0f);
            const f32 wingArea = (std::isfinite(aircraft.m_WingArea) && aircraft.m_WingArea > 0.0f) ? aircraft.m_WingArea : 0.0f;
            const f32 airDensity = SanitizedNonNegative(aircraft.m_AirDensity, 1.225f);
            const f32 liftSlope = SanitizedNonNegative(aircraft.m_LiftSlope, 5.0f);
            const f32 zeroLift = std::isfinite(aircraft.m_ZeroLiftCoefficient) ? aircraft.m_ZeroLiftCoefficient : 0.0f;
            const f32 stallRad = glm::radians((std::isfinite(aircraft.m_StallAngleDeg) && aircraft.m_StallAngleDeg > 0.1f) ? aircraft.m_StallAngleDeg : 15.0f);
            const f32 cd0 = SanitizedNonNegative(aircraft.m_DragCoefficient, 0.0f);
            const f32 inducedK = SanitizedNonNegative(aircraft.m_InducedDragFactor, 0.0f);
            const f32 pitchTorque = SanitizedNonNegative(aircraft.m_PitchTorque, 0.0f);
            const f32 rollTorque = SanitizedNonNegative(aircraft.m_RollTorque, 0.0f);
            const f32 yawTorque = SanitizedNonNegative(aircraft.m_YawTorque, 0.0f);
            const f32 authoritySpeed = (std::isfinite(aircraft.m_ControlAuthoritySpeed) && aircraft.m_ControlAuthoritySpeed > 1.0e-2f) ? aircraft.m_ControlAuthoritySpeed : 40.0f;
            const f32 pitchDamping = SanitizedNonNegative(aircraft.m_PitchDamping, 0.0f);
            const f32 rollDamping = SanitizedNonNegative(aircraft.m_RollDamping, 0.0f);
            const f32 yawDamping = SanitizedNonNegative(aircraft.m_YawDamping, 0.0f);
            const f32 weathervane = SanitizedNonNegative(aircraft.m_WeathervaneStrength, 0.0f);
            const f32 throttle = std::isfinite(aircraft.m_ThrottleInput) ? std::clamp(aircraft.m_ThrottleInput, 0.0f, 1.0f) : 0.0f;
            const f32 pitchInput = std::isfinite(aircraft.m_PitchInput) ? std::clamp(aircraft.m_PitchInput, -1.0f, 1.0f) : 0.0f;
            const f32 rollInput = std::isfinite(aircraft.m_RollInput) ? std::clamp(aircraft.m_RollInput, -1.0f, 1.0f) : 0.0f;
            const f32 yawInput = std::isfinite(aircraft.m_YawInput) ? std::clamp(aircraft.m_YawInput, -1.0f, 1.0f) : 0.0f;

            const f32 mass = body->GetMass();
            const f32 dampingMassScale = (std::isfinite(mass) && mass > 0.0f) ? mass : 1.0f;

            glm::vec3 force(0.0f);
            glm::vec3 torque(0.0f);

            // --- Thrust --------------------------------------------------------
            // Along the nose, at the centre of mass: a centreline engine produces
            // no trim moment, and off-axis thrust is a per-airframe detail this
            // model deliberately leaves out.
            if (maxThrust > 0.0f && throttle > 0.0f)
                force += forward * (maxThrust * throttle);

            const f32 airspeed = glm::length(linVel);
            if (airspeed > kMinAirspeed)
            {
                const glm::vec3 velDir = linVel / airspeed;
                // Dynamic pressure q = 0.5 * rho * V^2 — the airspeed² term that
                // makes every aerodynamic force fall away as the aircraft slows.
                const f32 dynamicPressure = 0.5f * airDensity * airspeed * airspeed;

                // Angle of attack: the angle between the relative wind and the
                // wing chord. Positive = nose above the flight path.
                const f32 alongForward = glm::dot(linVel, forward);
                const f32 alongUp = glm::dot(linVel, up);
                const f32 alpha = std::atan2(-alongUp, alongForward);

                f32 cl = 0.0f;
                if (wingArea > 0.0f && std::isfinite(alpha))
                {
                    cl = LiftCoefficient(alpha, zeroLift, liftSlope, stallRad);

                    // Lift acts perpendicular to the airflow, in the aircraft's
                    // plane of symmetry. cross(velDir, right) reduces to the body
                    // up-axis in level flight and correctly tilts with bank —
                    // which is what makes a banked turn work at all.
                    const glm::vec3 liftAxis = glm::cross(velDir, right);
                    const f32 liftAxisLen = glm::length(liftAxis);
                    if (liftAxisLen > kMinLiftAxisLength)
                    {
                        const glm::vec3 liftDir = liftAxis / liftAxisLen;
                        const glm::vec3 lift = liftDir * (dynamicPressure * wingArea * cl);
                        if (IsFinite(lift))
                            force += lift;
                    }
                }

                // Drag: parasitic + induced. The Cl² term is why a hard turn (high
                // Cl) bleeds speed — the coupling that stops the model turning
                // for free.
                if (wingArea > 0.0f)
                {
                    const f32 cd = cd0 + inducedK * cl * cl;
                    const glm::vec3 drag = -velDir * (dynamicPressure * wingArea * cd);
                    if (IsFinite(drag))
                        force += drag;
                }

                // Static (weathercock) stability. cross(forward, velDir) is the
                // axis that rotates the nose onto the relative wind, with a
                // magnitude of sin(angle) — a natural small-angle restoring term.
                // It is perpendicular to `forward` by construction, so it has no
                // roll component and can never fight the ailerons.
                if (weathervane > 0.0f && wingArea > 0.0f)
                {
                    const glm::vec3 align = glm::cross(forward, velDir);
                    const glm::vec3 restoring = align * (weathervane * dynamicPressure * wingArea);
                    if (IsFinite(restoring))
                        torque += restoring;
                }
            }

            // --- Control surfaces ----------------------------------------------
            // Authority scales with airspeed: at a standstill the sticks do
            // nothing, which is both realistic and what stops a parked aircraft
            // spinning itself in place.
            const f32 authority = std::clamp(airspeed / authoritySpeed, 0.0f, 1.0f);
            if (authority > 0.0f)
            {
                // +X torque pitches the nose DOWN (it rotates +Z toward -Y), so
                // nose-up input is a NEGATIVE torque about the right wing.
                torque += -right * (pitchTorque * pitchInput * authority);
                // +Z torque rolls the right wing UP, so roll-right is negative.
                torque += -forward * (rollTorque * rollInput * authority);
                // +Y torque yaws the nose toward +X (right) — this one is direct.
                torque += up * (yawTorque * yawInput * authority);
            }

            // --- Rate damping ---------------------------------------------------
            // Per-body-axis, so a rolling aircraft is damped in roll without its
            // pitch response being deadened. Mass-scaled to keep the coefficients
            // mass-independent decay rates (same convention as BuoyancyComponent
            // and BoatComponent). Applies at ANY airspeed — this is the term that
            // keeps a stalled or stationary airframe from tumbling indefinitely.
            const glm::vec3 angVel = body->GetAngularVelocity();
            if (IsFinite(angVel))
            {
                torque += -right * (glm::dot(angVel, right) * pitchDamping * dampingMassScale);
                torque += -forward * (glm::dot(angVel, forward) * rollDamping * dampingMassScale);
                torque += -up * (glm::dot(angVel, up) * yawDamping * dampingMassScale);
            }

            // --- Landing gear -----------------------------------------------------
            // Three sprung, ray-cast legs. Each is applied AT ITS OWN CONTACT POINT
            // rather than at the centre of mass, which is the whole point: the
            // aircraft then pivots about the main gear (a few tens of cm behind the
            // CoM) instead of about the fuselage's rear edge, so the elevator has a
            // short enough moment arm to rotate for takeoff.
            //
            // Deliberately a spring model rather than leaving it to the fuselage
            // collider: the collider still exists and still catches a belly landing,
            // but with the gear extended it never reaches the ground, so ground
            // handling is governed by these legs alone.
            if (aircraft.m_HasLandingGear)
            {
                const f32 gearLength = (std::isfinite(aircraft.m_GearLength) && aircraft.m_GearLength > 0.01f) ? aircraft.m_GearLength : 1.2f;
                const f32 mainZ = std::isfinite(aircraft.m_MainGearOffsetZ) ? aircraft.m_MainGearOffsetZ : -0.6f;
                const f32 noseZ = std::isfinite(aircraft.m_NoseGearOffsetZ) ? aircraft.m_NoseGearOffsetZ : 2.5f;
                const f32 halfTrack = (std::isfinite(aircraft.m_MainGearHalfTrack) && aircraft.m_MainGearHalfTrack > 0.0f) ? aircraft.m_MainGearHalfTrack : 2.0f;
                const f32 stiffness = SanitizedNonNegative(aircraft.m_GearStiffness, 12.0f);
                const f32 gearDamping = std::clamp(std::isfinite(aircraft.m_GearDamping) ? aircraft.m_GearDamping : 0.5f, 0.0f, 1.0f);
                const f32 rollingResistance = SanitizedNonNegative(aircraft.m_GearRollingResistance, 0.0f);
                const f32 lateralGrip = SanitizedNonNegative(aircraft.m_GearLateralGrip, 0.0f);

                const std::array<GearLeg, 3> legs{ {
                    { glm::vec3(-halfTrack, 0.0f, mainZ), gearLength },
                    { glm::vec3(halfTrack, 0.0f, mainZ), gearLength },
                    { glm::vec3(0.0f, 0.0f, noseZ), gearLength },
                } };

                const glm::vec3 bodyPos = body->GetPosition();
                const f32 gearMass = (std::isfinite(mass) && mass > 0.0f) ? mass : 1.0f;
                // Per-leg share of the aircraft's weight — makes m_GearStiffness an
                // airframe-independent number instead of a raw N/m the designer
                // would have to re-derive for every mass.
                const f32 weightPerLeg = gearMass * kGravity / static_cast<f32>(legs.size());

                for (const GearLeg& leg : legs)
                {
                    const glm::vec3 attach = bodyPos + rot * leg.m_LocalAttach;
                    if (!IsFinite(attach))
                        continue;

                    // Cast straight down from the attachment. Excluding this entity
                    // stops the ray hitting the aircraft's own fuselage collider.
                    RayCastInfo ray(attach, glm::vec3(0.0f, -1.0f, 0.0f), leg.m_Length);
                    ray.m_ExcludedEntities.push_back(entity.GetUUID());
                    SceneQueryHit hit;
                    if (!jolt->CastRay(ray, hit) || !hit.HasHit())
                        continue; // wheel in the air — this leg carries nothing

                    const f32 compression = leg.m_Length - hit.m_Distance;
                    if (!(compression > 0.0f) || !std::isfinite(compression))
                        continue;

                    // Spring: proportional to compression. Damper: opposes only the
                    // component of the contact point's velocity along the leg, so it
                    // resists bouncing without fighting the roll-out.
                    const glm::vec3 contactVel = body->GetLinearVelocity() + glm::cross(body->GetAngularVelocity(), attach - bodyPos);
                    if (!IsFinite(contactVel))
                        continue;

                    const f32 springForce = compression * stiffness * weightPerLeg;
                    // Critical damping for a spring of this rate carrying this share
                    // of the mass; the ratio then scales it the usual 0..1 way.
                    const f32 criticalDamping = 2.0f * std::sqrt(std::max(stiffness * weightPerLeg, 0.0f) * (gearMass / static_cast<f32>(legs.size())));
                    const f32 damperForce = -contactVel.y * gearDamping * criticalDamping;
                    const f32 normalForce = std::max(springForce + damperForce, 0.0f); // a leg can push, never pull
                    if (!std::isfinite(normalForce) || normalForce <= 0.0f)
                        continue;

                    body->AddForce(glm::vec3(0.0f, normalForce, 0.0f), attach, EForceMode::Force);

                    // Tyre friction, split along/across the wheel so the gear can
                    // hold the aircraft on the centreline (high lateral grip) while
                    // barely resisting the takeoff roll (low rolling resistance).
                    //
                    // Both coefficients are true friction coefficients scaled by
                    // THIS LEG'S NORMAL FORCE, i.e. Coulomb — not a velocity decay
                    // scaled by the aircraft's mass. That distinction matters a lot:
                    // a mass-scaled viscous term grows without bound with speed, so
                    // it fought the takeoff roll harder the closer the aircraft got
                    // to flying speed (an effective rolling-resistance coefficient
                    // of ~0.5 — sand, not tarmac). Load-proportional friction also
                    // fades out on its own as the wing takes the weight, which is
                    // exactly what should happen during a rotation.
                    //
                    // The slip is regularised over kTyreSlipReference rather than
                    // used raw, so a stationary wheel doesn't chatter between
                    // full-force-left and full-force-right across the zero crossing.
                    glm::vec3 rollDir(forward.x, 0.0f, forward.z);
                    const f32 rollLen = glm::length(rollDir);
                    if (rollLen > 1.0e-3f)
                    {
                        rollDir /= rollLen;
                        const glm::vec3 sideDir(rollDir.z, 0.0f, -rollDir.x);
                        const glm::vec3 groundVel(contactVel.x, 0.0f, contactVel.z);

                        const auto tyreAxisForce = [&](const glm::vec3& axis, f32 coefficient)
                        {
                            const f32 slip = glm::dot(groundVel, axis);
                            const f32 saturation = std::clamp(slip / kTyreSlipReference, -1.0f, 1.0f);
                            return axis * (-saturation * coefficient * normalForce);
                        };

                        const glm::vec3 tyreForce = tyreAxisForce(rollDir, rollingResistance) +
                                                    tyreAxisForce(sideDir, lateralGrip);
                        if (IsFinite(tyreForce) && glm::dot(tyreForce, tyreForce) > 0.0f)
                            body->AddForce(tyreForce, attach, EForceMode::Force);
                    }
                }
            }

            if (IsFinite(force) && glm::dot(force, force) > 0.0f)
                body->AddForce(force, EForceMode::Force);
            if (IsFinite(torque) && glm::dot(torque, torque) > 0.0f)
                body->AddTorque(torque);
        }
    }
} // namespace OloEngine
