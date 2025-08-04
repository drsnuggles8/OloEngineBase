#pragma once

#include "OloEngine/Core/Base.h"
#include "CommandPacket.h"
#include "ThreadLocalCache.h" // Added include for ThreadLocalCache
#include <thread>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>

namespace OloEngine
{
    // Command allocator for efficient command packet memory management
    class CommandAllocator : public RefCounted
    {
    public:
        static const sizet DEFAULT_BLOCK_SIZE = 64 * 1024; // 64KB blocks
        static const sizet MAX_COMMAND_SIZE = 1024; // Maximum size of any command (increased for PBR)
        static const sizet COMMAND_ALIGNMENT = 16; // Ensure commands are aligned properly
        
        explicit CommandAllocator(sizet blockSize = DEFAULT_BLOCK_SIZE);
        ~CommandAllocator() = default;
        
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
            auto* packet = new (packetMemory) CommandPacket();
            
            // Initialize the packet with the command data
            packet->Initialize(commandData, metadata);
            
            return packet;
        }
		
        template<typename T>
        CommandPacket* AllocatePacketWithCommand(const PacketMetadata& metadata = {})
        {
            constexpr sizet packetSize = sizeof(CommandPacket);
            constexpr sizet commandSize = sizeof(T);
            constexpr sizet totalSize = packetSize + commandSize;
            void* block = AllocateCommandMemory(totalSize);
            OLO_CORE_ASSERT(block, "CommandAllocator::AllocatePacketWithCommand: Allocation failed!");
            // Placement-new the packet at the start
            auto* packet = new (block) CommandPacket();
            // Placement-new the command immediately after
            void* commandMem = static_cast<u8*>(block) + packetSize;
            T* cmd = new (commandMem) T();
			
            packet->SetCommandData(cmd, commandSize);
            packet->SetMetadata(metadata);
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
		mutable std::mutex m_CachesLock;
        std::atomic<sizet> m_AllocationCount{0};
    };
}
