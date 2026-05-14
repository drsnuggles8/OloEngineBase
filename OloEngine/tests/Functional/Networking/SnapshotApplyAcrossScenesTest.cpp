#include "OloEnginePCH.h"

// =============================================================================
// SnapshotApplyAcrossScenesTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Server Scene × Physics3D × ComponentReplicator × Client Scene. The
//   PhysicsTransformReplicationTest already covers a single component's
//   net-archive round-trip; this tests the integrated path: a server scene
//   with a live entity simulating physics, a snapshot serialised mid-tick,
//   then applied to a *different* scene's entity (the client). A regression
//   that breaks "server simulates, client mirrors" — net archive losing
//   precision after physics, snapshot ordering, applying components in the
//   wrong order — only shows up when the round-trip happens across two
//   independent Scene instances.
//
// Scenario: build a server scene, tick physics for 0.5s. Serialise the
// post-tick TransformComponent via the net archive. Build a separate
// client scene with a fresh entity. Deserialise into the client entity's
// component. Assert the client transform matches the server within
// net-archive precision and the two scenes remain independent (server
// continues to simulate, client transform stays put).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Serialization/Archive.h"

#include <cmath>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

class SnapshotApplyAcrossScenesTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Server scene: harness-owned. Static floor + dynamic body.
        auto floor = GetScene().CreateEntity("Server.Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        BoxCollider3DComponent fc;
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(fc);
        Rigidbody3DComponent fb;
        fb.m_Type = BodyType3D::Static;
        floor.AddComponent<Rigidbody3DComponent>(fb);

        m_ServerBody = GetScene().CreateEntity("Server.Body");
        m_ServerBody.GetComponent<TransformComponent>().Translation = { 0.0f, 8.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.5f;
        m_ServerBody.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        m_ServerBody.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();
    }

    Entity m_ServerBody;
};

TEST_F(SnapshotApplyAcrossScenesTest, ServerSnapshotMirrorsToClientSceneAndScenesStayIndependent)
{
    // Phase 1: drive the server scene for 0.5s. Body should fall noticeably.
    TickFor(/*seconds=*/0.5f);

    const TransformComponent serverPostTick = m_ServerBody.GetComponent<TransformComponent>();
    ASSERT_LT(serverPostTick.Translation.y, 8.0f - 0.5f)
        << "server body did not fall — physics tick is broken; the rest of the "
           "round-trip would be vacuous.";

    // Phase 2: serialise the server transform via the *network* archive.
    // Mutable copy because ComponentReplicator::Serialize takes a non-const ref
    // (it's the same code path for read and write).
    TransformComponent serverCopy = serverPostTick;
    std::vector<u8> snapshot;
    {
        FMemoryWriter writer(snapshot);
        writer.ArIsNetArchive = true;
        ComponentReplicator::Serialize(writer, serverCopy);
        ASSERT_FALSE(writer.IsError()) << "server-side snapshot write failed";
        ASSERT_GT(snapshot.size(), 0u)  << "snapshot payload is empty";
    }

    // Phase 3: build a client scene. Distinct Scene instance, distinct entity,
    // no physics on this side (client just mirrors transforms — typical
    // remote-actor pattern).
    Ref<Scene> clientScene = Scene::Create();
    clientScene->SetRenderingEnabled(false);
    Entity clientEntity = clientScene->CreateEntity("Client.Mirror");
    clientEntity.GetComponent<TransformComponent>().Translation = { 99.0f, 99.0f, 99.0f }; // Distinct sentinel

    // Phase 4: deserialise into the client entity's TransformComponent.
    {
        FMemoryReader reader(snapshot);
        reader.ArIsNetArchive = true;
        ComponentReplicator::Serialize(reader, clientEntity.GetComponent<TransformComponent>());
        ASSERT_FALSE(reader.IsError()) << "client-side snapshot read failed";
    }

    // Phase 5: client transform mirrors server within net-archive precision.
    constexpr f32 kNetTolerance = 1e-3f;
    const auto& clientT = clientEntity.GetComponent<TransformComponent>();
    EXPECT_NEAR(clientT.Translation.x, serverPostTick.Translation.x, kNetTolerance);
    EXPECT_NEAR(clientT.Translation.y, serverPostTick.Translation.y, kNetTolerance)
        << "client did not mirror server's post-physics y";
    EXPECT_NEAR(clientT.Translation.z, serverPostTick.Translation.z, kNetTolerance);

    // Phase 6: scenes remain independent. Tick the server another 0.5s and
    // tick the (physics-less) client by the same dt. Server's body should
    // have moved further; client's transform should be exactly what we
    // applied (no server contamination through some shared singleton).
    const f32 clientYSnapshot = clientT.Translation.y;
    const f32 serverYBeforePhase6 = serverPostTick.Translation.y;

    TickFor(/*seconds=*/0.5f); // server (harness scene) ticks via the harness
    {
        const f32 dt = 1.0f / 60.0f;
        for (u32 i = 0; i < 30; ++i) clientScene->OnUpdateRuntime(dt);
    }

    const f32 serverYAfter = m_ServerBody.GetComponent<TransformComponent>().Translation.y;
    const f32 clientYAfter = clientEntity.GetComponent<TransformComponent>().Translation.y;

    EXPECT_LT(serverYAfter, serverYBeforePhase6)
        << "server simulation halted after the snapshot — possible global state corruption";
    EXPECT_NEAR(clientYAfter, clientYSnapshot, 1e-4f)
        << "client transform changed without a body or replication — scene isolation is broken";

    // Clean up client scene before the test exits so its physics shutdown
    // ordering is well-defined relative to the harness's scene.
    clientScene.Reset();
}
