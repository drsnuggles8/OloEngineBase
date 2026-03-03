#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Log.h"

TEST(LoggerTest, InitTest)
{
    // Log::Init() is called in main(), so loggers are already initialized
    EXPECT_NE(OloEngine::Log::GetCoreLogger(), nullptr);
    EXPECT_NE(OloEngine::Log::GetClientLogger(), nullptr);
}
