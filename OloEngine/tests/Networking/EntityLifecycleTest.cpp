#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// Contract tests for entity spawn/despawn replication — the piece that turns a
// snapshot stream (which assumes a fixed entity set) into a live world where the
// server can create and destroy things.
//
// The two behaviours worth pinning here are both "quiet if wrong":
//   * a spawn must carry enough state that the client never renders a frame of
//     default-constructed placeholder, and
//   * a despawn must destroy ONLY what the spawn path created. Destroying a
//     scene-authored entity because it left relevance would delete the level a
//     piece at a time, and the player would just see the world disappearing.

#include "OloEngine/Networking/Replication/ComponentInterpolationRegistry.h"
#include "OloEngine/Networking/Replication/EntityLifecycle.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace OloEngine;

class EntityLifecycleTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ComponentInterpolationRegistry::RegisterDefaults();
        NetworkSpawnRegistry::RegisterDefaults();
        m_ServerScene = CreateScope<Scene>();
        m_ClientScene = CreateScope<Scene>();
    }

    Entity MakeServerEntity(u64 uuid, const glm::vec3& position, u32 owner, ENetworkAuthority authority)
    {
        Entity entity = m_ServerScene->CreateEntityWithUUID(UUID(uuid), "ServerPawn");
        entity.GetComponent<TransformComponent>().Translation = position;
        auto& nic = entity.AddComponent<NetworkIdentityComponent>();
        nic.OwnerClientID = owner;
        nic.Authority = authority;
        nic.IsReplicated = true;
        return entity;
    }

    Scope<Scene> m_ServerScene;
    Scope<Scene> m_ClientScene;
};

TEST_F(EntityLifecycleTest, SpawnCarriesIdentityAndInitialStateAcrossTheWire)
{
    Entity server = MakeServerEntity(4242ull, { 3.0f, -1.5f, 7.25f }, /*owner*/ 2, ENetworkAuthority::Client);

    NetworkSpawnParams params = EntityLifecycle::DescribeEntity(server, NetworkSpawnRegistry::kNetworkPlayerArchetype);
    const auto payload = EntityLifecycle::EncodeSpawn(params, EntitySnapshot::CaptureEntity(server));

    NetworkSpawnParams decoded;
    SnapshotEntity comps;
    ASSERT_TRUE(EntityLifecycle::DecodeSpawn(payload.data(), static_cast<u32>(payload.size()), decoded, comps));

    EXPECT_EQ(decoded.EntityUUID, 4242ull);
    EXPECT_EQ(decoded.OwnerClientID, 2u);
    EXPECT_EQ(decoded.Authority, ENetworkAuthority::Client);
    EXPECT_EQ(decoded.Archetype, NetworkSpawnRegistry::kNetworkPlayerArchetype);
    EXPECT_FALSE(comps.empty());

    bool created = false;
    Entity client = EntityLifecycle::ApplySpawn(*m_ClientScene, decoded, comps, created);
    ASSERT_TRUE(client);
    EXPECT_TRUE(created);

    // The entity exists under the SERVER's uuid — that identity is the mapping the
    // snapshot, RPC-targeting and interest paths all key off.
    EXPECT_EQ(static_cast<u64>(client.GetUUID()), 4242ull);

    ASSERT_TRUE(client.HasComponent<NetworkIdentityComponent>());
    EXPECT_EQ(client.GetComponent<NetworkIdentityComponent>().OwnerClientID, 2u);
    EXPECT_EQ(client.GetComponent<NetworkIdentityComponent>().Authority, ENetworkAuthority::Client);

    // And it arrived at the server's position, not at the origin.
    ASSERT_TRUE(client.HasComponent<TransformComponent>());
    const auto& translation = client.GetComponent<TransformComponent>().Translation;
    EXPECT_FLOAT_EQ(translation.x, 3.0f);
    EXPECT_FLOAT_EQ(translation.y, -1.5f);
    EXPECT_FLOAT_EQ(translation.z, 7.25f);
}

