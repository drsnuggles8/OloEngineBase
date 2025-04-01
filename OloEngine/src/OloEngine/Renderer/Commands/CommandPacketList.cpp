#include "OloEnginePCH.h"
#include "CommandPacketList.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include <algorithm>

namespace OloEngine
{
    // Static pool initialization
    std::vector<CommandPacket*> CommandPacketList::s_PacketPool;

    CommandPacketList::~CommandPacketList()
    {
        Clear();
    }

    CommandPacketList::CommandPacketList(CommandPacketList&& other) noexcept
        : m_Head(other.m_Head), m_Tail(other.m_Tail), m_PacketCount(other.m_PacketCount)
    {
        other.m_Head = nullptr;
        other.m_Tail = nullptr;
        other.m_PacketCount = 0;
    }

    CommandPacketList& CommandPacketList::operator=(CommandPacketList&& other) noexcept
    {
        if (this != &other)
        {
            // Clean up existing packets
            Clear();
            
            // Transfer ownership
            m_Head = other.m_Head;
            m_Tail = other.m_Tail;
            m_PacketCount = other.m_PacketCount;
            
            other.m_Head = nullptr;
            other.m_Tail = nullptr;
            other.m_PacketCount = 0;
        }
        return *this;
    }

    CommandPacket* CommandPacketList::AllocatePacket()
    {
        OLO_PROFILE_FUNCTION();
        
        CommandPacket* packet = nullptr;
        
        // Try to get a packet from the pool first
        if (!s_PacketPool.empty())
        {
            packet = s_PacketPool.back();
            s_PacketPool.pop_back();
        }
        else
        {
            // Pool is empty, allocate a new packet
            packet = AllocateNewPacket();
        }
        
        if (packet)
        {
            // Add the packet to the linked list
            if (!m_Head)
            {
                // First packet in the list
                m_Head = packet;
                m_Tail = packet;
            }
            else
            {
                // Add to the end of the list
                m_Tail->SetNext(packet);
                m_Tail = packet;
            }
            
            packet->SetNext(nullptr);
            m_PacketCount++;
        }
        
        return packet;
    }

    void CommandPacketList::Execute(RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        
        CommandPacket* current = m_Head;
        while (current)
        {
            current->Execute(api);
            current = current->GetNext();
        }
    }

    void CommandPacketList::Clear()
    {
        OLO_PROFILE_FUNCTION();
        
        // Return all packets to the pool
        CommandPacket* current = m_Head;
        while (current)
        {
            CommandPacket* next = current->GetNext();
            s_PacketPool.push_back(current);
            current = next;
        }
        
        m_Head = nullptr;
        m_Tail = nullptr;
        m_PacketCount = 0;
    }

    CommandPacket* CommandPacketList::AllocateNewPacket()
    {
        // If the pool is empty, grow it
        if (s_PacketPool.empty())
        {
            // Determine how many packets to add
            const u32 growSize = s_PacketPool.size() == 0 ? INITIAL_POOL_SIZE : GROWTH_SIZE;
            
            // Create new packets and add them to the pool
            for (u32 i = 0; i < growSize; ++i)
            {
                s_PacketPool.push_back(new CommandPacket());
            }
            
            OLO_CORE_INFO("CommandPacketList: Added {0} new packets to pool (total: {1})", 
                          growSize, s_PacketPool.size());
        }
        
        // Now the pool should have packets
        if (!s_PacketPool.empty())
        {
            CommandPacket* packet = s_PacketPool.back();
            s_PacketPool.pop_back();
            return packet;
        }
        
        OLO_CORE_ERROR("CommandPacketList: Failed to allocate packet!");
        return nullptr;
    }

    void CommandPacketList::Sort()
    {
        OLO_PROFILE_FUNCTION();
        
        // If we have 0 or 1 packets, no need to sort
        if (!m_Head || !m_Head->GetNext())
            return;
            
        // Convert linked list to vector for easier sorting
        std::vector<CommandPacket*> packets;
        packets.reserve(m_PacketCount);
        
        CommandPacket* current = m_Head;
        while (current)
        {
            CommandPacket* next = current->GetNext();
            current->SetNext(nullptr);
            packets.push_back(current);
            current = next;
        }
        
        // Sort the packets based on their keys
        std::stable_sort(packets.begin(), packets.end(), 
            [](const CommandPacket* a, const CommandPacket* b) {
                return *a < *b;
            });
            
        // Rebuild the linked list
        m_Head = packets[0];
        current = m_Head;
        
        for (size_t i = 1; i < packets.size(); ++i)
        {
            current->SetNext(packets[i]);
            current = packets[i];
        }
        
        m_Tail = current;
        m_Tail->SetNext(nullptr);
    }

    void CommandPacketList::BatchPackets()
    {
        OLO_PROFILE_FUNCTION();
        
        // If we have 0 or 1 packets, no need to batch
        if (!m_Head || !m_Head->GetNext())
            return;
            
        // First, sort the packets to bring similar ones together
        Sort();
        
        // Now iterate through the sorted list and look for batching opportunities
        CommandPacket* current = m_Head;
        std::vector<CommandPacket*> batchedList;
        batchedList.reserve(m_PacketCount);
        
        while (current)
        {
            CommandPacket* next = current->GetNext();
            CommandPacket* batchStart = current;
            u32 batchCount = 1;
            
            // Look for consecutive packets that can be batched
            while (next && batchStart->CanBatchWith(*next))
            {
                batchCount++;
                CommandPacket* temp = next;
                next = next->GetNext();
                
                // Return the batched packet to the pool
                s_PacketPool.push_back(temp);
            }
            
            // Add the batch start packet to our new list
            batchedList.push_back(batchStart);
            current = next;
        }
        
        // Rebuild the linked list from the batched list
        if (!batchedList.empty())
        {
            m_Head = batchedList[0];
            current = m_Head;
            
            for (size_t i = 1; i < batchedList.size(); ++i)
            {
                current->SetNext(batchedList[i]);
                current = batchedList[i];
            }
            
            m_Tail = current;
            m_Tail->SetNext(nullptr);
            m_PacketCount = static_cast<u32>(batchedList.size());
        }
        else
        {
            m_Head = nullptr;
            m_Tail = nullptr;
            m_PacketCount = 0;
        }
    }
}