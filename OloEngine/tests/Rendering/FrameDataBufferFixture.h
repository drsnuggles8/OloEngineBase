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
            // A renderer brought up by an earlier test in this process (e.g.
            // AssetSceneLoad or any RendererAttachedTest) already owns the
            // manager and keeps it alive process-wide. Only manage the
            // lifecycle when it isn't already up — otherwise reuse it and don't
            // shut down what we don't own. Keeps the fixture order-independent.
            m_OwnsFrameData = !FrameDataBufferManager::IsInitialized();
            if (m_OwnsFrameData)
                FrameDataBufferManager::Init();
            // Reset for a clean frame
            FrameDataBufferManager::Get().Reset();
        }

        void TearDown() override
        {
            if (m_OwnsFrameData)
                FrameDataBufferManager::Shutdown();
        }

      private:
        bool m_OwnsFrameData = false;
    };

} // namespace OloEngine
