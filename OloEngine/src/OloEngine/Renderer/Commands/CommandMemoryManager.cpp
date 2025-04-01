#include "OloEnginePCH.h"
#include "CommandMemoryManager.h"

namespace OloEngine
{
    // Static member initialization
    std::vector<std::unique_ptr<CommandAllocator>> CommandMemoryManager::s_AllocatorPool;
    std::unordered_map<std::thread::id, CommandAllocator*> CommandMemoryManager::s_ThreadAllocators;
    std::mutex CommandMemoryManager::s_PoolMutex;
    std::mutex CommandMemoryManager::s_ThreadMapMutex;
    std::mutex CommandMemoryManager::s_StatsMutex;
    CommandMemoryManager::Statistics CommandMemoryManager::s_Stats;
    bool CommandMemoryManager::s_Initialized = false;

    void CommandMemoryManager::Init()
    {
        OLO_PROFILE_FUNCTION();
        
        if (s_Initialized)
            return;

        // Create initial pool of allocators
        std::scoped_lock<std::mutex> lock(s_PoolMutex);
        const sizet initialPoolSize = 4; // Start with a few allocators
        
        for (sizet i = 0; i < initialPoolSize; ++i)
        {
            s_AllocatorPool.push_back(std::make_unique<CommandAllocator>());
        }
        
        s_Stats.ActiveAllocatorCount = static_cast<u32>(initialPoolSize);
        s_Initialized = true;
        
        OLO_CORE_INFO("CommandMemoryManager: Initialized with {0} allocators", initialPoolSize);
    }

    void CommandMemoryManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Initialized)
            return;

        // Clear thread allocator mappings
        {
            std::scoped_lock<std::mutex> lock(s_ThreadMapMutex);
            s_ThreadAllocators.clear();
        }

        // Clear allocator pool
        {
            std::scoped_lock<std::mutex> lock(s_PoolMutex);
            s_AllocatorPool.clear();
        }

        // Reset statistics
        {
            std::scoped_lock<std::mutex> lock(s_StatsMutex);
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
        std::scoped_lock<std::mutex> poolLock(s_PoolMutex);
        
        if (s_AllocatorPool.empty())
        {
            // Create a new allocator if the pool is empty
            s_AllocatorPool.push_back(std::make_unique<CommandAllocator>());
            
            std::scoped_lock<std::mutex> statsLock(s_StatsMutex);
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
        std::scoped_lock<std::mutex> lock(s_PoolMutex);
        s_AllocatorPool.push_back(std::unique_ptr<CommandAllocator>(allocator));
        
        OLO_CORE_TRACE("CommandMemoryManager: Returned allocator to pool, available: {0}", 
            s_AllocatorPool.size());
    }

    void CommandMemoryManager::ReleaseCommandPacket(CommandPacket* packet)
    {
        if (!packet || !s_Initialized)
            return;

        // In our allocator-based system, we don't actually free individual packets
        // They will be reclaimed when the allocator is reset
        
        // Update statistics
        {
            std::scoped_lock<std::mutex> lock(s_StatsMutex);
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
            std::scoped_lock<std::mutex> lock(s_ThreadMapMutex);
            for (auto& [threadId, allocator] : s_ThreadAllocators)
            {
                if (allocator)
                    allocator->Reset();
            }
        }

        // Reset pool allocators
        {
            std::scoped_lock<std::mutex> lock(s_PoolMutex);
            for (auto& allocator : s_AllocatorPool)
            {
                allocator->Reset();
            }
        }

        // Reset frame statistics
        {
            std::scoped_lock<std::mutex> lock(s_StatsMutex);
            s_Stats.FramePacketCount = 0;
            s_Stats.ActivePacketCount = 0;
        }
        
        OLO_CORE_TRACE("CommandMemoryManager: All allocators reset for new frame");
    }

    CommandMemoryManager::Statistics CommandMemoryManager::GetStatistics()
    {
        std::scoped_lock<std::mutex> lock(s_StatsMutex);
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
            std::scoped_lock<std::mutex> lock(s_ThreadMapMutex);
            auto it = s_ThreadAllocators.find(threadId);
            if (it != s_ThreadAllocators.end() && it->second)
                return it->second;
        }
        
        // Thread doesn't have an allocator, get one from the pool
        CommandAllocator* allocator = GetFrameAllocator();
        
        // Register this allocator for this thread
        if (allocator)
        {
            std::scoped_lock<std::mutex> lock(s_ThreadMapMutex);
            s_ThreadAllocators[threadId] = allocator;
            OLO_CORE_TRACE("CommandMemoryManager: Assigned allocator to thread ID {0}", 
                static_cast<void*>(&threadId));
        }
        
        return allocator;
    }
}