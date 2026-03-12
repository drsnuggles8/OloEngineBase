#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Scene/Components.h"

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // NetworkIdentityComponentTest
    // -------------------------------------------------------------------------

    TEST(NetworkIdentityComponentTest, DefaultValues)
    {
        NetworkIdentityComponent nic;
        EXPECT_EQ(nic.OwnerClientID, 0u);
        EXPECT_EQ(nic.Authority,     ENetworkAuthority::Server);
        EXPECT_TRUE(nic.IsReplicated);
    }

    TEST(NetworkIdentityComponentTest, CopySemantics)
    {
        NetworkIdentityComponent src;
        src.OwnerClientID = 42u;
        src.Authority     = ENetworkAuthority::Client;
        src.IsReplicated  = false;

        NetworkIdentityComponent dst = src;
        EXPECT_EQ(dst.OwnerClientID, 42u);
        EXPECT_EQ(dst.Authority,     ENetworkAuthority::Client);
        EXPECT_FALSE(dst.IsReplicated);
    }

    TEST(NetworkIdentityComponentTest, AllAuthorityValuesAreValid)
    {
        NetworkIdentityComponent nic;

        nic.Authority = ENetworkAuthority::Server;
        EXPECT_EQ(nic.Authority, ENetworkAuthority::Server);

        nic.Authority = ENetworkAuthority::Client;
        EXPECT_EQ(nic.Authority, ENetworkAuthority::Client);

        nic.Authority = ENetworkAuthority::Shared;
        EXPECT_EQ(nic.Authority, ENetworkAuthority::Shared);
    }

} // namespace OloEngine::Tests
