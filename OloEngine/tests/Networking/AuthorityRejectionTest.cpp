#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Prediction/ServerInputHandler.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

using namespace OloEngine;

// Helper: build a serialized InputCommand payload (tick + entityUUID + payload)
static std::vector<u8> MakeInputPayload(u32 tick, u64 entityUUID, const std::vector<u8>& payload = {})
{
    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;
    writer << tick;
    writer << entityUUID;
    if (!payload.empty())
    {
        writer.Serialize(const_cast<u8*>(payload.data()), static_cast<i64>(payload.size()));
    }
    return buffer;
}

class AuthorityRejectionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = CreateScope<Scene>();
        m_Handler = ServerInputHandler();
        m_AppliedCount = 0;
        m_LastAppliedEntityUUID = 0;

        m_Handler.SetInputApplyCallback(
            [this](Scene& /*s*/, u64 uuid, const u8* /*data*/, u32 /*size*/)
            {
                ++m_AppliedCount;
                m_LastAppliedEntityUUID = uuid;
            });
    }

    Scope<Scene> m_Scene;
    ServerInputHandler m_Handler;
    u32 m_AppliedCount = 0;
    u64 m_LastAppliedEntityUUID = 0;
};

TEST_F(AuthorityRejectionTest, AcceptsInputFromOwnerWithClientAuthority)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Owned");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.OwnerClientID = 1;
    nic.Authority = ENetworkAuthority::Client;

    auto payload = MakeInputPayload(1, 100, { 0xAA });
    m_Handler.ProcessInput(*m_Scene, 1, payload.data(), static_cast<u32>(payload.size()));

    EXPECT_EQ(m_AppliedCount, 1u);
    EXPECT_EQ(m_LastAppliedEntityUUID, 100u);
    EXPECT_EQ(m_Handler.GetLastProcessedTick(1), 1u);
}

TEST_F(AuthorityRejectionTest, RejectsInputFromNonOwner)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Owned");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.OwnerClientID = 1;
    nic.Authority = ENetworkAuthority::Client;

    // Client 2 tries to control entity owned by client 1
    auto payload = MakeInputPayload(1, 100, { 0xBB });
    m_Handler.ProcessInput(*m_Scene, 2, payload.data(), static_cast<u32>(payload.size()));

    EXPECT_EQ(m_AppliedCount, 0u);
    EXPECT_EQ(m_Handler.GetLastProcessedTick(2), 0u); // Not tracked since rejected
}

TEST_F(AuthorityRejectionTest, RejectsInputForServerAuthoritativeEntity)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "ServerControlled");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.OwnerClientID = 1;
    nic.Authority = ENetworkAuthority::Server;

    // Even the owner can't send inputs for server-authoritative entities
    auto payload = MakeInputPayload(1, 100, { 0xCC });
    m_Handler.ProcessInput(*m_Scene, 1, payload.data(), static_cast<u32>(payload.size()));

    EXPECT_EQ(m_AppliedCount, 0u);
}

TEST_F(AuthorityRejectionTest, AcceptsInputForSharedAuthorityEntity)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Shared");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.OwnerClientID = 1;
    nic.Authority = ENetworkAuthority::Shared;

    auto payload = MakeInputPayload(1, 100, { 0xDD });
    m_Handler.ProcessInput(*m_Scene, 1, payload.data(), static_cast<u32>(payload.size()));

    EXPECT_EQ(m_AppliedCount, 1u);
}

TEST_F(AuthorityRejectionTest, RejectsInputForUnknownEntity)
{
    // No entity with UUID 999 exists
    auto payload = MakeInputPayload(1, 999, { 0xEE });
    m_Handler.ProcessInput(*m_Scene, 1, payload.data(), static_cast<u32>(payload.size()));

    EXPECT_EQ(m_AppliedCount, 0u);
}

TEST_F(AuthorityRejectionTest, RejectsInputForEntityWithoutNetworkIdentity)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "NoNIC");
    // Entity exists but has no NetworkIdentityComponent

    auto payload = MakeInputPayload(1, 100, { 0xFF });
    m_Handler.ProcessInput(*m_Scene, 1, payload.data(), static_cast<u32>(payload.size()));

    EXPECT_EQ(m_AppliedCount, 0u);
}

TEST_F(AuthorityRejectionTest, TracksLastProcessedTickPerClient)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Owned");
    auto& nic = e.AddComponent<NetworkIdentityComponent>();
    nic.OwnerClientID = 1;
    nic.Authority = ENetworkAuthority::Client;

    auto p1 = MakeInputPayload(5, 100, { 0x01 });
    auto p2 = MakeInputPayload(10, 100, { 0x02 });
    m_Handler.ProcessInput(*m_Scene, 1, p1.data(), static_cast<u32>(p1.size()));
    m_Handler.ProcessInput(*m_Scene, 1, p2.data(), static_cast<u32>(p2.size()));

    EXPECT_EQ(m_Handler.GetLastProcessedTick(1), 10u);
    EXPECT_EQ(m_Handler.GetAllLastProcessedTicks().size(), 1u);
}

TEST_F(AuthorityRejectionTest, RejectsTruncatedPayload)
{
    // Payload too small to contain tick + entityUUID
    std::vector<u8> tiny = { 0x01, 0x02 };
    m_Handler.ProcessInput(*m_Scene, 1, tiny.data(), static_cast<u32>(tiny.size()));

    EXPECT_EQ(m_AppliedCount, 0u);
}
