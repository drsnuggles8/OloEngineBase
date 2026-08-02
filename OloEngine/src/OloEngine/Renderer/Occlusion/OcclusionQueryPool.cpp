#include "OloEnginePCH.h"
#include "OcclusionQueryPool.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    OcclusionQueryPool& OcclusionQueryPool::GetInstance()
    {
        static OcclusionQueryPool instance;
        return instance;
    }

    OcclusionQueryPool::~OcclusionQueryPool()
    {
        OLO_CORE_ASSERT(!m_Initialized, "OcclusionQueryPool::~OcclusionQueryPool: Shutdown() was not called before destruction!");
    }

    void OcclusionQueryPool::Initialize(u32 maxQueries)
    {
        OLO_PROFILE_FUNCTION();
        if (m_Initialized)
            return;

        m_MaxQueries = maxQueries;

        // Create double-buffered query objects
        for (u32 buf = 0; buf < 2; ++buf)
        {
            m_QueryObjects[buf].resize(maxQueries, RHI::NullResource);
            RenderCommand::CreateQueries(RHI::QueryType::OcclusionAnySamples, m_QueryObjects[buf]);
        }

        m_Results.resize(maxQueries, true); // Default visible until proven otherwise
        for (u32 buf = 0; buf < 2; ++buf)
            m_QueryIssued[buf].resize(maxQueries, false);
        m_WriteBuffer = 0;
        m_WriteQueryCount = 0;
        m_ReadableQueryCount = 0;
        m_FirstFrame = true;
        m_Initialized = true;

        OLO_CORE_INFO("OcclusionQueryPool: Initialized with {} queries (double-buffered)", maxQueries);
    }

    void OcclusionQueryPool::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized)
            return;

        for (u32 buf = 0; buf < 2; ++buf)
        {
            if (!m_QueryObjects[buf].empty())
            {
                RenderCommand::DeleteQueries(m_QueryObjects[buf]);
                m_QueryObjects[buf].clear();
            }
        }

        m_Results.clear();
        for (u32 buf = 0; buf < 2; ++buf)
            m_QueryIssued[buf].clear();
        m_Initialized = false;
        m_Active = false;

        OLO_CORE_INFO("OcclusionQueryPool: Shutdown");
    }

    bool OcclusionQueryPool::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized)
            return false;

        // Swap buffers: the write buffer from last frame becomes the read buffer
        u32 readBuffer = m_WriteBuffer;
        m_WriteBuffer = 1 - m_WriteBuffer;

        // Read back results from the previous frame's queries (now in readBuffer)
        bool hasResults = false;
        if (!m_FirstFrame)
        {
            m_ReadableQueryCount = m_WriteQueryCount; // previous frame's count

            for (u32 i = 0; i < m_ReadableQueryCount; ++i)
            {
                // Only read back indices that actually had queries issued
                if (!m_QueryIssued[readBuffer][i])
                {
                    m_Results[i] = true; // Default visible for non-issued queries
                    continue;
                }

                const bool available = RenderCommand::IsQueryResultAvailable(m_QueryObjects[readBuffer][i]);
                if (!available)
                {
                    // If result not yet available, assume visible to avoid popping
                    m_Results[i] = true;
                    continue;
                }

                const u32 result = RenderCommand::GetQueryResultU32(m_QueryObjects[readBuffer][i]);
                m_Results[i] = (result != 0);
            }
            hasResults = (m_ReadableQueryCount > 0);
        }

        m_WriteQueryCount = 0;
        // Clear issued flags for the new write buffer
        std::ranges::fill(m_QueryIssued[m_WriteBuffer], false);
        m_Active = true;
        m_FirstFrame = false;

        return hasResults;
    }

    void OcclusionQueryPool::BeginQuery(u32 objectIndex)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Active || objectIndex >= m_MaxQueries)
            return;

        RenderCommand::BeginQuery(RHI::QueryType::OcclusionAnySamples,
                                  m_QueryObjects[m_WriteBuffer][objectIndex]);
        m_QueryIssued[m_WriteBuffer][objectIndex] = true;

        if (objectIndex >= m_WriteQueryCount)
            m_WriteQueryCount = objectIndex + 1;
    }

    void OcclusionQueryPool::EndQuery([[maybe_unused]] u32 objectIndex) const
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Active)
            return;

        RenderCommand::EndQuery(RHI::QueryType::OcclusionAnySamples);
    }

    void OcclusionQueryPool::EndFrame()
    {
        OLO_PROFILE_FUNCTION();
        m_Active = false;
    }

    bool OcclusionQueryPool::WasVisible(u32 objectIndex) const
    {
        OLO_PROFILE_FUNCTION();
        if (objectIndex < m_ReadableQueryCount)
            return m_Results[objectIndex];
        return true; // Default visible if no query was issued
    }

    RHI::ResourceHandle OcclusionQueryPool::GetQueryHandle(u32 objectIndex) const
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized || objectIndex >= m_MaxQueries)
            return RHI::NullResource;
        // Return the read buffer's query identity (previous frame)
        u32 readBuffer = 1 - m_WriteBuffer;
        return m_QueryObjects[readBuffer][objectIndex];
    }
} // namespace OloEngine
