#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// Contract tests for the RPC layer's two hard parts: the wire codec (which reads
// entirely attacker-controlled bytes) and the authority routing (which is the only
// thing standing between a client and the server's simulation).
//
// These are deliberately transport-free — RpcDispatcher::ExecuteLocally is given a
// Scene and a decoded call directly, so a routing bug shows up as a failed
// assertion here rather than as "the multiplayer demo feels wrong".

#include "OloEngine/Networking/RPC/RpcDispatcher.h"
#include "OloEngine/Networking/RPC/RpcRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    RpcArgList MakeSampleArgs()
    {
        RpcArgList args;
        args.push_back(RpcArg::MakeBool(true));
        args.push_back(RpcArg::MakeInt(-42));
        args.push_back(RpcArg::MakeFloat(3.5));
        args.push_back(RpcArg::MakeString("hello wire"));
        args.push_back(RpcArg::MakeVec3({ 1.0f, -2.0f, 0.5f }));
        args.push_back(RpcArg::MakeEntity(0xFFFF'FFFF'0000'0001ull));
        return args;
    }
} // namespace

class RpcMarshallingTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        RpcRegistry::Clear();
        m_Scene = CreateScope<Scene>();
    }

    void TearDown() override
    {
        RpcRegistry::Clear();
    }

    // A client-owned entity, so ownership checks have something real to test.
    Entity MakeOwnedEntity(u64 uuid, u32 ownerClientID)
    {
        Entity entity = m_Scene->CreateEntityWithUUID(UUID(uuid), "Pawn");
        auto& nic = entity.AddComponent<NetworkIdentityComponent>();
        nic.OwnerClientID = ownerClientID;
        nic.Authority = ENetworkAuthority::Client;
        nic.IsReplicated = true;
        return entity;
    }

    Scope<Scene> m_Scene;
};

