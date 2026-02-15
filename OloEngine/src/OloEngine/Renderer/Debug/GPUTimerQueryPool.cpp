#include "OloEnginePCH.h"
#include "GPUTimerQueryPool.h"
#include "OloEngine/Core/Log.h"

#include <glad/gl.h>

namespace OloEngine
{
    GPUTimerQueryPool& GPUTimerQueryPool::GetInstance()
    {
        static GPUTimerQueryPool instance;
        return instance;
    }

    void GPUTimerQueryPool::Initialize(u32 maxQueries)
    {
        if (m_Initialized)
            return;

        m_MaxQueries = maxQueries;

        // Create double-buffered query objects
        for (u32 buf = 0; buf < 2; ++buf)
        {
            m_QueryObjects[buf].resize(maxQueries, 0);
            glCreateQueries(GL_TIME_ELAPSED, static_cast<GLsizei>(maxQueries), m_QueryObjects[buf].data());
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
        if (!m_Initialized)
            return;

        for (u32 buf = 0; buf < 2; ++buf)
        {
            if (!m_QueryObjects[buf].empty())
            {
                glDeleteQueries(static_cast<GLsizei>(m_QueryObjects[buf].size()), m_QueryObjects[buf].data());
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
            std::fill(m_Results.begin(), m_Results.end(), 0.0);

            for (u32 i = 0; i < m_ReadableQueryCount; ++i)
            {
                GLuint64 timeNs = 0;
                glGetQueryObjectui64v(m_QueryObjects[readBuffer][i], GL_QUERY_RESULT, &timeNs);
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

        glBeginQuery(GL_TIME_ELAPSED, m_QueryObjects[m_WriteBuffer][commandIndex]);

        if (commandIndex >= m_WriteQueryCount)
            m_WriteQueryCount = commandIndex + 1;
    }

    void GPUTimerQueryPool::EndQuery([[maybe_unused]] u32 commandIndex)
    {
        if (!m_Active)
            return;

        glEndQuery(GL_TIME_ELAPSED);
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
} // namespace OloEngine
