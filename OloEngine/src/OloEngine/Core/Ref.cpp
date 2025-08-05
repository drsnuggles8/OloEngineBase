#include "OloEnginePCH.h"
#include "Ref.h"

#include <unordered_set>
#include <mutex>
#include <atomic>

namespace OloEngine {

    struct LiveReferencesData
    {
        std::unordered_set<void*> references;
        std::mutex mutex;
        std::atomic<bool> isValid{true};
        
        ~LiveReferencesData()
        {
            isValid = false;
        }
    };

    static LiveReferencesData& GetLiveReferencesData()
    {
        static LiveReferencesData data;
        return data;
    }

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

    namespace RefUtils
	{
        void AddToLiveReferences(void* instance)
        {
            auto& data = GetLiveReferencesData();
            
            // Check if the tracking system is still valid
            if (!data.isValid.load(std::memory_order_acquire))
            {
                // System is shutting down, don't attempt to access the hash table
                return;
            }
            
            std::scoped_lock<std::mutex> lock(data.mutex);
            OLO_CORE_ASSERT(instance);
            
            // Double-check after acquiring the lock
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return;
            }
            
            data.references.insert(instance);
        }

        void RemoveFromLiveReferences(void* instance)
        {
            auto& data = GetLiveReferencesData();
            
            // Check if the tracking system is still valid before doing anything
            if (!data.isValid.load(std::memory_order_acquire))
            {
                // System is shutting down, don't attempt to access the hash table
                return;
            }
            
            std::scoped_lock<std::mutex> lock(data.mutex);
            OLO_CORE_ASSERT(instance);
            
            // Double-check after acquiring the lock
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return;
            }
            
            auto it = data.references.find(instance);
            if (it != data.references.end())
            {
                data.references.erase(it);
            }
        }

        bool IsLive(void* instance)
        {
            auto& data = GetLiveReferencesData();
            
            // Check if the tracking system is still valid
            if (!data.isValid.load(std::memory_order_acquire))
            {
                // System is shutting down, assume all references are dead
                return false;
            }
            
            std::scoped_lock<std::mutex> lock(data.mutex);
            OLO_CORE_ASSERT(instance);
            
            // Double-check after acquiring the lock
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return false;
            }
            
            return data.references.find(instance) != data.references.end();
        }
    }

}
