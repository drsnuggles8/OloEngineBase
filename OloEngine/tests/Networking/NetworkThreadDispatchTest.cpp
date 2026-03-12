#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/ExtendedTaskPriority.h"

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // NetworkThreadDispatchTest
    // -------------------------------------------------------------------------

    TEST(NetworkThreadDispatchTest, NetworkThreadEnumValueIsThree)
    {
        EXPECT_EQ(static_cast<i32>(Tasks::ENamedThread::NetworkThread), 3);
    }

    TEST(NetworkThreadDispatchTest, NetworkThreadCountIsNow4)
    {
        // Count must be > NetworkThread (3), so Count >= 4
        EXPECT_GT(static_cast<i32>(Tasks::ENamedThread::Count),
                  static_cast<i32>(Tasks::ENamedThread::NetworkThread));
    }

    TEST(NetworkThreadDispatchTest, ExtendedPriorityNetworkThreadNormalPriExists)
    {
        const char* str = Tasks::ToString(Tasks::EExtendedTaskPriority::NetworkThreadNormalPri);
        ASSERT_NE(str, nullptr);
        EXPECT_STREQ(str, "NetworkThreadNormalPri");
    }

    TEST(NetworkThreadDispatchTest, ExtendedPriorityNetworkThreadHiPriExists)
    {
        const char* str = Tasks::ToString(Tasks::EExtendedTaskPriority::NetworkThreadHiPri);
        ASSERT_NE(str, nullptr);
        EXPECT_STREQ(str, "NetworkThreadHiPri");
    }

    TEST(NetworkThreadDispatchTest, GetNamedThreadForNetworkPriority)
    {
        EXPECT_EQ(
            Tasks::GetNamedThread(Tasks::EExtendedTaskPriority::NetworkThreadNormalPri),
            Tasks::ENamedThread::NetworkThread);
        EXPECT_EQ(
            Tasks::GetNamedThread(Tasks::EExtendedTaskPriority::NetworkThreadHiPri),
            Tasks::ENamedThread::NetworkThread);
    }

    TEST(NetworkThreadDispatchTest, IsHighPriorityForNetworkHiPri)
    {
        EXPECT_TRUE(Tasks::IsHighPriority(Tasks::EExtendedTaskPriority::NetworkThreadHiPri));
        EXPECT_TRUE(Tasks::IsHighPriority(Tasks::EExtendedTaskPriority::NetworkThreadHiPriLocalQueue));
        EXPECT_FALSE(Tasks::IsHighPriority(Tasks::EExtendedTaskPriority::NetworkThreadNormalPri));
    }

    TEST(NetworkThreadDispatchTest, IsLocalQueueForNetworkLocalPriority)
    {
        EXPECT_TRUE(Tasks::IsLocalQueue(Tasks::EExtendedTaskPriority::NetworkThreadNormalPriLocalQueue));
        EXPECT_TRUE(Tasks::IsLocalQueue(Tasks::EExtendedTaskPriority::NetworkThreadHiPriLocalQueue));
        EXPECT_FALSE(Tasks::IsLocalQueue(Tasks::EExtendedTaskPriority::NetworkThreadNormalPri));
    }

} // namespace OloEngine::Tests
