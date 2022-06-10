// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Log.h"

TEST(LogTest, InitTest)
{
	ASSERT_EQ(OloEngine::Log::GetCoreLogger(), nullptr);
	ASSERT_EQ(OloEngine::Log::GetClientLogger(), nullptr);

	OloEngine::Log::Init();

	EXPECT_NE(OloEngine::Log::GetCoreLogger(), nullptr);
	EXPECT_NE(OloEngine::Log::GetClientLogger(), nullptr);
}
