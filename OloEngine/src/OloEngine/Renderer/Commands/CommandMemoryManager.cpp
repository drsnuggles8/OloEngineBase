#include "OloEnginePCH.h"
#include "CommandMemoryManager.h"
#include "OloEngine/Threading/UniqueLock.h"
#include <cstring>

namespace OloEngine
{
    // Static member initialization
    std::vector<std::unique_ptr<CommandAllocator>> CommandMemoryManager::s_AllocatorPool;
    std::unordered_map<std::thread::id, CommandAllocator*> CommandMemoryManager::s_ThreadAllocators;
    FMutex CommandMemoryManager::s_PoolMutex;
    FMutex CommandMemoryManager::s_ThreadMapMutex;
    FMutex CommandMemoryManager::s_StatsMutex;
    CommandMemoryManager::Statistics CommandMemoryManager::s_Stats;
    bool CommandMemoryManager::s_Initialized = false;

    // Per-worker allocator storage initialization
    std::array<WorkerAllocatorSlot, MAX_ALLOCATOR_WORKERS> CommandMemoryManager::s_WorkerAllocators = {};

    void CommandMemoryManager::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Initialized)
            return;

        // Create initial pool of allocators
        TUniqueLock<FMutex> lock(s_PoolMutex);
        const sizet initialPoolSize = 4; // Start with a few allocators

        for (sizet i = 0; i < initialPoolSize; ++i)
        {
            s_AllocatorPool.push_back(std::make_unique<CommandAllocator>());
        }

        // Initialize per-worker allocators
        for (u32 i = 0; i < MAX_ALLOCATOR_WORKERS; ++i)
        {
            s_WorkerAllocators[i].allocator = new CommandAllocator();
            s_WorkerAllocators[i].inUse.store(false, std::memory_order_relaxed);
        }

        s_Stats.ActiveAllocatorCount = static_cast<u32>(initialPoolSize + MAX_ALLOCATOR_WORKERS);
        s_Initialized = true;

        OLO_CORE_INFO("CommandMemoryManager: Initialized with {0} pool allocators and {1} worker allocators",
                      initialPoolSize, MAX_ALLOCATOR_WORKERS);
    }

    void CommandMemoryManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
            return;

        // Clear thread allocator mappings
        {
            TUniqueLock<FMutex> lock(s_ThreadMapMutex);
            s_ThreadAllocators.clear();
        }

        // Free per-worker allocators
        for (u32 i = 0; i < MAX_ALLOCATOR_WORKERS; ++i)
        {
            delete s_WorkerAllocators[i].allocator;
            s_WorkerAllocators[i].allocator = nullptr;
            s_WorkerAllocators[i].inUse.store(false, std::memory_order_relaxed);
        }

        // Clear allocator pool
        {
            TUniqueLock<FMutex> lock(s_PoolMutex);
            s_AllocatorPool.clear();
        }

        // Reset statistics
        {
            TUniqueLock<FMutex> lock(s_StatsMutex);
            s_Stats = Statistics{};
        }

        s_Initialized = false;
        OLO_CORE_INFO("CommandMemoryManager: Shutdown completed");
    }

    CommandAllocator* CommandMemoryManager::GetFrameAllocator()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("CommandMemoryManager: Not initialized!");
            return nullptr;
        }

        // Try to get an allocator from the pool
        TUniqueLock<FMutex> poolLock(s_PoolMutex);

        if (s_AllocatorPool.empty())
        {
            // Create a new allocator if the pool is empty
            s_AllocatorPool.push_back(std::make_unique<CommandAllocator>());

            TUniqueLock<FMutex> statsLock(s_StatsMutex);
            s_Stats.ActiveAllocatorCount++;

            OLO_CORE_TRACE("CommandMemoryManager: Created new allocator, total count: {0}",
                           s_Stats.ActiveAllocatorCount);
        }

        // Get an allocator from the pool
        CommandAllocator* allocator = s_AllocatorPool.back().release();
        s_AllocatorPool.pop_back();

        return allocator;
    }

    void CommandMemoryManager::ReturnAllocator(CommandAllocator* allocator)
    {
        OLO_PROFILE_FUNCTION();

        if (!allocator || !s_Initialized)
            return;

        // Reset the allocator before returning it to the pool
        allocator->Reset();

        // Add the allocator back to the pool
        TUniqueLock<FMutex> lock(s_PoolMutex);
        s_AllocatorPool.push_back(std::unique_ptr<CommandAllocator>(allocator));

        // OLO_CORE_TRACE("CommandMemoryManager: Returned allocator to pool, available: {0}", s_AllocatorPool.size());
    }

    void CommandMemoryManager::ReleaseCommandPacket(CommandPacket* packet)
    {
        if (!packet || !s_Initialized)
            return;

        // In our allocator-based system, we don't actually free individual packets
        // They will be reclaimed when the allocator is reset

        // Update statistics
        {
            TUniqueLock<FMutex> lock(s_StatsMutex);
            if (s_Stats.ActivePacketCount > 0)
                s_Stats.ActivePacketCount--;
        }

        // No need to destroy the packet - memory will be reused
    }

    void CommandMemoryManager::ResetAllocators()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
            return;

        // Reset thread allocators
        {
            TUniqueLock<FMutex> lock(s_ThreadMapMutex);
            for (auto& [threadId, allocator] : s_ThreadAllocators)
            {
                if (allocator)
                {
                    allocator->Reset();
                }
            }
        }

        // Reset pool allocators
        {
            TUniqueLock<FMutex> lock(s_PoolMutex);
            for (auto& allocator : s_AllocatorPool)
            {
                allocator->Reset();
            }
        }

        // Reset frame statistics
        {
            TUniqueLock<FMutex> lock(s_StatsMutex);
            s_Stats.FramePacketCount = 0;
            s_Stats.ActivePacketCount = 0;
        }

        OLO_CORE_TRACE("CommandMemoryManager: All allocators reset for new frame");
    }

    CommandMemoryManager::Statistics CommandMemoryManager::GetStatistics()
    {
        TUniqueLock<FMutex> lock(s_StatsMutex);
        return s_Stats;
    }

    CommandAllocator* CommandMemoryManager::GetCurrentThreadAllocator()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("CommandMemoryManager: Not initialized!");
            return nullptr;
        }

        std::thread::id threadId = std::this_thread::get_id();

        // Check if this thread already has an allocator
        {
            TUniqueLock<FMutex> lock(s_ThreadMapMutex);
            auto it = s_ThreadAllocators.find(threadId);
            if (it != s_ThreadAllocators.end() && it->second)
                return it->second;
        }

        // Thread doesn't have an allocator, get one from the pool
        CommandAllocator* allocator = GetFrameAllocator();

        // Register this allocator for this thread
        if (allocator)
        {
            TUniqueLock<FMutex> lock(s_ThreadMapMutex);
            s_ThreadAllocators[threadId] = allocator;
            OLO_CORE_TRACE("CommandMemoryManager: Assigned allocator to thread ID {0}", static_cast<void*>(&threadId));
        }

        return allocator;
    }

    // ========================================================================
    // Per-Worker Allocator API Implementation
    // ========================================================================

    CommandAllocator* CommandMemoryManager::GetWorkerAllocator(u32 workerIndex)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(s_Initialized, "CommandMemoryManager: Not initialized!");
        OLO_CORE_ASSERT(workerIndex < MAX_ALLOCATOR_WORKERS,
                        "CommandMemoryManager: Invalid worker index {}!", workerIndex);

        WorkerAllocatorSlot& slot = s_WorkerAllocators[workerIndex];

        // Mark allocator as in use
        bool expected = false;
        if (!slot.inUse.compare_exchange_strong(expected, true, std::memory_order_acquire))
        {
            OLO_CORE_WARN("CommandMemoryManager: Worker allocator {} already in use!", workerIndex);
        }

        return slot.allocator;
    }

    void CommandMemoryManager::PrepareWorkerAllocatorsForFrame()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("CommandMemoryManager: Not initialized!");
            return;
        }

        // Reset all worker allocators for the new frame
        for (u32 i = 0; i < MAX_ALLOCATOR_WORKERS; ++i)
        {
            if (s_WorkerAllocators[i].allocator)
            {
                s_WorkerAllocators[i].allocator->Reset();
            }
            // Mark as available for use
            s_WorkerAllocators[i].inUse.store(false, std::memory_order_release);
        }

        OLO_CORE_TRACE("CommandMemoryManager: Prepared {} worker allocators for frame",
                       MAX_ALLOCATOR_WORKERS);
    }

    void CommandMemoryManager::ReleaseWorkerAllocators()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
            return;

        // Mark all worker allocators as not in use
        for (u32 i = 0; i < MAX_ALLOCATOR_WORKERS; ++i)
        {
            s_WorkerAllocators[i].inUse.store(false, std::memory_order_release);
        }

        OLO_CORE_TRACE("CommandMemoryManager: Released all worker allocators");
    }

    std::pair<u32, CommandAllocator*> CommandMemoryManager::GetWorkerAllocatorByIndex(u32 workerIndex)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("CommandMemoryManager: Not initialized!");
            return { 0, nullptr };
        }

        if (workerIndex >= MAX_ALLOCATOR_WORKERS)
        {
            OLO_CORE_ERROR("CommandMemoryManager: Worker index {} exceeds max {}!",
                           workerIndex, MAX_ALLOCATOR_WORKERS);
            return { 0, nullptr };
        }

        // Directly access the worker allocator by index - no thread ID lookup needed
        // This is the optimized path for ParallelFor where contextIndex is already known
        CommandAllocator* allocator = GetWorkerAllocator(workerIndex);
        return { workerIndex, allocator };
    }
} // namespace OloEngine
