#include "OloEnginePCH.h"

// =============================================================================
// PhysicsTransformReplicationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Physics3D × Networking. The existing ComponentReplicatorTest round-trips
//   default-constructed components; the existing physics tests never go
//   through the network archive. The integration concern — "a Transform
//   that physics has been modifying for several frames survives the net
//   archive" — is uncovered. Replication uses a different code path than
//   memory-archive serialisation (ArIsNetArchive=true), and bugs there
//   only show up in real client/server play.
//
// Scenario: a dynamic sphere falls under gravity for 0.5s of simulated
// time; the post-tick TransformComponent is then serialised through the
// net archive and deserialised into a fresh component on a second
// "client-side" entity. Assert the round-trip preserves the physics-driven
// values within net-archive precision.
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

class PhysicsTransformReplicationTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Static floor — present so the body falls toward something
        // realistic, not toward infinity (which would make NaN harder to
        // distinguish from "still falling").
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        // Server-side dynamic body. Mid-air start so 0.5s of physics gives
        // a meaningfully non-trivial transform to replicate.
        m_ServerBody = GetScene().CreateEntity("ServerBody");
        auto& serverTransform = m_ServerBody.GetComponent<TransformComponent>();
        serverTransform.Translation = { 1.25f, 5.0f, -2.5f };
        serverTransform.SetRotationEuler({ 0.0f, 0.7853f, 0.0f }); // 45deg around Y
        auto& body = m_ServerBody.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        auto& col = m_ServerBody.AddComponent<SphereCollider3DComponent>();
        col.m_Radius = 0.5f;

        EnablePhysics3D();
    }

    Entity m_ServerBody;
};

TEST_F(PhysicsTransformReplicationTest, NetArchiveRoundTripPreservesPostPhysicsTransform)
{
    // Tick long enough for physics to non-trivially move the body and
    // potentially also collide with the floor — a realistic mid-game
    // replication moment.
    TickFor(/*seconds=*/0.5f);

    // Copy out as a mutable local because the replicator's Serialize takes a
    // non-const ref (it's the same code path for read and write).
    TransformComponent server = m_ServerBody.GetComponent<TransformComponent>();

    // Sanity: physics actually moved the body. If this fails, EnablePhysics3D
    // didn't wire the body and the rest of the test would be vacuous.
    ASSERT_LT(server.Translation.y, 5.0f - 0.1f)
        << "body did not fall — physics tick is broken or body never bound";
    ASSERT_TRUE(std::isfinite(server.Translation.x) &&
                std::isfinite(server.Translation.y) &&
                std::isfinite(server.Translation.z))
        << "server transform contains NaN/Inf after physics tick";

    // Serialise through the *network* archive (different code path from
    // the memory archive used by SaveGame).
    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;
    ComponentReplicator::Serialize(writer, server);
    ASSERT_FALSE(writer.IsError()) << "net-archive write failed";
    ASSERT_GT(buffer.size(), 0u) << "net-archive produced empty payload";

    // Deserialise into a fresh "client-side" component.
    TransformComponent client;
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    ComponentReplicator::Serialize(reader, client);
    ASSERT_FALSE(reader.IsError()) << "net-archive read failed";

    // Net archives may quantise; tolerate ~1e-3 (well below visual precision).
    constexpr f32 kNetTolerance = 1e-3f;
    EXPECT_NEAR(client.Translation.x, server.Translation.x, kNetTolerance);
    EXPECT_NEAR(client.Translation.y, server.Translation.y, kNetTolerance);
    EXPECT_NEAR(client.Translation.z, server.Translation.z, kNetTolerance);

    const auto serverEuler = server.GetRotationEuler();
    const auto clientEuler = client.GetRotationEuler();
    EXPECT_NEAR(clientEuler.x, serverEuler.x, kNetTolerance);
    EXPECT_NEAR(clientEuler.y, serverEuler.y, kNetTolerance);
    EXPECT_NEAR(clientEuler.z, serverEuler.z, kNetTolerance);

    EXPECT_NEAR(client.Scale.x, server.Scale.x, kNetTolerance);
    EXPECT_NEAR(client.Scale.y, server.Scale.y, kNetTolerance);
    EXPECT_NEAR(client.Scale.z, server.Scale.z, kNetTolerance);
}
