#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Scene/Components.h"

TEST(NetworkIdentityComponentTest, DefaultValues)
{
    using namespace OloEngine;

    NetworkIdentityComponent nic;
    EXPECT_EQ(nic.OwnerClientID, 0u);
    EXPECT_EQ(nic.Authority, ENetworkAuthority::Server);
    EXPECT_TRUE(nic.IsReplicated);
}

TEST(NetworkIdentityComponentTest, CopySemantics)
{
    using namespace OloEngine;

    NetworkIdentityComponent original;
    original.OwnerClientID = 42;
    original.Authority = ENetworkAuthority::Client;
    original.IsReplicated = false;

    NetworkIdentityComponent copy(original);
    EXPECT_EQ(copy.OwnerClientID, 42u);
    EXPECT_EQ(copy.Authority, ENetworkAuthority::Client);
    EXPECT_FALSE(copy.IsReplicated);
}
