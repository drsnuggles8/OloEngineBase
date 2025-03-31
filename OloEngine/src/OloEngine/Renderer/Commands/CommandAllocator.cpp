#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"

namespace OloEngine
{
    CommandMemoryBlock::CommandMemoryBlock(sizet size)
        : Size(size)
    {
        OLO_PROFILE_FUNCTION();
        Memory = static_cast<u8*>(malloc(size));
        OLO_CORE_ASSERT(Memory, "Failed to allocate command memory block!");
    }

    CommandMemoryBlock::~CommandMemoryBlock()
    {
        if (Memory)
        {
            free(Memory);
            Memory = nullptr;
        }
    }

    void* CommandMemoryBlock::Allocate(sizet size, sizet alignment)
    {
        if (size == 0)
            return nullptr;

        // Calculate aligned address
        uintptr_t currentPtr = reinterpret_cast<uintptr_t>(Memory + Used);
        uintptr_t alignedPtr = (currentPtr + alignment - 1) & ~(alignment - 1);
        sizet alignmentPadding = alignedPtr - currentPtr;
        
        // Check if we have enough space
        sizet totalSize = size + alignmentPadding;
        if (Used + totalSize > Size)
            return nullptr;
            
        // Update used counter and return aligned pointer
        Used += totalSize;
        return reinterpret_cast<void*>(alignedPtr);
    }

    void CommandMemoryBlock::Reset()
    {
        Used = 0;
    }

    CommandAllocator::CommandAllocator(sizet blockSize)
        : m_BlockSize(blockSize)
    {
        OLO_PROFILE_FUNCTION();
        
        // Allocate initial block
        m_CurrentBlock = new CommandMemoryBlock(m_BlockSize);
        m_Blocks.push_back(m_CurrentBlock);
    }

    CommandAllocator::~CommandAllocator()
    {
        ReleaseMemory();
    }

    CommandPacket* CommandAllocator::AllocateCommandPacket(sizet commandSize, CommandType type)
    {
        OLO_PROFILE_FUNCTION();
        
        // Calculate total size needed (CommandPacket + command data)
        sizet totalSize = sizeof(CommandPacket);
        
        // Lock to ensure thread safety
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Try to allocate from current block
        void* memory = m_CurrentBlock->Allocate(totalSize, alignof(CommandPacket));
        
        // If current block is full, create a new one
        if (!memory)
        {
            auto* newBlock = new CommandMemoryBlock(m_BlockSize);
            m_Blocks.push_back(newBlock);
            
            // Link blocks
            newBlock->Next = nullptr;
            m_CurrentBlock->Next = newBlock;
            m_CurrentBlock = newBlock;
            
            // Try allocation again
            memory = m_CurrentBlock->Allocate(totalSize, alignof(CommandPacket));
            
            if (!memory)
            {
                OLO_CORE_ERROR("CommandAllocator: Failed to allocate memory for CommandPacket!");
                return nullptr;
            }
        }
        
        // Initialize the command packet
        CommandPacket* packet = static_cast<CommandPacket*>(memory);
        new (packet) CommandPacket(); // Placement new to initialize
        
        // Setup the command header
        packet->Header.Type = type;
        packet->Header.Size = static_cast<u16>(commandSize);
        packet->Header.Flags = 0;
        
        return packet;
    }

    void* CommandAllocator::AllocateAuxMemory(sizet size, sizet alignment)
    {
        OLO_PROFILE_FUNCTION();
        
        if (size == 0)
            return nullptr;
            
        // Lock to ensure thread safety
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Try to allocate from current block
        void* memory = m_CurrentBlock->Allocate(size, alignment);
        
        // If current block is full, create a new one
        if (!memory)
        {
            // For large allocations, create a custom sized block
            sizet blockSize = size > m_BlockSize ? size * 2 : m_BlockSize;
            
            auto* newBlock = new CommandMemoryBlock(blockSize);
            m_Blocks.push_back(newBlock);
            
            // Link blocks
            newBlock->Next = nullptr;
            m_CurrentBlock->Next = newBlock;
            m_CurrentBlock = newBlock;
            
            // Try allocation again
            memory = m_CurrentBlock->Allocate(size, alignment);
            
            if (!memory)
            {
                OLO_CORE_ERROR("CommandAllocator: Failed to allocate auxiliary memory!");
                return nullptr;
            }
        }
        
        return memory;
    }

    void CommandAllocator::Reset()
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Reset all blocks for reuse
        for (auto* block : m_Blocks)
        {
            block->Reset();
        }
        
        // Start again from the first block
        if (!m_Blocks.empty())
            m_CurrentBlock = m_Blocks[0];
    }

    sizet CommandAllocator::GetBlockCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Blocks.size();
    }

    void CommandAllocator::ReleaseMemory()
    {
        OLO_PROFILE_FUNCTION();
        
        // Delete all blocks
        for (auto* block : m_Blocks)
        {
            delete block;
        }
        
        m_Blocks.clear();
        m_CurrentBlock = nullptr;
    }

    // Thread-local allocator implementation
    ThreadLocalCommandAllocator::ThreadLocalCommandAllocator(CommandAllocator& parentAllocator)
        : m_ParentAllocator(parentAllocator), m_Used(0)
    {
    }

    CommandPacket* ThreadLocalCommandAllocator::AllocateCommandPacket(sizet commandSize, CommandType type)
    {
        // For small commands, try to allocate from local cache
        sizet totalSize = sizeof(CommandPacket);
        
        if (totalSize <= 128 && m_Used + totalSize <= m_LocalCache.size())
        {
            // Calculate aligned address
            uintptr_t currentPtr = reinterpret_cast<uintptr_t>(m_LocalCache.data() + m_Used);
            uintptr_t alignedPtr = (currentPtr + alignof(CommandPacket) - 1) & ~(alignof(CommandPacket) - 1);
            sizet alignmentPadding = alignedPtr - currentPtr;
            
            // Check if we have enough space after alignment
            if (m_Used + alignmentPadding + totalSize <= m_LocalCache.size())
            {
                // Allocate from local cache
                m_Used += alignmentPadding + totalSize;
                
                // Initialize the command packet
                CommandPacket* packet = reinterpret_cast<CommandPacket*>(alignedPtr);
                new (packet) CommandPacket(); // Placement new to initialize
                
                // Setup the command header
                packet->Header.Type = type;
                packet->Header.Size = static_cast<u16>(commandSize);
                packet->Header.Flags = 0;
                
                return packet;
            }
        }
        
        // Fall back to parent allocator
        return m_ParentAllocator.AllocateCommandPacket(commandSize, type);
    }

    void* ThreadLocalCommandAllocator::AllocateAuxMemory(sizet size, sizet alignment)
    {
        // For small allocations, try local cache first
        if (size <= 64 && m_Used + size + alignment <= m_LocalCache.size())
        {
            // Calculate aligned address
            uintptr_t currentPtr = reinterpret_cast<uintptr_t>(m_LocalCache.data() + m_Used);
            uintptr_t alignedPtr = (currentPtr + alignment - 1) & ~(alignment - 1);
            sizet alignmentPadding = alignedPtr - currentPtr;
            
            // Check if we have enough space after alignment
            if (m_Used + alignmentPadding + size <= m_LocalCache.size())
            {
                // Allocate from local cache
                void* memory = reinterpret_cast<void*>(alignedPtr);
                m_Used += alignmentPadding + size;
                return memory;
            }
        }
        
        // Fall back to parent allocator
        return m_ParentAllocator.AllocateAuxMemory(size, alignment);
    }

    void ThreadLocalCommandAllocator::Reset()
    {
        m_Used = 0;
    }
}