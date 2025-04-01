#pragma once

#include "CommandPacket.h"
#include "OloEngine/Core/Base.h"
#include <vector>

namespace OloEngine
{
    // Command packet list - a linked list of command packets
    class CommandPacketList
    {
    public:
        CommandPacketList() = default;
        ~CommandPacketList();
        
        // Disallow copying
        CommandPacketList(const CommandPacketList&) = delete;
        CommandPacketList& operator=(const CommandPacketList&) = delete;
        
        // Allow moving
        CommandPacketList(CommandPacketList&& other) noexcept;
        CommandPacketList& operator=(CommandPacketList&& other) noexcept;

        // Add a new packet to the list
        CommandPacket* AllocatePacket();
        
        // Initialize a packet with command data and add it to the list
        template<typename T>
        CommandPacket* CreatePacket(const T& commandData, const PacketMetadata& metadata = {})
        {
            CommandPacket* packet = AllocatePacket();
            if (packet)
            {
                packet->Initialize(commandData, metadata);
            }
            return packet;
        }
        
        // Execute all commands in the list
        void Execute(RendererAPI& api);
        
        // Clear the list, returning all packets to the pool
        void Clear();
        
        // Sort the packets based on keys for optimal rendering
        void Sort();
        
        // Try to batch compatible packets together
        void BatchPackets();
        
        // Statistics
        u32 GetPacketCount() const { return m_PacketCount; }
        
    private:
        // Helper to allocate a new packet from the heap
        CommandPacket* AllocateNewPacket();
        
        // Linked list management
        CommandPacket* m_Head = nullptr;
        CommandPacket* m_Tail = nullptr;
        u32 m_PacketCount = 0;
        
        // Static packet pool for reuse
        static constexpr u32 INITIAL_POOL_SIZE = 1000;
        static constexpr u32 GROWTH_SIZE = 500;
        static std::vector<CommandPacket*> s_PacketPool;
    };
}