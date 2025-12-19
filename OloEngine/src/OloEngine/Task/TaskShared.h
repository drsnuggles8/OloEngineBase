// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Templates/FunctionRef.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Debug/Instrumentor.h"

#if TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

namespace OloEngine::LowLevelTasks
{
    // @brief Thread-safe multicast delegate for oversubscription limit reached events
    //
    // This delegate is broadcasted when all worker threads are waiting and we've reached
    // the oversubscription limit. The receiver needs to be thread-safe.
    class FOversubscriptionLimitReached
    {
      public:
        using FCallback = TFunction<void()>;

        // @brief Add a callback to be invoked when oversubscription limit is reached
        // @param Callback The callback function
        void Add(FCallback Callback)
        {
            TUniqueLock<FMutex> Lock(m_Mutex);
            m_Callbacks.Add(MoveTemp(Callback));
        }

        // @brief Broadcast to all registered callbacks
        //
        // Thread-safe - can be called from any thread.
        void Broadcast()
        {
            TUniqueLock<FMutex> Lock(m_Mutex);
            for (const auto& Callback : m_Callbacks)
            {
                if (Callback)
                {
                    Callback();
                }
            }
        }

        // @brief Clear all registered callbacks
        void Clear()
        {
            TUniqueLock<FMutex> Lock(m_Mutex);
            m_Callbacks.Empty();
        }

      private:
        FMutex m_Mutex;
        TArray<FCallback> m_Callbacks;
    };

    // @brief Aligned array type for cache-line aligned allocations
    //
    // Used for worker events and local queues to avoid false sharing.
    // Note: Element types must be movable for reallocation to work.
    // For types with std::atomic members, provide explicit move constructor
    // that loads/stores the atomic values.
    template<typename NodeType>
    using TAlignedArray = TArray<NodeType, TAlignedHeapAllocator<alignof(NodeType)>>;

    namespace Private
    {
        // @class FOutOfWork
        // @brief Tracks when a worker is actively looking for work
        //
        // Used for profiling to understand when workers are idle vs active.
        // When Tracy profiling is enabled, emits events to visualize worker idle time.
        class FOutOfWork
        {
          private:
            bool m_ActivelyLookingForWork = false;
#if TRACY_ENABLE
            bool m_bTracyEventEmitted = false;
#endif

          public:
            OLO_FINLINE ~FOutOfWork()
            {
                Stop();
            }

            // @brief Mark the start of looking for work
            // @return true if this is a new start, false if already looking
            OLO_FINLINE bool Start()
            {
                if (!m_ActivelyLookingForWork)
                {
#if TRACY_ENABLE
                    // Emit Tracy event for worker looking for work
                    // This creates a visual marker in Tracy when workers are idle
                    static constexpr tracy::SourceLocationData OutOfWorkSourceLoc{
                        "TaskWorkerIsLookingForWork",
                        "FOutOfWork::Start",
                        __FILE__,
                        __LINE__,
                        0 // Color - 0 uses default
                    };
                    tracy::ScopedZone zone(&OutOfWorkSourceLoc, true);
                    m_bTracyEventEmitted = true;
#endif
                    m_ActivelyLookingForWork = true;
                    return true;
                }
                return false;
            }

            // @brief Mark the end of looking for work
            // @return true if we were looking and have now stopped, false otherwise
            OLO_FINLINE bool Stop()
            {
                if (m_ActivelyLookingForWork)
                {
#if TRACY_ENABLE
                    // Tracy zones are automatically ended when scope exits
                    // We just track whether we emitted one
                    m_bTracyEventEmitted = false;
#endif
                    m_ActivelyLookingForWork = false;
                    return true;
                }
                return false;
            }

            // @brief Check if currently looking for work
            OLO_FINLINE bool IsLookingForWork() const
            {
                return m_ActivelyLookingForWork;
            }
        };

    } // namespace Private

} // namespace OloEngine::LowLevelTasks
