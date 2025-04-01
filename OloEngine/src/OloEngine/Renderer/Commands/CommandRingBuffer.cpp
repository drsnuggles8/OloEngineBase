#include "OloEnginePCH.h"
#include "CommandRingBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include <algorithm>

namespace OloEngine
{
    CommandRingBuffer::CommandRingBuffer(u32 capacity)
        : m_Capacity(capacity)
    {
        OLO_CORE_ASSERT(capacity > 0, "Ring buffer capacity must be greater than zero!");
        
        // Pre-allocate the packet storage
        m_Packets.resize(capacity);
    }

    CommandRingBuffer::~CommandRingBuffer() = default;

    CommandRingBuffer::CommandRingBuffer(CommandRingBuffer&& other) noexcept
        : m_Packets(std::move(other.m_Packets)),
          m_Head(other.m_Head),
          m_Tail(other.m_Tail),
          m_PacketCount(other.m_PacketCount),
          m_Capacity(other.m_Capacity)
    {
        other.m_Head = 0;
        other.m_Tail = 0;
        other.m_PacketCount = 0;
        other.m_Capacity = 0;
    }

    CommandRingBuffer& CommandRingBuffer::operator=(CommandRingBuffer&& other) noexcept
    {
        if (this != &other)
        {
            m_Packets = std::move(other.m_Packets);
            m_Head = other.m_Head;
            m_Tail = other.m_Tail;
            m_PacketCount = other.m_PacketCount;
            m_Capacity = other.m_Capacity;
            
            other.m_Head = 0;
            other.m_Tail = 0;
            other.m_PacketCount = 0;
            other.m_Capacity = 0;
        }
        return *this;
    }

    CommandPacket* CommandRingBuffer::AllocatePacket()
    {
        OLO_PROFILE_FUNCTION();
        
        // Check if the buffer is full
        if (m_PacketCount == m_Capacity)
        {
            OLO_CORE_WARN("CommandRingBuffer: Buffer is full, cannot allocate more packets!");
            return nullptr;
        }
        
        // Get the next available slot
        CommandPacket* packet = &m_Packets[m_Tail];
        
        // Update the tail index
        m_Tail = NextIndex(m_Tail);
        m_PacketCount++;
        
        return packet;
    }

    void CommandRingBuffer::Execute(RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_PacketCount == 0)
            return;
            
        // Execute all packets in sequence
        u32 index = m_Head;
        for (u32 i = 0; i < m_PacketCount; ++i)
        {
            m_Packets[index].Execute(api);
            index = NextIndex(index);
        }
    }

    void CommandRingBuffer::Reset()
    {
        // Just reset the indices, no need to clear the actual packet data
        m_Head = 0;
        m_Tail = 0;
        m_PacketCount = 0;
    }

    void CommandRingBuffer::Sort()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_PacketCount <= 1)
            return;
            
        // Copy packets to a temporary vector for sorting
        std::vector<CommandPacket> sortedPackets;
        sortedPackets.reserve(m_PacketCount);
        
        // Copy the packets from the ring buffer to the vector
        u32 index = m_Head;
        for (u32 i = 0; i < m_PacketCount; ++i)
        {
            sortedPackets.push_back(std::move(m_Packets[index]));
            index = NextIndex(index);
        }
        
        // Sort the packets
        std::stable_sort(sortedPackets.begin(), sortedPackets.end(),
            [](const CommandPacket& a, const CommandPacket& b) {
                return a < b;
            });
            
        // Copy the sorted packets back to the ring buffer
        for (u32 i = 0; i < m_PacketCount; ++i)
        {
            m_Packets[i] = std::move(sortedPackets[i]);
        }
        
        // Reset the head and tail indices
        m_Head = 0;
        m_Tail = m_PacketCount % m_Capacity;
    }

    void CommandRingBuffer::BatchPackets()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_PacketCount <= 1)
            return;
            
        // First sort to bring similar packets together
        Sort();
        
        // Now try to batch compatible packets
        std::vector<CommandPacket> batchedPackets;
        batchedPackets.reserve(m_PacketCount);
        
        u32 index = m_Head;
        for (u32 i = 0; i < m_PacketCount; ++i)
        {
            CommandPacket& current = m_Packets[index];
            
            // Check if we can batch with the previous packet
            if (!batchedPackets.empty() && batchedPackets.back().CanBatchWith(current))
            {
                // Packets could be batched in a more sophisticated way,
                // but for now we'll just keep the first one
                // (actual batching would be implemented in a real engine)
            }
            else
            {
                // Can't batch, add as a new packet
                batchedPackets.push_back(std::move(current));
            }
            
            index = NextIndex(index);
        }
        
        // Copy the batched packets back to the ring buffer
        u32 newCount = static_cast<u32>(batchedPackets.size());
        for (u32 i = 0; i < newCount; ++i)
        {
            m_Packets[i] = std::move(batchedPackets[i]);
        }
        
        // Reset indices
        m_Head = 0;
        m_Tail = newCount % m_Capacity;
        m_PacketCount = newCount;
    }
}