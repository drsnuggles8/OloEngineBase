#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Log.h"

int main(int argc, char** argv)
{
    // Log is auto-initialized via Meyer's singleton
    (void)OloEngine::Log::Get();
    ::testing::InitGoogleTest(&argc, argv);
    return ::RUN_ALL_TESTS();
}
