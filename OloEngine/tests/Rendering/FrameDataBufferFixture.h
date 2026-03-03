#pragma once

#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include <gtest/gtest.h>

namespace OloEngine
{
    /**
     * @brief Test fixture that initializes and shuts down FrameDataBufferManager.
     *
     * Any test that exercises code paths calling FrameDataBufferManager::Get()
     * (e.g. CommandBucket::BatchCommands, ConvertToInstanced) must derive from
     * this fixture or call Init()/Shutdown() manually in SetUp/TearDown.
     */
    class FrameDataBufferFixture : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            FrameDataBufferManager::Init();
            // Reset for a clean frame
            FrameDataBufferManager::Get().Reset();
        }

        void TearDown() override
        {
            FrameDataBufferManager::Shutdown();
        }
    };

} // namespace OloEngine
