#pragma once

#include "OloEngine/Core/Base.h"
#include "CommandPacket.h"
#include <thread>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>

namespace OloEngine
{
    // Memory block for storing command data
    struct MemoryBlock
    {
        u8* Data = nullptr;
        sizet Size = 0;
        sizet Offset = 0;
        MemoryBlock* Next = nullptr;
    };

    // Thread-local memory cache for command allocations
    class ThreadLocalCache
    {
    public:
        ThreadLocalCache(sizet blockSize);
        ~ThreadLocalCache();

        // Disallow copying
        ThreadLocalCache(const ThreadLocalCache&) = delete;
        ThreadLocalCache& operator=(const ThreadLocalCache&) = delete;

        // Move operations
        ThreadLocalCache(ThreadLocalCache&& other) noexcept;
        ThreadLocalCache& operator=(ThreadLocalCache&& other) noexcept;

        // Allocate memory for a command
        void* Allocate(sizet size, sizet alignment = 8);
        
        // Reset the allocator - doesn't free memory, just resets offsets
        void Reset();
        
        // Completely free all memory
        void FreeAll();
        
        // Add a new block to the chain
        void AddBlock(sizet minSize);

    private:
        MemoryBlock* m_CurrentBlock = nullptr;
        MemoryBlock* m_FirstBlock = nullptr;
        sizet m_DefaultBlockSize = 0;
        sizet m_TotalAllocated = 0;
        sizet m_WastedMemory = 0;
    };

    // Command allocator for efficient command packet memory management
    class CommandAllocator
    {
    public:
        static const sizet DEFAULT_BLOCK_SIZE = 64 * 1024; // 64KB blocks
        static const sizet MAX_COMMAND_SIZE = 256; // Maximum size of any command
        static const sizet COMMAND_ALIGNMENT = 16; // Ensure commands are aligned properly
        
        CommandAllocator(sizet blockSize = DEFAULT_BLOCK_SIZE);
        ~CommandAllocator();
        
        // Disallow copying
        CommandAllocator(const CommandAllocator&) = delete;
        CommandAllocator& operator=(const CommandAllocator&) = delete;
        
        // Move operations
        CommandAllocator(CommandAllocator&& other) noexcept;
        CommandAllocator& operator=(CommandAllocator&& other) noexcept;
        
        // Allocate memory for a command
        void* AllocateCommandMemory(sizet size);
        
        // Create a command packet with the given command data
        template<typename T>
        CommandPacket* CreateCommandPacket(const T& commandData, const PacketMetadata& metadata = {})
        {
            static_assert(sizeof(T) <= MAX_COMMAND_SIZE, "Command exceeds maximum size");
            
            // Allocate memory for the CommandPacket
            void* packetMemory = AllocateCommandMemory(sizeof(CommandPacket));
            if (!packetMemory)
                return nullptr;
                
            // Construct a new CommandPacket in the allocated memory
            CommandPacket* packet = new (packetMemory) CommandPacket();
            
            // Initialize the packet with the command data
            packet->Initialize(commandData, metadata);
            
            return packet;
        }
        
        // Reset the allocator - doesn't free memory, just resets offsets
        void Reset();
        
        // Statistics
        sizet GetTotalAllocated() const;
        sizet GetAllocationCount() const { return m_AllocationCount; }
        
    private:
        // Get the thread-local cache for the current thread
        ThreadLocalCache& GetThreadLocalCache();
        
        sizet m_BlockSize;
        std::unordered_map<std::thread::id, ThreadLocalCache> m_ThreadCaches;
        std::mutex m_CachesLock;
        std::atomic<sizet> m_AllocationCount{0};
    };
}