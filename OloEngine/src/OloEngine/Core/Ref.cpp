#include "OloEnginePCH.h"
#include "Ref.h"

#include <unordered_map>
#include <atomic>
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

namespace OloEngine
{

    struct LiveReferencesData
    {
        // Live object -> the generation it was given when it became live.
        std::unordered_map<const RefCounted*, u64> references;
        u64 nextGeneration = 1; // 0 means "no object"
        FMutex mutex;
        std::atomic<bool> isValid{ true };

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

    u32 RefCounted::DecRefCount() const
    {
        // fetch_sub returns the value *before* the decrement; std::memory_order_acq_rel
        // ensures that if this call turns out to be the one that brings the count to
        // zero, no side effects from the object's eventual destructor can be reordered
        // before this decrement is visible to other threads (same idiom as
        // Templates/RefCounting.h's TTransactionalAtomicRefCount::ImmediatelyRelease).
        const u32 previous = m_RefCount.fetch_sub(1, std::memory_order_acq_rel);
        OLO_CORE_ASSERT(previous > 0, "RefCounted: DecRefCount() underflow (refcount already zero)");
        return previous - 1;
    }

    u32 RefCounted::GetRefCount() const
    {
        return m_RefCount.load(std::memory_order_relaxed);
    }

    namespace RefUtils
    {
        void AddToLiveReferences(const RefCounted* instance)
        {
            auto& data = GetLiveReferencesData();

            // Check if the tracking system is still valid
            if (!data.isValid.load(std::memory_order_acquire))
            {
                // System is shutting down, don't attempt to access the hash table
                return;
            }

            TUniqueLock<FMutex> lock(data.mutex);
            OLO_CORE_ASSERT(instance);

            // Double-check after acquiring the lock
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return;
            }

            if (data.references.try_emplace(instance, data.nextGeneration).second)
            {
                ++data.nextGeneration;
            }
        }

        u64 LiveGeneration(const RefCounted* instance)
        {
            auto& data = GetLiveReferencesData();
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return 0;
            }

            TUniqueLock<FMutex> lock(data.mutex);
            OLO_CORE_ASSERT(instance);
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return 0;
            }

            const auto found = data.references.find(instance);
            return found != data.references.end() ? found->second : 0;
        }

        bool IsLive(const RefCounted* instance, u64 generation)
        {
            auto& data = GetLiveReferencesData();

            // Check if the tracking system is still valid
            if (!data.isValid.load(std::memory_order_acquire))
            {
                // System is shutting down, assume all references are dead
                return false;
            }

            TUniqueLock<FMutex> lock(data.mutex);
            OLO_CORE_ASSERT(instance);

            // Double-check after acquiring the lock
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return false;
            }

            const auto found = data.references.find(instance);
            return found != data.references.end() && found->second == generation;
        }

        bool Release(RefCounted* instance)
        {
            auto& data = GetLiveReferencesData();
            OLO_CORE_ASSERT(instance);

            // Shutting down: the registry is torn down (or tearing down), so there is
            // nobody left to race a WeakRef::Lock() against — decrement and authorize
            // the delete without touching the (possibly torn-down) registry.
            if (!data.isValid.load(std::memory_order_acquire))
            {
                return instance->DecRefCount() == 0;
            }

            // The decrement itself must happen *inside* this critical section, not
            // before it. An earlier version of this function decremented lock-free
            // first and only re-validated under the lock when the count reached zero
            // -- but that leaves a window between the decrement and acquiring this
            // lock during which a concurrent TryLockLive() can fully resurrect
            // `instance`, release it again itself, and delete it, all before this
            // thread ever gets here -- leaving `instance` dangling by the time this
            // finally runs (a real, reproducible double-free/use-after-free, not just
            // a theoretical race). Doing the decrement under the same lock TryLockLive()
            // uses closes that window entirely: the two can never interleave.
            TUniqueLock<FMutex> lock(data.mutex);

            if (!data.isValid.load(std::memory_order_acquire))
            {
                return instance->DecRefCount() == 0;
            }

            if (instance->DecRefCount() != 0)
            {
                return false;
            }

            data.references.erase(instance);
            return true;
        }

        bool TryLockLive(RefCounted* instance, u64 generation)
        {
            auto& data = GetLiveReferencesData();
            OLO_CORE_ASSERT(instance);

            if (!data.isValid.load(std::memory_order_acquire))
            {
                return false;
            }

            TUniqueLock<FMutex> lock(data.mutex);

            if (!data.isValid.load(std::memory_order_acquire))
            {
                return false;
            }

            // Membership in the live-reference set is the authoritative signal: it is
            // only removed by Release() while holding this same lock, right before
            // that thread commits to deleting the instance. So if we observe
            // `instance` still present here, Release() cannot yet have (and, because
            // we hold the lock, cannot concurrently) removed it — incrementing the
            // refcount now is safe and visible to Release()'s own check.
            // The generation is compared before `instance` is touched: a stale
            // WeakRef whose address now holds another object, or no object, must
            // not write to it.
            const auto found = data.references.find(instance);
            if (found == data.references.end() || found->second != generation)
            {
                return false;
            }

            instance->IncRefCount();
            return true;
        }
    } // namespace RefUtils

} // namespace OloEngine
