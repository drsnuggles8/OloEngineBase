#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
//
// =============================================================================
// InterpolatedComponentsSmoothOnRemoteClientTest — Functional Test (issue #462).
//
// Cross-subsystem seam under test:
//   Physics3D × Networking (EntitySnapshot capture × SnapshotInterpolator). The
//   issue's acceptance criterion is "a networked rotating / physics-driven entity
//   moves smoothly on a remote client (no per-snapshot popping)". Before #462 the
//   interpolator blended only TransformComponent; the generalization routes every
//   registered interpolatable component (transform + rigidbody velocity + …)
//   through a per-component policy table. This test proves the *integrated* path:
//   a server body that physics moves AND rotates, serialised through the real
//   EntitySnapshot, then interpolated on a separate client scene whose mirror
//   entity has no physics of its own.
//
// Scenario: a dynamic sphere is launched with horizontal + angular velocity and
// falls under gravity. Two server snapshots are captured a few ticks apart (so
// translation, rotation and velocity all differ between them). On the client we
// sweep the interpolation factor 0.25 → 0.5 → 0.75 (via render delay) and assert
// the mirror entity's translation/rotation move *smoothly between* the two server
// states — strictly interior, monotonic, never snapping to either endpoint.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/SnapshotInterpolator.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr u64 kBodyUUID = 0xB0D1;

    [[nodiscard]] bool IsBetween(f32 value, f32 a, f32 b, f32 tol)
    {
        const f32 lo = std::min(a, b) - tol;
        const f32 hi = std::max(a, b) + tol;
        return value >= lo && value <= hi;
    }
} // namespace

class InterpolatedComponentsSmoothOnRemoteClientTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Server-side dynamic body: launched sideways and spinning, mid-air so a
        // few ticks of gravity give a non-trivial pose delta to replicate.
        m_ServerBody = GetScene().CreateEntityWithUUID(UUID(kBodyUUID), "Server.Body");
        auto& tc = m_ServerBody.GetComponent<TransformComponent>();
        tc.Translation = { 0.0f, 6.0f, 0.0f };

        auto& body = m_ServerBody.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_InitialLinearVelocity = { 4.0f, 0.0f, 0.0f };  // horizontal drift
        body.m_InitialAngularVelocity = { 0.0f, 3.0f, 0.0f }; // spin about Y

        auto& col = m_ServerBody.AddComponent<SphereCollider3DComponent>();
        col.m_Radius = 0.5f;

        auto& nic = m_ServerBody.AddComponent<NetworkIdentityComponent>();
        nic.IsReplicated = true;
        nic.Authority = ENetworkAuthority::Server;

        EnablePhysics3D();
    }

    Entity m_ServerBody;
};

