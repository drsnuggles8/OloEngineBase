#include "OloEnginePCH.h"
#include "CommandAllocator.h"
#include <cstdlib>
#include <algorithm>

namespace OloEngine
{
    // ThreadLocalCache implementation
    ThreadLocalCache::ThreadLocalCache(sizet blockSize)
        : m_DefaultBlockSize(blockSize)
    {
        OLO_CORE_ASSERT(blockSize > 0, "Block size must be greater than 0");
        AddBlock(m_DefaultBlockSize);
    }

    ThreadLocalCache::~ThreadLocalCache()
    {
        FreeAll();
    }

    ThreadLocalCache::ThreadLocalCache(ThreadLocalCache&& other) noexcept
        : m_CurrentBlock(other.m_CurrentBlock), 
          m_FirstBlock(other.m_FirstBlock),
          m_DefaultBlockSize(other.m_DefaultBlockSize),
          m_TotalAllocated(other.m_TotalAllocated),
          m_WastedMemory(other.m_WastedMemory)
    {
        other.m_CurrentBlock = nullptr;
        other.m_FirstBlock = nullptr;
    }

    ThreadLocalCache& ThreadLocalCache::operator=(ThreadLocalCache&& other) noexcept
    {
        if (this != &other)
        {
            FreeAll();

            m_CurrentBlock = other.m_CurrentBlock;
            m_FirstBlock = other.m_FirstBlock;
            m_DefaultBlockSize = other.m_DefaultBlockSize;
            m_TotalAllocated = other.m_TotalAllocated;
            m_WastedMemory = other.m_WastedMemory;

            other.m_CurrentBlock = nullptr;
            other.m_FirstBlock = nullptr;
        }
        return *this;
    }

    void* ThreadLocalCache::Allocate(sizet size, sizet alignment)
    {
        OLO_PROFILE_FUNCTION();

        if (size == 0)
            return nullptr;

        // Calculate aligned address
        sizet currentAddr = reinterpret_cast<sizet>(m_CurrentBlock->Data + m_CurrentBlock->Offset);
        sizet alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
        sizet alignmentPadding = alignedAddr - currentAddr;

        sizet alignedSize = size + alignmentPadding;

        // If not enough space in current block, allocate a new one
        if (m_CurrentBlock->Offset + alignedSize > m_CurrentBlock->Size)
        {
            // Amount wasted in the current block
            sizet wasted = m_CurrentBlock->Size - m_CurrentBlock->Offset;
            m_WastedMemory += wasted;

            // Calculate the required block size (at least the default size or the requested size)
            sizet requiredSize = std::max(m_DefaultBlockSize, alignedSize);
            AddBlock(requiredSize);
            
            // Recalculate addresses with the new block
            currentAddr = reinterpret_cast<sizet>(m_CurrentBlock->Data + m_CurrentBlock->Offset);
            alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
            alignmentPadding = alignedAddr - currentAddr;
        }

        // Update the offset in the current block
        m_CurrentBlock->Offset += size + alignmentPadding;
        
        m_TotalAllocated += size;

        // Return aligned pointer
        return reinterpret_cast<void*>(alignedAddr);
    }

    void ThreadLocalCache::Reset()
    {
        // Reset all blocks to their initial state without freeing memory
        MemoryBlock* block = m_FirstBlock;
        while (block)
        {
            block->Offset = 0;
            block = block->Next;
        }
        
        // Reset the current block to the first block
        m_CurrentBlock = m_FirstBlock;
        
        // Reset tracking metrics
        m_TotalAllocated = 0;
        m_WastedMemory = 0;
    }

    void ThreadLocalCache::FreeAll()
    {
        // Free all memory blocks
        MemoryBlock* block = m_FirstBlock;
        while (block)
        {
            MemoryBlock* next = block->Next;
            delete[] block->Data;
            delete block;
            block = next;
        }

        m_FirstBlock = nullptr;
        m_CurrentBlock = nullptr;
        m_TotalAllocated = 0;
        m_WastedMemory = 0;
    }

    void ThreadLocalCache::AddBlock(sizet minSize)
    {
        // Create a new memory block
        MemoryBlock* newBlock = new MemoryBlock();
        newBlock->Size = std::max(minSize, m_DefaultBlockSize);
        newBlock->Offset = 0;
        newBlock->Data = new u8[newBlock->Size];
        newBlock->Next = nullptr;

        // Add to the linked list
        if (!m_FirstBlock)
        {
            m_FirstBlock = newBlock;
            m_CurrentBlock = newBlock;
        }
        else
        {
            m_CurrentBlock->Next = newBlock;
            m_CurrentBlock = newBlock;
        }

        OLO_CORE_TRACE("ThreadLocalCache: Added new block of size {0} bytes", newBlock->Size);
    }

    // CommandAllocator implementation
    CommandAllocator::CommandAllocator(sizet blockSize)
        : m_BlockSize(blockSize)
    {
        OLO_CORE_ASSERT(blockSize > 0, "Block size must be greater than 0");
    }

    CommandAllocator::~CommandAllocator()
    {
        // All caches will be automatically destroyed
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
            OLO_CORE_ERROR("CommandAllocator: Requested size {0} exceeds maximum command size {1}", 
                size, MAX_COMMAND_SIZE);
            return nullptr;
        }

        // Increment allocation count
        m_AllocationCount++;

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
        for (auto& pair : m_ThreadCaches)
        {
            pair.second.Reset();
        }

        // Reset allocation counter
        m_AllocationCount.store(0);
    }

    sizet CommandAllocator::GetTotalAllocated() const
    {
        // Lock to prevent concurrent modifications
        std::scoped_lock<std::mutex> lock(m_CachesLock);

        sizet total = 0;
        for (const auto& pair : m_ThreadCaches)
        {
            // We would need to add a method to get this from ThreadLocalCache
            // For now, we just return the allocation count
        }
        
        return m_AllocationCount;
    }

    ThreadLocalCache& CommandAllocator::GetThreadLocalCache()
    {
        OLO_PROFILE_FUNCTION();

        // Get current thread ID
        std::thread::id threadId = std::this_thread::get_id();

        // Check if we already have a cache for this thread
        {
            std::scoped_lock<std::mutex> lock(m_CachesLock);
            auto it = m_ThreadCaches.find(threadId);
            if (it != m_ThreadCaches.end())
            {
                return it->second;
            }
        }

        // No cache found, create a new one
        std::scoped_lock<std::mutex> lock(m_CachesLock);
        auto result = m_ThreadCaches.emplace(threadId, ThreadLocalCache(m_BlockSize));
        OLO_CORE_TRACE("CommandAllocator: Created new thread cache for thread ID {0}", 
            static_cast<void*>(&threadId));
        return result.first->second;
    }
}