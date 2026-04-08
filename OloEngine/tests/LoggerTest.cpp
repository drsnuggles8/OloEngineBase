#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Log.h"

TEST(LoggerTest, InitTest)
{
    // Log is initialized automatically via Meyer's singleton on first use
    EXPECT_NE(OloEngine::Log::Get().GetCoreLogger(), nullptr);
    EXPECT_NE(OloEngine::Log::Get().GetClientLogger(), nullptr);
}
