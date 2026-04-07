#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Log.h"

int main(int argc, char** argv)
{
    // Initialize logging explicitly
    OloEngine::Log::Initialize();
    ::testing::InitGoogleTest(&argc, argv);
    return ::RUN_ALL_TESTS();
}
