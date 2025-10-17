#include "OloEnginePCH.h"
#include "LockFreeAllocator.h"

#include <cstdlib>

namespace OloEngine
{
    LockFreeAllocator::LockFreeAllocator(sizet blockSize, sizet initialCapacity, sizet alignment)
        : m_BlockSize(blockSize)
        , m_Alignment(alignment)
    {
        OLO_PROFILE_FUNCTION();

        // Validate parameters
        OLO_CORE_ASSERT(blockSize >= sizeof(void*), "Block size must be at least sizeof(void*)");
        OLO_CORE_ASSERT(alignment > 0 && (alignment & (alignment - 1)) == 0, "Alignment must be a power of 2");
        OLO_CORE_ASSERT(initialCapacity > 0, "Initial capacity must be greater than 0");

        // Ensure block size is aligned
        m_BlockSize = (blockSize + alignment - 1) & ~(alignment - 1);

        // Allocate initial chunk
        if (!AllocateChunk(initialCapacity))
        {
            OLO_CORE_ERROR("LockFreeAllocator: Failed to allocate initial chunk");
        }
    }

    LockFreeAllocator::~LockFreeAllocator()
    {
        OLO_PROFILE_FUNCTION();
        FreeAllChunks();
    }

    LockFreeAllocator::LockFreeAllocator(LockFreeAllocator&& other) noexcept
        : m_BlockSize(other.m_BlockSize)
        , m_Alignment(other.m_Alignment)
        , m_ChunkList(other.m_ChunkList)
    {
        m_FreeList.store(other.m_FreeList.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_Capacity.store(other.m_Capacity.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_FreeCount.store(other.m_FreeCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
        
        // Clear other's state
        other.m_FreeList.store(nullptr, std::memory_order_relaxed);
        other.m_Capacity.store(0, std::memory_order_relaxed);
        other.m_FreeCount.store(0, std::memory_order_relaxed);
        other.m_ChunkList = nullptr;
    }

    LockFreeAllocator& LockFreeAllocator::operator=(LockFreeAllocator&& other) noexcept
    {
        if (this != &other)
        {
            // Free our existing resources
            FreeAllChunks();

            // Move other's resources
            m_BlockSize = other.m_BlockSize;
            m_Alignment = other.m_Alignment;
            m_ChunkList = other.m_ChunkList;
            m_FreeList.store(other.m_FreeList.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_Capacity.store(other.m_Capacity.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_FreeCount.store(other.m_FreeCount.load(std::memory_order_relaxed), std::memory_order_relaxed);

            // Clear other's state
            other.m_FreeList.store(nullptr, std::memory_order_relaxed);
            other.m_Capacity.store(0, std::memory_order_relaxed);
            other.m_FreeCount.store(0, std::memory_order_relaxed);
            other.m_ChunkList = nullptr;
        }
        return *this;
    }

    void* LockFreeAllocator::Allocate()
    {
        OLO_PROFILE_FUNCTION();

        // Lock-free pop from free list (Treiber stack)
        // OPTIMIZATION: Initial read can be relaxed - CAS will re-validate
        FreeNode* node = m_FreeList.load(std::memory_order_relaxed);
        
        while (node != nullptr)
        {
            FreeNode* next = node->Next;
            
            // Try to CAS the head of the free list
            // OPTIMIZATION: On CAS failure, we don't need acquire - just retry with new value
            // The successful CAS provides acquire semantics automatically
            if (m_FreeList.compare_exchange_weak(
                node, next,
                std::memory_order_acquire,  // Success: acquire ownership of node
                std::memory_order_relaxed))  // Failure: just reload and retry
            {
                // Successfully allocated
                m_FreeCount.fetch_sub(1, std::memory_order_relaxed);
                return node;
            }
            
            // CAS failed, node was updated by another thread, retry
            // Note: compare_exchange_weak updates 'node' with current value on failure
        }
        
        // Free list is empty
        // OLO_CORE_WARN("LockFreeAllocator: Free list exhausted (capacity: {0}, block size: {1})", 
        //     m_Capacity.load(std::memory_order_relaxed), m_BlockSize);
        return nullptr;
    }

    void LockFreeAllocator::Free(void* ptr)
    {
        OLO_PROFILE_FUNCTION();

        if (!ptr)
        {
            // Silently return for null pointer (common pattern in allocators)
            return;
        }

        // Lock-free push onto free list (Treiber stack)
        FreeNode* node = static_cast<FreeNode*>(ptr);
        FreeNode* oldHead = m_FreeList.load(std::memory_order_relaxed);
        
        do
        {
            node->Next = oldHead;
        }
        while (!m_FreeList.compare_exchange_weak(
            oldHead, node,
            std::memory_order_release,
            std::memory_order_relaxed));

        m_FreeCount.fetch_add(1, std::memory_order_relaxed);
    }

    bool LockFreeAllocator::AllocateChunk(sizet numBlocks)
    {
        OLO_PROFILE_FUNCTION();

        // Allocate aligned memory from system
        // Total size = numBlocks * blockSize
        sizet totalSize = numBlocks * m_BlockSize;
        
#if defined(_WIN32)
        void* memory = _aligned_malloc(totalSize, m_Alignment);
#else
        void* memory = std::aligned_alloc(m_Alignment, totalSize);
#endif

        if (!memory)
        {
            OLO_CORE_ERROR("LockFreeAllocator: Failed to allocate chunk ({0} blocks, {1} bytes total)", 
                numBlocks, totalSize);
            return false;
        }

        // Create chunk node (not from pool, this is for tracking)
        MemoryChunk* chunk = new MemoryChunk();
        chunk->Memory = memory;
        chunk->NumBlocks = numBlocks;
        chunk->Next = m_ChunkList;
        m_ChunkList = chunk;

        // Build free list from this chunk
        // We need to push all blocks onto the free list atomically as a batch
        // to avoid contention during initialization
        
        // First, link all blocks in this chunk together
        u8* blockPtr = static_cast<u8*>(memory);
        FreeNode* firstNode = reinterpret_cast<FreeNode*>(blockPtr);
        FreeNode* prevNode = firstNode;
        
        for (sizet i = 1; i < numBlocks; ++i)
        {
            blockPtr += m_BlockSize;
            FreeNode* node = reinterpret_cast<FreeNode*>(blockPtr);
            prevNode->Next = node;
            prevNode = node;
        }

        // Now we have a chain: firstNode -> ... -> prevNode
        // We need to atomically prepend this chain to the free list
        FreeNode* oldHead = m_FreeList.load(std::memory_order_relaxed);
        
        do
        {
            // Link the last node in our chain to the old head
            prevNode->Next = oldHead;
        }
        while (!m_FreeList.compare_exchange_weak(
            oldHead, firstNode,
            std::memory_order_release,
            std::memory_order_relaxed));

        // Update counters
        m_Capacity.fetch_add(numBlocks, std::memory_order_relaxed);
        m_FreeCount.fetch_add(numBlocks, std::memory_order_relaxed);

        return true;
    }

    void LockFreeAllocator::FreeAllChunks()
    {
        OLO_PROFILE_FUNCTION();

        // Free all memory chunks
        MemoryChunk* chunk = m_ChunkList;
        while (chunk)
        {
            MemoryChunk* next = chunk->Next;
            
#if defined(_WIN32)
            _aligned_free(chunk->Memory);
#else
            std::free(chunk->Memory);
#endif

            delete chunk;
            chunk = next;
        }

        m_ChunkList = nullptr;
        m_FreeList.store(nullptr, std::memory_order_relaxed);
        m_Capacity.store(0, std::memory_order_relaxed);
        m_FreeCount.store(0, std::memory_order_relaxed);
    }

} // namespace OloEngine
