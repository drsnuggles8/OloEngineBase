#include "OloEnginePCH.h"
#include "GPUPassTimerPool.h"
#include "OloEngine/Core/Log.h"

#include <glad/gl.h>

namespace OloEngine
{
    GPUPassTimerPool& GPUPassTimerPool::GetInstance()
    {
        static GPUPassTimerPool instance;
        return instance;
    }

    GPUPassTimerPool::~GPUPassTimerPool()
    {
        OLO_CORE_ASSERT(!m_Initialized, "GPUPassTimerPool::~GPUPassTimerPool: Shutdown() was not called before destruction!");
    }

    void GPUPassTimerPool::Initialize(u32 maxPassesPerFrame)
    {
        OLO_PROFILE_FUNCTION();
        if (m_Initialized)
            return;

        m_MaxPasses = maxPassesPerFrame;

        const u32 queriesPerSlot = 2 + (2 * maxPassesPerFrame);
        for (auto& slot : m_Slots)
        {
            slot.Queries.resize(queriesPerSlot, 0);
            glCreateQueries(GL_TIMESTAMP, static_cast<GLsizei>(queriesPerSlot), slot.Queries.data());
            slot.PassNames.resize(maxPassesPerFrame);
            slot.PassCount = 0;
            slot.FrameNumber = 0;
            slot.Pending = false;
        }

        m_WriteSlot = 0;
        m_FrameCounter = 0;
        m_Active = false;
        m_PassOpen = false;
        m_SubPassOpen = false;
        m_LastFrameGpuMs = 0.0;
        m_LastResolvedFrame = 0;
        m_LastPassTimings.clear();
        m_Initialized = true;

        OLO_CORE_INFO("GPUPassTimerPool: Initialized with {} pass slots x {} frames in flight", maxPassesPerFrame, kSlotCount);
    }