TEST_F(RpcMarshallingTest, EveryArgumentTypeSurvivesTheWire)
{
    const RpcArgList original = MakeSampleArgs();
    const auto payload = RpcDispatcher::Encode(1234u, 99ull, 7u, original);

    RpcDispatcher::DecodedRpc decoded;
    ASSERT_TRUE(RpcDispatcher::Decode(payload.data(), static_cast<u32>(payload.size()), decoded));

    EXPECT_EQ(decoded.Id, 1234u);
    EXPECT_EQ(decoded.EntityUUID, 99ull);
    EXPECT_EQ(decoded.TargetClientID, 7u);
    ASSERT_EQ(decoded.Args.size(), original.size());

    EXPECT_EQ(decoded.Args[0].Type, ERpcArgType::Bool);
    EXPECT_TRUE(decoded.Args[0].AsBool);

    EXPECT_EQ(decoded.Args[1].Type, ERpcArgType::Int);
    EXPECT_EQ(decoded.Args[1].AsInt, -42);

    EXPECT_EQ(decoded.Args[2].Type, ERpcArgType::Float);
    EXPECT_DOUBLE_EQ(decoded.Args[2].AsFloat, 3.5);

    EXPECT_EQ(decoded.Args[3].Type, ERpcArgType::String);
    EXPECT_EQ(decoded.Args[3].AsString, "hello wire");

    EXPECT_EQ(decoded.Args[4].Type, ERpcArgType::Vec3);
    EXPECT_FLOAT_EQ(decoded.Args[4].AsVec3.x, 1.0f);
    EXPECT_FLOAT_EQ(decoded.Args[4].AsVec3.y, -2.0f);
    EXPECT_FLOAT_EQ(decoded.Args[4].AsVec3.z, 0.5f);

    // The high-bit UUID is the interesting case: it is where a signed round-trip
    // would silently corrupt half the id space.
    EXPECT_EQ(decoded.Args[5].Type, ERpcArgType::Entity);
    EXPECT_EQ(decoded.Args[5].AsEntity, 0xFFFF'FFFF'0000'0001ull);
}

TEST_F(RpcMarshallingTest, TruncatedPayloadIsRejectedRatherThanPartiallyDecoded)
{
    const auto payload = RpcDispatcher::Encode(1u, 0ull, 0u, MakeSampleArgs());
    ASSERT_GT(payload.size(), 8u);

    // Every prefix of a valid payload must be refused: a partially-decoded call
    // would run a handler with arguments the sender never sent.
    for (sizet cut = 1; cut < payload.size(); ++cut)
    {
        RpcDispatcher::DecodedRpc decoded;
        const bool ok = RpcDispatcher::Decode(payload.data(), static_cast<u32>(cut), decoded);
        if (ok)
        {
            // A prefix may only succeed if it happens to contain the full header
            // plus every declared argument — impossible here, since the last arg
            // ends exactly at payload.size().
            FAIL() << "Decode accepted a truncated payload of " << cut << " / " << payload.size() << " bytes";
        }
    }
}

TEST_F(RpcMarshallingTest, HostileStringLengthIsRejectedNotAllocated)
{
    // A hand-built payload declaring a 1 GB string in a 20-byte buffer. The decoder
    // must reject it on the length check rather than resizing first and failing the
    // read afterwards.
    std::vector<u8> payload;
    FMemoryWriter writer(payload);
    writer.ArIsNetArchive = true;
    u32 id = 1;
    u64 entity = 0;
    u32 target = 0;
    u16 argCount = 1;
    u8 argType = static_cast<u8>(ERpcArgType::String);
    i32 hostileLength = 1024 * 1024 * 1024;
    writer << id << entity << target << argCount << argType << hostileLength;

    RpcDispatcher::DecodedRpc decoded;
    EXPECT_FALSE(RpcDispatcher::Decode(payload.data(), static_cast<u32>(payload.size()), decoded));
}

TEST_F(RpcMarshallingTest, UnknownRpcIdIsDropped)
{
    RpcDispatcher::DecodedRpc call;
    call.Id = RpcRegistry::HashName("NeverRegistered");

    EXPECT_FALSE(RpcDispatcher::ExecuteLocally(m_Scene.get(), call, /*sender*/ 1, /*receivedOnServer*/ true));
}

TEST_F(RpcMarshallingTest, ServerRefusesAClientPushingAMulticastRpc)
{
    // Authority routing: multicast originates on the server only. A client that
    // forges the payload must still be refused, because the check reads the
    // SERVER's copy of the descriptor, which the client cannot touch.
    bool ran = false;
    RpcDescriptor descriptor;
    descriptor.Name = "Broadcast";
    descriptor.Target = ERpcTarget::Multicast;
    descriptor.Handler = [&ran](const RpcContext&, const RpcArgList&)
    { ran = true; };
    RpcRegistry::Register(descriptor);

    RpcDispatcher::DecodedRpc call;
    call.Id = RpcRegistry::HashName("Broadcast");

    EXPECT_FALSE(RpcDispatcher::ExecuteLocally(m_Scene.get(), call, /*sender*/ 3, /*receivedOnServer*/ true));
    EXPECT_FALSE(ran);
}

TEST_F(RpcMarshallingTest, ClientRefusesAServerTargetRpcPushedByTheServer)
{
    bool ran = false;
    RpcDescriptor descriptor;
    descriptor.Name = "ServerOnly";
    descriptor.Target = ERpcTarget::Server;
    descriptor.Handler = [&ran](const RpcContext&, const RpcArgList&)
    { ran = true; };
    RpcRegistry::Register(descriptor);

    RpcDispatcher::DecodedRpc call;
    call.Id = RpcRegistry::HashName("ServerOnly");

    EXPECT_FALSE(RpcDispatcher::ExecuteLocally(m_Scene.get(), call, /*sender*/ 0, /*receivedOnServer*/ false));
    EXPECT_FALSE(ran);
}

TEST_F(RpcMarshallingTest, EntityBoundServerRpcRefusesANonOwner)
{
    MakeOwnedEntity(500ull, /*ownerClientID*/ 1);

    bool ran = false;
    RpcDescriptor descriptor;
    descriptor.Name = "Fire";
    descriptor.Target = ERpcTarget::Server;
    descriptor.RequiresOwnership = true;
    descriptor.Handler = [&ran](const RpcContext&, const RpcArgList&)
    { ran = true; };
    RpcRegistry::Register(descriptor);

    RpcDispatcher::DecodedRpc call;
    call.Id = RpcRegistry::HashName("Fire");
    call.EntityUUID = 500ull;

    // Client 2 does not own entity 500.
    EXPECT_FALSE(RpcDispatcher::ExecuteLocally(m_Scene.get(), call, /*sender*/ 2, /*receivedOnServer*/ true));
    EXPECT_FALSE(ran);

    // The owner is allowed through, and sees the right context.
    u32 observedSender = 0;
    u64 observedEntity = 0;
    descriptor.Handler = [&](const RpcContext& context, const RpcArgList&)
    {
        ran = true;
        observedSender = context.SenderClientID;
        observedEntity = context.EntityUUID;
    };
    RpcRegistry::Register(descriptor);

    EXPECT_TRUE(RpcDispatcher::ExecuteLocally(m_Scene.get(), call, /*sender*/ 1, /*receivedOnServer*/ true));
    EXPECT_TRUE(ran);
    EXPECT_EQ(observedSender, 1u);
    EXPECT_EQ(observedEntity, 500ull);
}

TEST_F(RpcMarshallingTest, OwnershipCheckIsSkippedWhenTheDescriptorOptsOut)
{
    MakeOwnedEntity(600ull, /*ownerClientID*/ 1);

    bool ran = false;
    RpcDescriptor descriptor;
    descriptor.Name = "Interact";
    descriptor.Target = ERpcTarget::Server;
    descriptor.RequiresOwnership = false; // e.g. "open that door"
    descriptor.Handler = [&ran](const RpcContext&, const RpcArgList&)
    { ran = true; };
    RpcRegistry::Register(descriptor);

    RpcDispatcher::DecodedRpc call;
    call.Id = RpcRegistry::HashName("Interact");
    call.EntityUUID = 600ull;

    EXPECT_TRUE(RpcDispatcher::ExecuteLocally(m_Scene.get(), call, /*sender*/ 2, /*receivedOnServer*/ true));
    EXPECT_TRUE(ran);
}

TEST_F(RpcMarshallingTest, ReRegisteringANameReplacesItsHandlerRatherThanDuplicating)
{
    // A script reload must be able to rebind its RPCs without leaking the previous
    // closure or leaving two handlers racing for the same name.
    int firstRuns = 0;
    int secondRuns = 0;

    RpcDescriptor descriptor;
    descriptor.Name = "Reloadable";
    descriptor.Target = ERpcTarget::Server;
    descriptor.RequiresOwnership = false;
    descriptor.Handler = [&firstRuns](const RpcContext&, const RpcArgList&)
    { ++firstRuns; };
    RpcRegistry::Register(descriptor);
    ASSERT_EQ(RpcRegistry::Size(), 1u);

    descriptor.Handler = [&secondRuns](const RpcContext&, const RpcArgList&)
    { ++secondRuns; };
    RpcRegistry::Register(descriptor);
    EXPECT_EQ(RpcRegistry::Size(), 1u);

    RpcDispatcher::DecodedRpc call;
    call.Id = RpcRegistry::HashName("Reloadable");
    EXPECT_TRUE(RpcDispatcher::ExecuteLocally(m_Scene.get(), call, 1, true));

    EXPECT_EQ(firstRuns, 0);
    EXPECT_EQ(secondRuns, 1);
}

TEST_F(RpcMarshallingTest, ClearScriptOwnedDropsOnlyScriptHandlers)
{
    // The VM-teardown safety net: a handler capturing a sol::protected_function must
    // not survive its sol::state, while native registrations must.
    RpcDescriptor native;
    native.Name = "NativeRpc";
    native.ScriptOwned = false;
    RpcRegistry::Register(native);

    RpcDescriptor scripted;
    scripted.Name = "ScriptRpc";
    scripted.ScriptOwned = true;
    RpcRegistry::Register(scripted);

    ASSERT_EQ(RpcRegistry::Size(), 2u);

    RpcRegistry::ClearScriptOwned();

    EXPECT_EQ(RpcRegistry::Size(), 1u);
    EXPECT_TRUE(RpcRegistry::FindByName("NativeRpc").has_value());
    EXPECT_FALSE(RpcRegistry::FindByName("ScriptRpc").has_value());
}
