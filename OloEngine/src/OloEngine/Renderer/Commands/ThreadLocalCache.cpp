#include "OloEnginePCH.h"
#include "ThreadLocalCache.h"
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
        {
            return nullptr;
        }

        // Validate alignment to avoid undefined behavior
        OLO_CORE_ASSERT(alignment > 0, "Alignment must be greater than 0");

        // Calculate aligned address
        auto currentAddr = reinterpret_cast<sizet>(m_CurrentBlock->Data + m_CurrentBlock->Offset);
        sizet alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
        sizet alignmentPadding = alignedAddr - currentAddr;

        // If not enough space in current block, allocate a new one
        if (sizet alignedSize = size + alignmentPadding; m_CurrentBlock->Offset + alignedSize > m_CurrentBlock->Size)
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
        auto* newBlock = new MemoryBlock();
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
} // namespace OloEngine
