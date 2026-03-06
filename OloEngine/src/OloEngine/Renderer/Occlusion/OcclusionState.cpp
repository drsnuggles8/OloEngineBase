#include "OloEnginePCH.h"
#include "OcclusionState.h"

namespace OloEngine
{
    OcclusionStateManager& OcclusionStateManager::GetInstance()
    {
        static OcclusionStateManager instance;
        return instance;
    }

    void OcclusionStateManager::Clear()
    {
        m_States.clear();
        m_FreeQueryIndices.clear();
        m_NextQueryIndex = 0;
        m_CurrentFrame = 0;
    }

    OcclusionState& OcclusionStateManager::GetOrCreate(u64 objectID)
    {
        return m_States[objectID];
    }

    bool OcclusionStateManager::Has(u64 objectID) const
    {
        return m_States.contains(objectID);
    }

    void OcclusionStateManager::Remove(u64 objectID)
    {
        auto it = m_States.find(objectID);
        if (it != m_States.end())
        {
            if (it->second.QueryIndex != UINT32_MAX)
            {
                FreeQueryIndex(it->second.QueryIndex);
            }
            m_States.erase(it);
        }
    }

    u32 OcclusionStateManager::AllocateQueryIndex()
    {
        if (!m_FreeQueryIndices.empty())
        {
            u32 index = m_FreeQueryIndices.back();
            m_FreeQueryIndices.pop_back();
            return index;
        }
        if (m_NextQueryIndex < m_MaxQueries)
        {
            return m_NextQueryIndex++;
        }
        return UINT32_MAX;
    }

    void OcclusionStateManager::FreeQueryIndex(u32 index)
    {
        m_FreeQueryIndices.push_back(index);
    }

    void OcclusionStateManager::SetMaxQueries(u32 max)
    {
        m_MaxQueries = max;
    }

    void OcclusionStateManager::BeginFrame()
    {
        m_CurrentFrame++;
    }
} // namespace OloEngine