    void GPUPassTimerPool::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized)
            return;

        for (auto& slot : m_Slots)
        {
            if (!slot.Queries.empty())
            {
                glDeleteQueries(static_cast<GLsizei>(slot.Queries.size()), slot.Queries.data());
                slot.Queries.clear();
            }
            slot.PassNames.clear();
            slot.Pending = false;
        }

        m_LastPassTimings.clear();
        m_Initialized = false;
        m_Active = false;
        m_PassOpen = false;
        m_SubPassOpen = false;

        OLO_CORE_INFO("GPUPassTimerPool: Shutdown");
    }

    void GPUPassTimerPool::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized)
            return;

        // Resolve pending slots oldest-first so the published results always end
        // on the newest fully-completed frame. Walking (m_WriteSlot + i) visits
        // slots in ascending write age.
        for (u32 i = 1; i <= kSlotCount; ++i)
        {
            FrameSlot& slot = m_Slots[(m_WriteSlot + i) % kSlotCount];
            if (slot.Pending)
                TryResolveSlot(slot, false);
        }

        m_WriteSlot = (m_WriteSlot + 1) % kSlotCount;
        FrameSlot& slot = m_Slots[m_WriteSlot];

        // About to overwrite: if the GPU is somehow >kSlotCount-1 frames behind,
        // drop the stale slot rather than stalling on it.
        if (slot.Pending)
            TryResolveSlot(slot, true);

        ++m_FrameCounter;
        slot.FrameNumber = m_FrameCounter;
        slot.PassCount = 0;
        slot.Pending = false;

        glQueryCounter(slot.Queries[0], GL_TIMESTAMP);
        m_Active = true;
        m_PassOpen = false;
        m_SubPassOpen = false;
    }

    void GPUPassTimerPool::EndFrame()
    {
        if (!m_Initialized || !m_Active)
            return;

        // Close any bracket left open (a Begin without its End). Every
        // allocated pair MUST have both timestamps stamped before the slot is
        // marked pending — resolving a never-stamped end query would publish
        // garbage.
        EndSubPass();
        EndPass();

        FrameSlot& slot = m_Slots[m_WriteSlot];
        glQueryCounter(slot.Queries[1], GL_TIMESTAMP);
        slot.Pending = true;
        m_Active = false;
    }

    void GPUPassTimerPool::BeginPass(const std::string& name)
    {
        if (!m_Active || m_PassOpen)
            return;

        FrameSlot& slot = m_Slots[m_WriteSlot];
        if (slot.PassCount >= m_MaxPasses)
            return;

        // Allocate the pair up front (rather than on EndPass) so a sub-pass
        // opened inside this bracket gets its own pair without colliding.
        m_CurrentPassIndex = slot.PassCount++;
        slot.PassNames[m_CurrentPassIndex] = name;
        glQueryCounter(slot.Queries[2 + (2 * m_CurrentPassIndex)], GL_TIMESTAMP);
        m_PassOpen = true;
    }

    void GPUPassTimerPool::EndPass()
    {
        if (!m_Active || !m_PassOpen)
            return;

        // A sub-pass left open must not outlive its parent bracket.
        EndSubPass();

        FrameSlot& slot = m_Slots[m_WriteSlot];
        glQueryCounter(slot.Queries[3 + (2 * m_CurrentPassIndex)], GL_TIMESTAMP);
        m_PassOpen = false;
    }

    void GPUPassTimerPool::BeginSubPass(const std::string& name)
    {
        if (!m_Active || !m_PassOpen || m_SubPassOpen)
            return;

        FrameSlot& slot = m_Slots[m_WriteSlot];
        if (slot.PassCount >= m_MaxPasses)
            return;

        m_CurrentSubPassIndex = slot.PassCount++;
        slot.PassNames[m_CurrentSubPassIndex] = slot.PassNames[m_CurrentPassIndex] + "/" + name;
        glQueryCounter(slot.Queries[2 + (2 * m_CurrentSubPassIndex)], GL_TIMESTAMP);
        m_SubPassOpen = true;
    }

    void GPUPassTimerPool::EndSubPass()
    {
        if (!m_Active || !m_SubPassOpen)
            return;

        FrameSlot& slot = m_Slots[m_WriteSlot];
        glQueryCounter(slot.Queries[3 + (2 * m_CurrentSubPassIndex)], GL_TIMESTAMP);
        m_SubPassOpen = false;
    }

    void GPUPassTimerPool::TryResolveSlot(FrameSlot& slot, bool dropIfUnavailable)
    {
        // The frame-end timestamp is the last query stamped in the slot; queries
        // complete in submission order, so its availability implies every earlier
        // timestamp in the slot is readable too.
        GLint available = GL_FALSE;
        glGetQueryObjectiv(slot.Queries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
        {
            if (dropIfUnavailable)
                slot.Pending = false;
            return;
        }

        GLuint64 frameBegin = 0;
        GLuint64 frameEnd = 0;
        glGetQueryObjectui64v(slot.Queries[0], GL_QUERY_RESULT, &frameBegin);
        glGetQueryObjectui64v(slot.Queries[1], GL_QUERY_RESULT, &frameEnd);

        m_LastFrameGpuMs = (frameEnd > frameBegin)
                               ? static_cast<f64>(frameEnd - frameBegin) / 1'000'000.0
                               : 0.0;

        m_LastPassTimings.clear();
        m_LastPassTimings.reserve(slot.PassCount);
        for (u32 i = 0; i < slot.PassCount; ++i)
        {
            GLuint64 passBegin = 0;
            GLuint64 passEnd = 0;
            glGetQueryObjectui64v(slot.Queries[2 + (2 * i)], GL_QUERY_RESULT, &passBegin);
            glGetQueryObjectui64v(slot.Queries[3 + (2 * i)], GL_QUERY_RESULT, &passEnd);

            m_LastPassTimings.push_back(PassTiming{
                .Name = slot.PassNames[i],
                .GpuMs = (passEnd > passBegin)
                             ? static_cast<f64>(passEnd - passBegin) / 1'000'000.0
                             : 0.0,
            });
        }

        m_LastResolvedFrame = slot.FrameNumber;
        slot.Pending = false;
    }
} // namespace OloEngine