TEST_F(EntityLifecycleTest, ApplySpawnOnAnExistingEntityUpdatesItWithoutReportingCreation)
{
    // A scene-authored entity that both sides already have: the spawn must adopt it
    // rather than duplicate it, and must NOT report `created` — that flag is what
    // stops a later despawn from destroying level geometry.
    Entity preexisting = m_ClientScene->CreateEntityWithUUID(UUID(77ull), "LevelProp");
    preexisting.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };

    Entity server = MakeServerEntity(77ull, { 10.0f, 0.0f, 0.0f }, /*owner*/ 0, ENetworkAuthority::Server);
    NetworkSpawnParams params = EntityLifecycle::DescribeEntity(server, "");

    bool created = true;
    Entity applied = EntityLifecycle::ApplySpawn(*m_ClientScene, params, EntitySnapshot::CaptureEntity(server), created);

    ASSERT_TRUE(applied);
    EXPECT_FALSE(created);
    EXPECT_EQ(static_cast<u64>(applied.GetUUID()), 77ull);
    EXPECT_FLOAT_EQ(applied.GetComponent<TransformComponent>().Translation.x, 10.0f);
}

TEST_F(EntityLifecycleTest, SpawnEnsuresAReplicatedComponentTheClientEntityLacked)
{
    // EntitySnapshot::Apply deliberately skips a component the entity does not
    // already carry — that is how a client keeps its local-only components out of
    // the replicated set. Spawn is the one path that must ADD them, or a freshly
    // created entity would never receive any state at all.
    Entity server = MakeServerEntity(88ull, { 1.0f, 2.0f, 3.0f }, 0, ENetworkAuthority::Server);
    auto& rb = server.AddComponent<Rigidbody3DComponent>();
    rb.m_Mass = 12.5f;

    NetworkSpawnParams params = EntityLifecycle::DescribeEntity(server, "");
    bool created = false;
    Entity client = EntityLifecycle::ApplySpawn(*m_ClientScene, params, EntitySnapshot::CaptureEntity(server), created);

    ASSERT_TRUE(client);
    ASSERT_TRUE(client.HasComponent<Rigidbody3DComponent>())
        << "spawn did not materialise the replicated Rigidbody3DComponent";
    EXPECT_FLOAT_EQ(client.GetComponent<Rigidbody3DComponent>().m_Mass, 12.5f);
}

