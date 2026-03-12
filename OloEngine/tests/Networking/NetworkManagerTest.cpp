#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Networking/Core/NetworkManager.h"

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // NetworkManagerTest
    // -------------------------------------------------------------------------

    TEST(NetworkManagerTest, InitShutdownLifecycle)
    {
        ASSERT_FALSE(NetworkManager::IsInitialized());

        bool initResult = NetworkManager::Init();
        EXPECT_TRUE(initResult);
        EXPECT_TRUE(NetworkManager::IsInitialized());

        NetworkManager::Shutdown();
        EXPECT_FALSE(NetworkManager::IsInitialized());
    }

    TEST(NetworkManagerTest, DoubleInitIsIdempotent)
    {
        ASSERT_FALSE(NetworkManager::IsInitialized());

        bool first  = NetworkManager::Init();
        bool second = NetworkManager::Init(); // Should return true without crashing
        EXPECT_TRUE(first);
        EXPECT_TRUE(second);
        EXPECT_TRUE(NetworkManager::IsInitialized());

        NetworkManager::Shutdown();
        EXPECT_FALSE(NetworkManager::IsInitialized());
    }

    TEST(NetworkManagerTest, ShutdownWithoutInitIsHarmless)
    {
        ASSERT_FALSE(NetworkManager::IsInitialized());
        // Should not crash
        NetworkManager::Shutdown();
        EXPECT_FALSE(NetworkManager::IsInitialized());
    }

    TEST(NetworkManagerTest, IsServerReturnsFalseWhenNotInitialized)
    {
        ASSERT_FALSE(NetworkManager::IsInitialized());
        EXPECT_FALSE(NetworkManager::IsServer());
    }

    TEST(NetworkManagerTest, IsClientReturnsFalseWhenNotInitialized)
    {
        ASSERT_FALSE(NetworkManager::IsInitialized());
        EXPECT_FALSE(NetworkManager::IsClient());
    }

    TEST(NetworkManagerTest, IsConnectedReturnsFalseWhenNotInitialized)
    {
        ASSERT_FALSE(NetworkManager::IsInitialized());
        EXPECT_FALSE(NetworkManager::IsConnected());
    }

} // namespace OloEngine::Tests
