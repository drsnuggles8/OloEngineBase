#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include <vector>
#include <mutex>
#include <array>

namespace OloEngine
{
    // Memory block for command allocation
    struct CommandMemoryBlock
    {
        u8* Memory = nullptr;
        sizet Size = 0;
        sizet Used = 0;
        CommandMemoryBlock* Next = nullptr;
        
        explicit CommandMemoryBlock(sizet size);
        ~CommandMemoryBlock();
        
        void* Allocate(sizet size, sizet alignment);
        void Reset();
    };
    
    // Main allocator for command packets and auxiliary memory
    class CommandAllocator
    {
    public:
        static constexpr sizet DefaultBlockSize = 64 * 1024; // 64KB default block size
        
        explicit CommandAllocator(sizet blockSize = DefaultBlockSize);
        ~CommandAllocator();
        
        // Allocate a command packet with header and data
        CommandPacket* AllocateCommandPacket(sizet commandSize, CommandType type);
        
        // Allocate auxiliary memory for arrays, etc.
        void* AllocateAuxMemory(sizet size, sizet alignment = 16);
        
        // Reset the allocator (doesn't free memory, just resets usage)
        void Reset();
        
        // Get the number of memory blocks currently in use
        [[nodiscard]] sizet GetBlockCount() const;
        
    private:
        // Release all allocated memory
        void ReleaseMemory();
        
        std::vector<CommandMemoryBlock*> m_Blocks;
        CommandMemoryBlock* m_CurrentBlock = nullptr;
        sizet m_BlockSize;
        mutable std::mutex m_Mutex;
    };
    
    // Thread-local command allocator for improved performance
    class ThreadLocalCommandAllocator
    {
    public:
        explicit ThreadLocalCommandAllocator(CommandAllocator& parentAllocator);
        
        // Allocate a command packet with header and data
        CommandPacket* AllocateCommandPacket(sizet commandSize, CommandType type);
        
        // Allocate auxiliary memory for arrays, etc.
        void* AllocateAuxMemory(sizet size, sizet alignment = 16);
        
        // Reset the local cache
        void Reset();
        
    private:
        CommandAllocator& m_ParentAllocator;
        std::array<u8, 16 * 1024> m_LocalCache; // 16KB local cache
        sizet m_Used = 0;
    };
}