TEST_F(EntityLifecycleTest, DespawnCodecRoundTrips)
{
    const auto payload = EntityLifecycle::EncodeDespawn(0xDEAD'BEEF'0000'0001ull);
    u64 uuid = 0;
    ASSERT_TRUE(EntityLifecycle::DecodeDespawn(payload.data(), static_cast<u32>(payload.size()), uuid));
    EXPECT_EQ(uuid, 0xDEAD'BEEF'0000'0001ull);
}

TEST_F(EntityLifecycleTest, MalformedSpawnPayloadsAreRejected)
{
    Entity server = MakeServerEntity(99ull, { 0.0f, 0.0f, 0.0f }, 0, ENetworkAuthority::Server);
    NetworkSpawnParams params = EntityLifecycle::DescribeEntity(server, "");
    const auto payload = EntityLifecycle::EncodeSpawn(params, EntitySnapshot::CaptureEntity(server));

    NetworkSpawnParams decoded;
    SnapshotEntity comps;

    EXPECT_FALSE(EntityLifecycle::DecodeSpawn(nullptr, 0, decoded, comps));
    EXPECT_FALSE(EntityLifecycle::DecodeSpawn(payload.data(), 0, decoded, comps));
    // Half a payload: the entity record is incomplete, so the whole spawn is refused
    // rather than producing an entity with arbitrary state.
    EXPECT_FALSE(EntityLifecycle::DecodeSpawn(payload.data(), static_cast<u32>(payload.size() / 2), decoded, comps));
}

TEST_F(EntityLifecycleTest, OutOfRangeAuthorityIsRefused)
{
    // A forged authority byte would be cast into an out-of-range ENetworkAuthority
    // and then compared against ::Server / ::Client throughout the authority checks.
    std::vector<u8> payload;
    {
        FMemoryWriter writer(payload);
        writer.ArIsNetArchive = true;
        u64 uuid = 5ull;
        i32 nameLength = 0;
        i32 archetypeLength = 0;
        u32 owner = 1;
        u8 authority = 200; // not a legal ENetworkAuthority
        writer << uuid << nameLength << archetypeLength << owner << authority;
    }
    EntitySnapshot::AppendEntityRecord(payload, 5ull, SnapshotEntity{ { 1u, { 0x00 } } });

    NetworkSpawnParams decoded;
    SnapshotEntity comps;
    EXPECT_FALSE(EntityLifecycle::DecodeSpawn(payload.data(), static_cast<u32>(payload.size()), decoded, comps));
}

TEST_F(EntityLifecycleTest, SpawnRecordEntityMustAgreeWithTheHeaderEntity)
{
    // The uuid appears in both the header and the entity record. A payload where
    // they disagree is forged or corrupt; picking either one would let a client be
    // told to build an entity under an id the sender never claimed.
    std::vector<u8> payload;
    {
        FMemoryWriter writer(payload);
        writer.ArIsNetArchive = true;
        u64 headerUUID = 11ull;
        i32 nameLength = 0;
        i32 archetypeLength = 0;
        u32 owner = 1;
        u8 authority = static_cast<u8>(ENetworkAuthority::Server);
        writer << headerUUID << nameLength << archetypeLength << owner << authority;
    }
    EntitySnapshot::AppendEntityRecord(payload, /*a different entity*/ 22ull, SnapshotEntity{ { 1u, { 0x00 } } });

    NetworkSpawnParams decoded;
    SnapshotEntity comps;
    EXPECT_FALSE(EntityLifecycle::DecodeSpawn(payload.data(), static_cast<u32>(payload.size()), decoded, comps));
}

TEST_F(EntityLifecycleTest, SpawnWithNoReplicatedComponentsStillCarriesItsIdentity)
{
    // The entity record is empty when a component set is empty, so the uuid has to
    // live in the header — otherwise such a spawn would be undecodable.
    NetworkSpawnParams params;
    params.EntityUUID = 5150ull;
    params.Name = "Marker";
    params.Archetype = "";
    params.OwnerClientID = 3;
    params.Authority = ENetworkAuthority::Server;

    const auto payload = EntityLifecycle::EncodeSpawn(params, SnapshotEntity{});

    NetworkSpawnParams decoded;
    SnapshotEntity comps;
    ASSERT_TRUE(EntityLifecycle::DecodeSpawn(payload.data(), static_cast<u32>(payload.size()), decoded, comps));
    EXPECT_EQ(decoded.EntityUUID, 5150ull);
    EXPECT_EQ(decoded.Name, "Marker");
    EXPECT_EQ(decoded.OwnerClientID, 3u);
    EXPECT_TRUE(comps.empty());
}

TEST_F(EntityLifecycleTest, PlayerColorsAreStableAndDistinctPerClient)
{
    // Both ends derive the colour from the client id, so a demo pawn looks the same
    // on every screen with nothing authored per scene.
    const glm::vec4 first = NetworkSpawnRegistry::PlayerColorForClient(1);
    const glm::vec4 second = NetworkSpawnRegistry::PlayerColorForClient(2);

    EXPECT_FLOAT_EQ(first.r, NetworkSpawnRegistry::PlayerColorForClient(1).r);
    EXPECT_FLOAT_EQ(first.g, NetworkSpawnRegistry::PlayerColorForClient(1).g);

    // Distinguishable, not merely unequal — a hair's difference in one channel
    // would satisfy `!=` while looking identical on screen.
    const f32 separation = std::abs(first.r - second.r) + std::abs(first.g - second.g) + std::abs(first.b - second.b);
    EXPECT_GT(separation, 0.25f) << "clients 1 and 2 must be visually distinguishable";
}

TEST_F(EntityLifecycleTest, NetworkPlayerArchetypeBuildsAVisibleOwnedPawn)
{
    NetworkSpawnParams params;
    params.EntityUUID = 3000ull;
    params.Name = "Player 4";
    params.Archetype = NetworkSpawnRegistry::kNetworkPlayerArchetype;
    params.OwnerClientID = 4;
    params.Authority = ENetworkAuthority::Client;

    bool created = false;
    Entity pawn = EntityLifecycle::ApplySpawn(*m_ClientScene, params, SnapshotEntity{}, created);

    ASSERT_TRUE(pawn);
    EXPECT_TRUE(created);
    EXPECT_TRUE(pawn.HasComponent<TransformComponent>());
    // Asset-free visuals are what let the demo scene be watchable with no imports.
    ASSERT_TRUE(pawn.HasComponent<SpriteRendererComponent>());
    EXPECT_TRUE(pawn.HasComponent<NetworkIdentityComponent>());
    EXPECT_EQ(pawn.GetComponent<NetworkIdentityComponent>().OwnerClientID, 4u);
}
