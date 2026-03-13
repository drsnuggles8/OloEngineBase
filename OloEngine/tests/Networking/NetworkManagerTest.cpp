#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Core/NetworkManager.h"

TEST(NetworkManagerTest, InitShutdownLifecycle)
{
    EXPECT_FALSE(OloEngine::NetworkManager::IsInitialized());

    EXPECT_TRUE(OloEngine::NetworkManager::Init());
    EXPECT_TRUE(OloEngine::NetworkManager::IsInitialized());

    OloEngine::NetworkManager::Shutdown();
    EXPECT_FALSE(OloEngine::NetworkManager::IsInitialized());
}

TEST(NetworkManagerTest, DoubleInitIsIdempotent)
{
    EXPECT_TRUE(OloEngine::NetworkManager::Init());
    EXPECT_TRUE(OloEngine::NetworkManager::Init());
    EXPECT_TRUE(OloEngine::NetworkManager::IsInitialized());

    OloEngine::NetworkManager::Shutdown();
    EXPECT_FALSE(OloEngine::NetworkManager::IsInitialized());
}

TEST(NetworkManagerTest, ShutdownWithoutInitIsHarmless)
{
    EXPECT_FALSE(OloEngine::NetworkManager::IsInitialized());
    OloEngine::NetworkManager::Shutdown();
    EXPECT_FALSE(OloEngine::NetworkManager::IsInitialized());
}
