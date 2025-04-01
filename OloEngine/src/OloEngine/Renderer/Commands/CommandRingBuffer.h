#pragma once

#include "CommandPacket.h"
#include "OloEngine/Core/Base.h"
#include <vector>

namespace OloEngine
{
    // Ring buffer implementation for command packets
    // Provides a fixed-size circular buffer for efficient memory reuse
    class CommandRingBuffer
    {
    public:
        CommandRingBuffer(u32 capacity = 1024);
        ~CommandRingBuffer();
        
        // Disallow copying
        CommandRingBuffer(const CommandRingBuffer&) = delete;
        CommandRingBuffer& operator=(const CommandRingBuffer&) = delete;
        
        // Allow moving
        CommandRingBuffer(CommandRingBuffer&& other) noexcept;
        CommandRingBuffer& operator=(CommandRingBuffer&& other) noexcept;

        // Add a new packet to the ring buffer
        CommandPacket* AllocatePacket();
        
        // Initialize a packet with command data and add it to the ring buffer
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
        
        // Execute all commands in the buffer
        void Execute(RendererAPI& api);
        
        // Reset the buffer for reuse
        void Reset();
        
        // Sort the packets in the buffer
        void Sort();
        
        // Try to batch compatible commands
        void BatchPackets();
        
        // Statistics
        u32 GetPacketCount() const { return m_PacketCount; }
        u32 GetCapacity() const { return m_Capacity; }
        bool IsFull() const { return m_PacketCount == m_Capacity; }
        
    private:
        // Ring buffer storage
        std::vector<CommandPacket> m_Packets;
        
        // Ring buffer indices
        u32 m_Head = 0;  // Index of the first packet
        u32 m_Tail = 0;  // Index where the next packet will be added
        u32 m_PacketCount = 0;
        u32 m_Capacity = 0;
        
        // Helper method to get the next index in the ring buffer
        u32 NextIndex(u32 index) const { return (index + 1) % m_Capacity; }
    };
}