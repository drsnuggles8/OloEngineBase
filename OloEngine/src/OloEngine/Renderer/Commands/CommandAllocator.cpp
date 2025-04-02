#include "OloEnginePCH.h"
#include "CommandAllocator.h"
#include "ThreadLocalCache.h"
#include <cstdlib>
#include <algorithm>

namespace OloEngine
{
    CommandAllocator::CommandAllocator(sizet blockSize)
        : m_BlockSize(blockSize)
    {
        OLO_CORE_ASSERT(blockSize > 0, "Block size must be greater than 0");
    }

    CommandAllocator::CommandAllocator(CommandAllocator&& other) noexcept
        : m_BlockSize(other.m_BlockSize),
          m_ThreadCaches(std::move(other.m_ThreadCaches)),
          m_AllocationCount(other.m_AllocationCount.load())
    {
    }

    CommandAllocator& CommandAllocator::operator=(CommandAllocator&& other) noexcept
    {
        if (this != &other)
        {
            m_BlockSize = other.m_BlockSize;
            m_ThreadCaches = std::move(other.m_ThreadCaches);
            m_AllocationCount.store(other.m_AllocationCount.load());
        }
        return *this;
    }

    void* CommandAllocator::AllocateCommandMemory(sizet size)
    {
        OLO_PROFILE_FUNCTION();

        if (size > MAX_COMMAND_SIZE)
        {
            OLO_CORE_ERROR("CommandAllocator: Requested size {0} exceeds maximum command size {1}", size, MAX_COMMAND_SIZE);
            return nullptr;
        }

        // Increment allocation count
		m_AllocationCount.fetch_add(1, std::memory_order_relaxed);

        // Get thread-local cache and allocate memory
        ThreadLocalCache& cache = GetThreadLocalCache();
        return cache.Allocate(size, COMMAND_ALIGNMENT);
    }

    void CommandAllocator::Reset()
    {
        OLO_PROFILE_FUNCTION();

        // Lock to prevent other threads from adding caches during reset
        std::scoped_lock<std::mutex> lock(m_CachesLock);

        // Reset all thread caches
		for (auto& [threadId, cache] : m_ThreadCaches)
		{
			cache.Reset();
		}

        // Reset allocation counter
        m_AllocationCount.store(0);
    }

    sizet CommandAllocator::GetTotalAllocated() const
    {
		std::scoped_lock<std::mutex> lock(m_CachesLock);

        sizet total = 0;
		for (const auto& [threadId, cache] : m_ThreadCaches)
		{
			total += cache.GetTotalAllocated();
		}
        
        return total;
    }

	ThreadLocalCache& CommandAllocator::GetThreadLocalCache()
	{
		OLO_PROFILE_FUNCTION();

		// Get current thread ID
		std::thread::id threadId = std::this_thread::get_id();

		// Acquire lock once for the entire operation
		std::scoped_lock<std::mutex> lock(m_CachesLock);

		// try_emplace will only insert if the key doesn't exist
		// - If threadId already exists in m_ThreadCaches, no insertion occurs
		// - If threadId doesn't exist, a new ThreadLocalCache is created and inserted
		auto [iter, inserted] = m_ThreadCaches.try_emplace(threadId, m_BlockSize);

		if (inserted)
		{
			OLO_CORE_TRACE("CommandAllocator: Created new thread cache for thread ID {0}",
			static_cast<void*>(&threadId));
		}

		// Return reference to the cache (either existing or newly created)
		return iter->second;
	}

}
