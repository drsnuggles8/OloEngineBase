#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Log.h"

TEST(LoggerTest, InitTest)
{
    ASSERT_EQ(OloEngine::Log::GetCoreLogger(), nullptr);
    ASSERT_EQ(OloEngine::Log::GetClientLogger(), nullptr);

    OloEngine::Log::Init();

    EXPECT_NE(OloEngine::Log::GetCoreLogger(), nullptr);
    EXPECT_NE(OloEngine::Log::GetClientLogger(), nullptr);
}