TEST_F(InterpolatedComponentsSmoothOnRemoteClientTest, RotatingPhysicsEntityInterpolatesSmoothlyOnClient)
{
    // Phase 1: let the body get moving, then capture two server snapshots a few
    // simulated ticks apart through the *real* EntitySnapshot path.
    TickFor(/*seconds=*/0.2f);
    const std::vector<u8> snapshotBefore = EntitySnapshot::Capture(GetScene());
    const TransformComponent serverBefore = m_ServerBody.GetComponent<TransformComponent>();

    TickFor(/*seconds=*/0.2f);
    const std::vector<u8> snapshotAfter = EntitySnapshot::Capture(GetScene());
    const TransformComponent serverAfter = m_ServerBody.GetComponent<TransformComponent>();

    ASSERT_FALSE(snapshotBefore.empty());
    ASSERT_FALSE(snapshotAfter.empty());

    // Sanity: physics actually moved AND rotated the body between captures —
    // otherwise interpolation has nothing to smooth and the test is vacuous.
    ASSERT_GT(std::abs(serverAfter.Translation.x - serverBefore.Translation.x), 0.05f)
        << "body did not drift horizontally — physics or initial velocity is broken";
    ASSERT_LT(serverAfter.Translation.y, serverBefore.Translation.y - 0.05f)
        << "body did not fall — gravity is broken";
    const f32 yawBefore = serverBefore.GetRotationEuler().y;
    const f32 yawAfter = serverAfter.GetRotationEuler().y;
    ASSERT_GT(std::abs(yawAfter - yawBefore), 0.01f)
        << "body did not rotate — angular velocity is broken";

    // Phase 2: a separate client scene with a mirror entity (same UUID, server
    // authority, NO physics — the classic remote-actor case).
    Ref<Scene> clientScene = Scene::Create();
    clientScene->SetRenderingEnabled(false);
    Entity mirror = clientScene->CreateEntityWithUUID(UUID(kBodyUUID), "Client.Mirror");
    auto& nic = mirror.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;
    nic.Authority = ENetworkAuthority::Server;
    mirror.GetComponent<TransformComponent>().Translation = { -50.0f, -50.0f, -50.0f }; // sentinel

    // Feed the two snapshots into the interpolator. tickRate 20, ticks 10 & 14:
    // renderTick = 14 - (renderDelay·20), so renderDelay tunes the blend factor
    // alpha = (renderTick - 10) / 4. We sweep alpha 0.25 → 0.5 → 0.75.
    SnapshotInterpolator interp;
    interp.SetServerTickRate(20);
    interp.PushSnapshot(10, snapshotBefore);
    interp.PushSnapshot(14, snapshotAfter);

    struct Sample
    {
        f32 Alpha;
        f32 RenderDelay;
    };
    const std::vector<Sample> sweep = {
        { 0.25f, 0.15f }, // renderTick 11
        { 0.50f, 0.10f }, // renderTick 12
        { 0.75f, 0.05f }, // renderTick 13
    };

    std::vector<f32> clientX;
    std::vector<f32> clientY;
    std::vector<f32> clientYaw;
    for (const auto& s : sweep)
    {
        interp.SetRenderDelay(s.RenderDelay);
        interp.Interpolate(*clientScene, 1.0f / 60.0f);

        const auto& mt = mirror.GetComponent<TransformComponent>();

        // The interpolated pose must lie *between* the two server states — proof
        // it blended rather than snapping to one snapshot (the pre-#462 risk).
        EXPECT_TRUE(IsBetween(mt.Translation.x, serverBefore.Translation.x, serverAfter.Translation.x, 1e-3f))
            << "alpha " << s.Alpha << ": client X " << mt.Translation.x << " escaped the bracket";
        EXPECT_TRUE(IsBetween(mt.Translation.y, serverBefore.Translation.y, serverAfter.Translation.y, 1e-3f))
            << "alpha " << s.Alpha << ": client Y " << mt.Translation.y << " escaped the bracket";

        // At alpha 0.5 the lerp lands on the translation midpoint.
        if (std::abs(s.Alpha - 0.5f) < 1e-4f)
        {
            const f32 midX = 0.5f * (serverBefore.Translation.x + serverAfter.Translation.x);
            EXPECT_NEAR(mt.Translation.x, midX, 1e-2f) << "alpha 0.5 should hit the X midpoint";
        }

        clientX.push_back(mt.Translation.x);
        clientY.push_back(mt.Translation.y);
        clientYaw.push_back(mt.GetRotationEuler().y);
    }

    // Phase 3: no per-snapshot popping — as alpha increases the client pose
    // progresses monotonically toward the newer snapshot, a smooth slide rather
    // than a stair-step that jumps only at snapshot boundaries.
    const bool xIncreasing = serverAfter.Translation.x > serverBefore.Translation.x;
    for (sizet i = 1; i < clientX.size(); ++i)
    {
        if (xIncreasing)
        {
            EXPECT_GT(clientX[i], clientX[i - 1]) << "client X must advance smoothly across the sweep";
        }
        else
        {
            EXPECT_LT(clientX[i], clientX[i - 1]) << "client X must advance smoothly across the sweep";
        }
        // Body is falling — Y strictly decreases across the sweep.
        EXPECT_LT(clientY[i], clientY[i - 1]) << "client Y must descend smoothly across the sweep";
    }

    // Rotation is slerped, not snapped: the mid sample's yaw sits between the two
    // server yaws (interior), confirming the slerp policy fired.
    EXPECT_TRUE(IsBetween(clientYaw[1], yawBefore, yawAfter, 1e-2f))
        << "client yaw " << clientYaw[1] << " should slerp between " << yawBefore << " and " << yawAfter;

    clientScene.Reset();
}
