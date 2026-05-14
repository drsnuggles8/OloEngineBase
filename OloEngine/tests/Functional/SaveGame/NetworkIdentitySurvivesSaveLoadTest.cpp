#include "OloEnginePCH.h"

// =============================================================================
// NetworkIdentitySurvivesSaveLoadTest — Functional Test.
//
// Cross-subsystem seam under test:
//   NetworkIdentityComponent × SaveGameSerializer. Multi-player scenes
//   save mid-session and reload (e.g. world hosts who go down and bring
//   the server back). The replication-id state — OwnerClientID,
//   Authority, IsReplicated flag — must round-trip so the client/server
//   relationship doesn't get reshuffled. A regression looks like
//   "everyone became Server-authoritative after the world reload" or
//   "NetworkIdentity was dropped entirely so replication stops sending".
//
// Scenario: build a scene with three NetworkIdentityComponent entities,
// each with distinct authority/owner/replicated config. Capture, restore,
// assert each entity's network identity matches the original.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class NetworkIdentitySurvivesSaveLoadTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_ServerActor = GetScene().CreateEntity("ServerActor");
        {
            NetworkIdentityComponent ni;
            ni.OwnerClientID = 0;
            ni.Authority = ENetworkAuthority::Server;
            ni.IsReplicated = true;
            m_ServerActor.AddComponent<NetworkIdentityComponent>(ni);
        }

        m_ClientActor = GetScene().CreateEntity("ClientActor");
        {
            NetworkIdentityComponent ni;
            ni.OwnerClientID = 42;
            ni.Authority = ENetworkAuthority::Client;
            ni.IsReplicated = true;
            m_ClientActor.AddComponent<NetworkIdentityComponent>(ni);
        }

        m_LocalProp = GetScene().CreateEntity("LocalProp");
        {
            NetworkIdentityComponent ni;
            ni.OwnerClientID = 0;
            ni.Authority = ENetworkAuthority::Server;
            ni.IsReplicated = false; // local-only prop
            m_LocalProp.AddComponent<NetworkIdentityComponent>(ni);
        }
    }

    Entity m_ServerActor, m_ClientActor, m_LocalProp;
};

TEST_F(NetworkIdentitySurvivesSaveLoadTest, AuthorityOwnerAndReplicatedFlagRoundTripCorrectly)
{
    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    auto verify = [&](Entity src, ENetworkAuthority expectedAuth, u32 expectedOwner, bool expectedReplicated) {
        Entity restoredEntity = restored->FindEntityByName(src.GetComponent<TagComponent>().Tag);
        ASSERT_TRUE(restoredEntity)
            << "entity '" << src.GetComponent<TagComponent>().Tag << "' missing from restored scene";
        ASSERT_TRUE(restoredEntity.HasComponent<NetworkIdentityComponent>())
            << "restored entity has no NetworkIdentityComponent — round-trip dropped it";
        const auto& ni = restoredEntity.GetComponent<NetworkIdentityComponent>();
        EXPECT_EQ(ni.Authority, expectedAuth) << "Authority mismatch on " << src.GetComponent<TagComponent>().Tag;
        EXPECT_EQ(ni.OwnerClientID, expectedOwner) << "OwnerClientID mismatch on " << src.GetComponent<TagComponent>().Tag;
        EXPECT_EQ(ni.IsReplicated, expectedReplicated) << "IsReplicated mismatch on " << src.GetComponent<TagComponent>().Tag;
    };

    verify(m_ServerActor, ENetworkAuthority::Server, 0u, true);
    verify(m_ClientActor, ENetworkAuthority::Client, 42u, true);
    verify(m_LocalProp,   ENetworkAuthority::Server, 0u, false);
}
