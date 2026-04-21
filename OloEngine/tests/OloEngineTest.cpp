#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Log.h"
#include "Rendering/PropertyTests/TestFailureCapture.h"

int main(int argc, char** argv)
{
    // Initialize logging explicitly
    OloEngine::Log::Initialize();
    ::testing::InitGoogleTest(&argc, argv);
    OloEngine::Tests::TestFailureCapture::RegisterFailureListener();
    return ::RUN_ALL_TESTS();
}
