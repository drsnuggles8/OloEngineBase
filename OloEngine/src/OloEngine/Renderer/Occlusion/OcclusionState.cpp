#include "OloEnginePCH.h"
#include "OcclusionState.h"

namespace OloEngine
{
    namespace
    {
        // One manager per scene view (ADR 0026): the states are keyed by
        // object and hold what THAT view's queries last answered.
        struct ViewManagers
        {
            std::unordered_map<u64, OcclusionStateManager*> Extra;
            OcclusionStateManager* Active = nullptr; // null: the main view's
        };

        ViewManagers& Managers()
        {
            static ViewManagers managers;
            return managers;
        }
    } // namespace

    OcclusionStateManager& OcclusionStateManager::GetInstance()
    {
        static OcclusionStateManager mainView;
        OcclusionStateManager* active = Managers().Active;
        return active ? *active : mainView;
    }

    void OcclusionStateManager::SetActiveView(u64 view)
    {
        ViewManagers& managers = Managers();
        if (view == 0)
        {
            managers.Active = nullptr;
            return;
        }
        OcclusionStateManager*& manager = managers.Extra[view];
        if (!manager)
            manager = new OcclusionStateManager();
        managers.Active = manager;
    }

    void OcclusionStateManager::ReleaseView(u64 view)
    {
        ViewManagers& managers = Managers();
        const auto it = managers.Extra.find(view);
        if (it == managers.Extra.end())
            return;
        if (managers.Active == it->second)
            managers.Active = nullptr;
        delete it->second;
        managers.Extra.erase(it);
    }

    void OcclusionStateManager::Clear()
    {
        OLO_PROFILE_FUNCTION();
        m_States.clear();
        m_FreeQueryIndices.Reset();
        m_NextQueryIndex = 0;
        m_CurrentFrame = 0;
    }

    OcclusionState& OcclusionStateManager::GetOrCreate(u64 objectID)
    {
        OLO_PROFILE_FUNCTION();
        return m_States[objectID];
    }

    bool OcclusionStateManager::Has(u64 objectID) const
    {
        return m_States.contains(objectID);
    }

    void OcclusionStateManager::Remove(u64 objectID)
    {
        OLO_PROFILE_FUNCTION();
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
        OLO_PROFILE_FUNCTION();
        if (!m_FreeQueryIndices.IsEmpty())
        {
            u32 index = m_FreeQueryIndices.Last();
            m_FreeQueryIndices.Pop(EAllowShrinking::No);
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
        OLO_PROFILE_FUNCTION();
        if (index >= m_MaxQueries)
            return;

        // Prevent duplicate free
        if (std::ranges::find(m_FreeQueryIndices, index) != m_FreeQueryIndices.end())
            return;

        m_FreeQueryIndices.Add(index);
    }

    void OcclusionStateManager::SetMaxQueries(u32 max)
    {
        m_MaxQueries = max;
    }

    void OcclusionStateManager::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();
        ++m_CurrentFrame;
    }
} // namespace OloEngine
