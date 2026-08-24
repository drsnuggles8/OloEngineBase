#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/SailSystem.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Wind/WindSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Below this the "horizontal forward" projection is meaningless (the hull
        // is pointing straight up or down) — skip rather than normalizing a
        // near-zero vector into garbage. Same threshold, same reason, as
        // BoatSystem.
        constexpr f32 kMinHorizontalForward = 1.0e-3f;

        // Apparent wind below this (m/s) is treated as flat calm. Dividing by it
        // to get a direction would amplify float noise into a randomly-swinging
        // yard, which on screen looks like the rig is possessed rather than like
        // a boat becalmed.
        constexpr f32 kMinApparentWind = 1.0e-3f;

        [[nodiscard("finiteness result must be used")]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        /// `value` if finite and inside [lo, hi] after clamping, else `fallback`.
        [[nodiscard("sanitized value must be used")]] f32 Sanitized(f32 value, f32 lo, f32 hi, f32 fallback)
        {
            return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
        }

        /// Zero the force readouts and mark the sail as not driving, without
        /// touching m_YardAngle — a sail that stops drawing (calm, disabled,
        /// hull pointing at the sky) should leave the yard where the crew left
        /// it rather than snapping it square.
        void PublishNoDrive(SailComponent& sail, f32 apparentSpeed, f32 apparentAngle)
        {
            sail.m_ApparentWindSpeed = apparentSpeed;
            sail.m_ApparentWindAngle = apparentAngle;
            sail.m_DriveForce = 0.0f;
            sail.m_HeelForce = 0.0f;
            sail.m_Luffing = true;
        }
    } // namespace

    void SailSystem::OnUpdate(Scene* scene, f32 rawTime, f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !std::isfinite(rawTime))
            return;

        // A negative or non-finite step would run the yard slew backwards; a
        // zero step is legal (the sail simply holds its trim for a tick).
        const f32 dt = (std::isfinite(deltaTime) && deltaTime > 0.0f) ? deltaTime : 0.0f;

        JoltScene* jolt = scene->GetPhysicsScene();
        if (!jolt)
            return;

        auto view = scene->GetAllEntitiesWith<TransformComponent, SailComponent, Rigidbody3DComponent>();
        if (view.begin() == view.end())
            return;

        // The scene's wind is the only wind there is. WindSystem::GetWindAtPoint
        // returns exactly zero while WindSettings::Enabled is false, so a scene
        // with the wind switched off gives every sail a flat calm rather than a
        // special case in here.
        const WindSettings& windSettings = scene->GetWindSettings();

        for (auto e : view)
        {
            Entity entity{ e, scene };
            auto& sail = entity.GetComponent<SailComponent>();
            if (!sail.m_Enabled)
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }

            if (entity.GetComponent<Rigidbody3DComponent>().m_Type != BodyType3D::Dynamic)
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }

            Ref<JoltBody> body = jolt->GetBody(entity);
            if (!body || !body->IsDynamic())
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }

            const glm::vec3 bodyPos = body->GetPosition();
            const glm::vec3 linVel = body->GetLinearVelocity();
            if (!IsFinite(bodyPos) || !IsFinite(linVel))
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }

            // --- Sanitize tunables (defence in depth: every one of these can
            // reach us straight off disk or out of a script) ------------------
            const f32 rho = Sanitized(sail.m_AirDensity, 0.0f, 100.0f, 1.225f);
            const f32 cnMax = Sanitized(sail.m_MaxNormalCoefficient, 0.0f, 100.0f, 1.5f);
            const f32 sailSet = Sanitized(sail.m_SailSetInput, 0.0f, 1.0f, 1.0f);
            const f32 area = Sanitized(sail.m_SailArea, 0.0f, 100000.0f, 38.0f) * sailSet;
            const f32 maxYard = glm::radians(Sanitized(sail.m_MaxYardAngleDeg, 0.0f, 90.0f, 45.0f));
            const f32 trimRate = glm::radians(Sanitized(sail.m_TrimRateDeg, 0.0f, 3600.0f, 35.0f));
            const f32 coeY = Sanitized(sail.m_CentreOfEffortY, -1000.0f, 1000.0f, 2.6f);
            const f32 coeZ = Sanitized(sail.m_CentreOfEffortZ, -1000.0f, 1000.0f, -0.36f);
            const f32 trimInput = Sanitized(sail.m_TrimInput, -1.0f, 1.0f, 0.0f);

            // A garbage yard angle would otherwise persist forever: nothing else
            // writes it, so one NaN would poison the slew for the rest of the run.
            f32 yardAngle = Sanitized(sail.m_YardAngle, -glm::half_pi<f32>(), glm::half_pi<f32>(), 0.0f);

            const glm::quat rot = body->GetRotation();
            const glm::vec3 hullForward = rot * glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 hullUp = rot * glm::vec3(0.0f, 1.0f, 0.0f);
            if (!IsFinite(hullForward) || !IsFinite(hullUp))
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }

            // The sail force is resolved in the HORIZONTAL plane: wind is
            // horizontal, and letting a pitching rig aim its drive at the sky is
            // the "boat planes into orbit" failure BoatSystem's thrust avoids for
            // the same reason. The heeling moment still comes out right, because
            // it comes from the centre of effort being high, not from the force
            // being tilted.
            glm::vec3 forwardFlat(hullForward.x, 0.0f, hullForward.z);
            const f32 forwardFlatLen = glm::length(forwardFlat);
            if (!(forwardFlatLen > kMinHorizontalForward))
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }
            forwardFlat /= forwardFlatLen;
            // Starboard == forward x up, which for a +Z-forward hull is local -X.
            // Getting this backwards was half of issue #897; see
            // Scene/EntityFacing.h, which is where the convention lives.
            const glm::vec3 starboardFlat(-forwardFlat.z, 0.0f, forwardFlat.x);
            const glm::vec3 portFlat = -starboardFlat;

            // --- Apparent wind -------------------------------------------------
            // Sampled at the centre of effort, not at the hull origin: that is
            // where the rig is, and it is what makes a gust field mean anything.
            const glm::vec3 coePoint = bodyPos + rot * glm::vec3(0.0f, coeY, coeZ);
            if (!IsFinite(coePoint))
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }

            const glm::vec3 trueWind = WindSystem::GetWindAtPoint(windSettings, coePoint, rawTime);
            if (!IsFinite(trueWind))
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }

            // The wind the sail feels is the wind MINUS the boat's own motion.
            // This is the term that makes a dead run self-limiting: the faster
            // you go downwind the less wind there is left to push you.
            const glm::vec3 apparent(trueWind.x - linVel.x, 0.0f, trueWind.z - linVel.z);
            const f32 apparentSpeed = glm::length(apparent);
            if (!(apparentSpeed > kMinApparentWind))
            {
                PublishNoDrive(sail, 0.0f, 0.0f);
                continue;
            }
            const glm::vec3 apparentDir = apparent / apparentSpeed; // where the air is GOING

            // Reported bearing of the wind SOURCE relative to the bow: 0 dead
            // ahead, +/-pi a dead run, positive on the starboard side.
            const glm::vec3 windFrom = -apparentDir;
            const f32 apparentAngle = std::atan2(glm::dot(windFrom, starboardFlat), glm::dot(windFrom, forwardFlat));

            // --- Brace the yard -------------------------------------------------
            // The sail normal at yard angle phi is
            //     n(phi) = cos(phi) * forward + sin(phi) * port,
            // so with a = apparentDir . forward and b = apparentDir . port the
            // forward drive is
            //     D(phi) = q * A * Cn * (a cos phi + b sin phi) * cos phi
            //            = q * A * Cn * (a + a cos 2phi + b sin 2phi) / 2,
            // which is a single cosine in 2phi over phi in (-90, 90] degrees.
            // It peaks where (cos 2phi, sin 2phi) lines up with (a, b), i.e. at
            // phi = atan2(b, a) / 2 — one atan2, no search. Because D is unimodal
            // over that interval, clamping the unconstrained optimum into the
            // yard's bracing limit gives the CONSTRAINED optimum too.
            //
            // The maximum value is q*A*Cn*(a + sqrt(a^2 + b^2))/2, which is zero
            // exactly when b == 0 and a < 0: the wind dead on the bow. That is
            // the no-go zone, and it is arithmetic rather than a special case.
            const f32 alongBow = glm::dot(apparentDir, forwardFlat);
            const f32 alongPort = glm::dot(apparentDir, portFlat);

            // Forward-drive coefficient at yard angle phi, i.e. D(phi) without
            // the q*A*Cn scale. Sign is what matters at the call site.
            const auto driveCoefficient = [alongBow, alongPort](f32 phi)
            {
                const f32 c = std::cos(phi);
                return (alongBow * c + alongPort * std::sin(phi)) * c;
            };

            // Auto-trim solves for the brace that makes the most forward drive;
            // manual trim takes the driver's command straight. Either way this is
            // only a TARGET — the slew below is what the yard actually reaches.
            const f32 yardTarget =
                sail.m_AutoTrim
                    ? std::clamp(0.5f * std::atan2(alongPort, alongBow), -maxYard, maxYard)
                    : trimInput * maxYard;

            bool chaseTarget = true;
            if (sail.m_AutoTrim)
            {
                // HEAD TO WIND, HOLD WHAT YOU HAVE. With the wind on the bow the
                // solved optimum is the LEAST-BAD angle, not a good one, and its
                // sign is decided by which side of dead-ahead the apparent wind
                // has wandered onto — so a boat pointed straight into it would
                // slam the yard hard across every time that flickered. Once no
                // reachable brace can produce drive there is nothing to chase:
                // leave the yard where the crew left it and let the sail flog.
                // The boat still gets the (negative) force, which is the sail
                // pushing it astern — correct, and the reason a boat in irons
                // has to be backed out rather than sailed out.
                //
                // Feathering (bracing edge-on to spill) would brake less and is
                // what a crew would do, but the angle that spills passes through
                // +/-90 degrees, so clamping it to the yard's travel puts the
                // SAME sign flip back. There is no stable chase here; holding is
                // the primitive.
                //
                // The other way into this branch is being OVERPOWERED: a rig too
                // big for its hull drives the boat past the speed the yard limit
                // caps it at, the apparent wind draws forward of the braced sail,
                // and it goes aback and brakes hard. That is real behaviour, not
                // a bug — the answer is the one a crew would reach for, which is
                // m_SailSetInput (reef), not a bigger clamp. It is also easy to
                // author by accident: SailTest's fixture comment records a rig
                // that did exactly this and made the over-canvassed boat SLOWER
                // than a reefed one.
                if (!(driveCoefficient(yardTarget) > 0.0f))
                    chaseTarget = false;
            }

            if (chaseTarget && std::isfinite(yardTarget))
            {
                const f32 maxStep = trimRate * dt;
                const f32 delta = std::clamp(yardTarget - yardAngle, -maxStep, maxStep);
                if (std::isfinite(delta))
                    yardAngle += delta;
            }
            sail.m_YardAngle = std::clamp(yardAngle, -glm::half_pi<f32>(), glm::half_pi<f32>());

            // --- Sail force -----------------------------------------------------
            const glm::vec3 sailNormal = forwardFlat * std::cos(sail.m_YardAngle) + portFlat * std::sin(sail.m_YardAngle);
            // Signed: the sign is which side of the sail the wind is on, and
            // multiplying it back into the normal below is what points the force
            // downwind without a separate sign test. Edge-on to the wind this is
            // zero and the sail luffs.
            const f32 normalCosine = glm::dot(apparentDir, sailNormal);

            // A heeled rig spills wind: the sail presents less of itself to a
            // horizontal airflow the further it lies over. Real, not a fudge —
            // and it is the term that lets a gust knock the boat down without
            // rolling it all the way past recovery, because the force fades as
            // the angle grows.
            const f32 heelFactor = std::max(hullUp.y, 0.0f);

            const f32 dynamicPressure = 0.5f * rho * apparentSpeed * apparentSpeed;
            const f32 forceMagnitude = dynamicPressure * area * cnMax * normalCosine * heelFactor;
            const glm::vec3 sailForce = sailNormal * forceMagnitude;
            if (!IsFinite(sailForce))
            {
                PublishNoDrive(sail, apparentSpeed, apparentAngle);
                continue;
            }

            sail.m_ApparentWindSpeed = apparentSpeed;
            sail.m_ApparentWindAngle = apparentAngle;
            sail.m_DriveForce = glm::dot(sailForce, forwardFlat);
            sail.m_HeelForce = glm::dot(sailForce, starboardFlat);
            sail.m_Luffing = !(sail.m_DriveForce > 0.0f);

            if (glm::dot(sailForce, sailForce) > 0.0f)
            {
                // Applied AT the centre of effort, never at the centre of mass:
                // the offset is the whole point. Height gives heel, and the
                // fore-and-aft offset gives helm balance.
                body->AddForce(sailForce, coePoint, EForceMode::Force);
            }
        }
    }
} // namespace OloEngine
