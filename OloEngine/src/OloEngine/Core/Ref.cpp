#include "OloEnginePCH.h"
#include "OloEngine/Core/Ref.h"

#include <unordered_set>
#include <mutex>

namespace OloEngine {

    static std::unordered_set<void*> s_LiveReferences;
    static std::mutex s_LiveReferenceMutex;

    // RefCounted implementation
    void RefCounted::IncRefCount() const
    {
        m_RefCount.fetch_add(1, std::memory_order_relaxed);
    }

    void RefCounted::DecRefCount() const
    {
        m_RefCount.fetch_sub(1, std::memory_order_acq_rel);
    }

    uint32_t RefCounted::GetRefCount() const
    {
        return m_RefCount.load(std::memory_order_relaxed);
    }

    namespace RefUtils {

        void AddToLiveReferences(void* instance)
        {
            std::scoped_lock<std::mutex> lock(s_LiveReferenceMutex);
            OLO_CORE_ASSERT(instance);
            s_LiveReferences.insert(instance);
        }

        void RemoveFromLiveReferences(void* instance)
        {
            std::scoped_lock<std::mutex> lock(s_LiveReferenceMutex);
            OLO_CORE_ASSERT(instance);
            OLO_CORE_ASSERT(s_LiveReferences.find(instance) != s_LiveReferences.end());
            s_LiveReferences.erase(instance);
        }

        bool IsLive(void* instance)
        {
            std::scoped_lock<std::mutex> lock(s_LiveReferenceMutex);
            OLO_CORE_ASSERT(instance);
            return s_LiveReferences.find(instance) != s_LiveReferences.end();
        }
    }

}