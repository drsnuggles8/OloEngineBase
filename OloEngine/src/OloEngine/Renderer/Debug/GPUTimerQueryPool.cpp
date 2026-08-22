#include "OloEnginePCH.h"
#include "GPUTimerQueryPool.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    GPUTimerQueryPool& GPUTimerQueryPool::GetInstance()
    {
        static GPUTimerQueryPool instance;
        return instance;
    }

    GPUTimerQueryPool::~GPUTimerQueryPool()
    {
        OLO_CORE_ASSERT(!m_Initialized, "GPUTimerQueryPool::~GPUTimerQueryPool: Shutdown() was not called before destruction!");
    }

    void GPUTimerQueryPool::Initialize(u32 maxQueries)
    {
        OLO_PROFILE_FUNCTION();
        if (m_Initialized)
            return;

        // Elapsed-time queries through the facade (#691): RHI::QueryType::
        // TimeElapsed lowers to GL_TIME_ELAPSED on GL and a timestamp pair on
        // Vulkan (VulkanQueryRegistry), so the pool runs on both backends. The
        // former GL-only early-out is gone with the direct glad calls it guarded.
        m_MaxQueries = maxQueries;

        // Create double-buffered query objects
        for (u32 buf = 0; buf < 2; ++buf)
        {
            m_QueryObjects[buf].assign(maxQueries, RHI::NullResource);
            RenderCommand::CreateQueries(RHI::QueryType::TimeElapsed, std::span<RHI::ResourceHandle>(m_QueryObjects[buf]));
        }

        m_Results.resize(maxQueries, 0.0);
        m_WriteBuffer = 0;
        m_WriteQueryCount = 0;
        m_ReadableQueryCount = 0;
        m_FirstFrame = true;
        m_Initialized = true;

        OLO_CORE_INFO("GPUTimerQueryPool: Initialized with {} queries (double-buffered)", maxQueries);
    }

    void GPUTimerQueryPool::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized)
            return;

        for (u32 buf = 0; buf < 2; ++buf)
        {
            if (!m_QueryObjects[buf].empty())
            {
                RenderCommand::DeleteQueries(std::span<const RHI::ResourceHandle>(m_QueryObjects[buf]));
                m_QueryObjects[buf].clear();
            }
        }

        m_Results.clear();
        m_Initialized = false;
        m_Active = false;

        OLO_CORE_INFO("GPUTimerQueryPool: Shutdown");
    }

    bool GPUTimerQueryPool::BeginFrame()
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
            std::ranges::fill(m_Results, 0.0);

            for (u32 i = 0; i < m_ReadableQueryCount; ++i)
            {
                if (!RenderCommand::IsQueryResultAvailable(m_QueryObjects[readBuffer][i]))
                    continue;

                const u64 timeNs = RenderCommand::GetQueryResultU64(m_QueryObjects[readBuffer][i]);
                m_Results[i] = static_cast<f64>(timeNs) / 1'000'000.0; // ns -> ms
            }
            hasResults = (m_ReadableQueryCount > 0);
        }

        m_WriteQueryCount = 0;
        m_Active = true;
        m_FirstFrame = false;

        return hasResults;
    }

    void GPUTimerQueryPool::BeginQuery(u32 commandIndex)
    {
        if (!m_Active || commandIndex >= m_MaxQueries)
            return;

        RenderCommand::BeginQuery(RHI::QueryType::TimeElapsed, m_QueryObjects[m_WriteBuffer][commandIndex]);

        if (commandIndex >= m_WriteQueryCount)
            m_WriteQueryCount = commandIndex + 1;
    }

    void GPUTimerQueryPool::EndQuery([[maybe_unused]] u32 commandIndex) const
    {
        if (!m_Active)
            return;

        RenderCommand::EndQuery(RHI::QueryType::TimeElapsed);
    }

    void GPUTimerQueryPool::EndFrame()
    {
        m_Active = false;
    }

    f64 GPUTimerQueryPool::GetQueryResultMs(u32 commandIndex) const
    {
        if (commandIndex < m_ReadableQueryCount)
            return m_Results[commandIndex];
        return 0.0;
    }

    bool GPUTimerQueryPool::TryGetIssuedQueryResultsMs(std::vector<f64>& outResultsMs) const
    {
        if (!m_Initialized || m_WriteQueryCount == 0)
            return false;

        const auto& queries = m_QueryObjects[m_WriteBuffer];

        // Queries complete in submission order, so the last issued query being
        // available implies every earlier one is readable too.
        if (!RenderCommand::IsQueryResultAvailable(queries[m_WriteQueryCount - 1]))
            return false;

        outResultsMs.resize(m_WriteQueryCount);
        for (u32 i = 0; i < m_WriteQueryCount; ++i)
        {
            const u64 timeNs = RenderCommand::GetQueryResultU64(queries[i]);
            outResultsMs[i] = static_cast<f64>(timeNs) / 1'000'000.0;
        }
        return true;
    }
} // namespace OloEngine
