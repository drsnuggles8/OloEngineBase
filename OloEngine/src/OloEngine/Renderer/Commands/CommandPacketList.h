#pragma once

#include "CommandPacket.h"
#include "OloEngine/Core/Base.h"
#include <vector>

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;

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

		CommandPacket* CreateRawPacket(const void* commandData, sizet commandSize, const PacketMetadata& metadata = {})
		{
			CommandPacket* packet = AllocatePacket();
			if (packet)
			{
				// Copy the raw command data
				packet->UpdateCommandData(commandData, commandSize);
				
				// Need to re-initialize dispatch function and command type from header
				const CommandHeader* header = static_cast<const CommandHeader*>(commandData);
				packet->SetCommandType(header->type);
				packet->SetDispatchFunction(header->dispatchFn);
				
				// Set metadata
				packet->SetMetadata(metadata);
			}
			return packet;
		}
        
        // Execute all commands in the list using the provided RendererAPI
        void Execute(RendererAPI& rendererAPI);
        
        // Clear the list, returning all packets to the pool
        void Clear();
        
        // Sort the packets based on keys for optimal rendering
        void Sort();
        
        // Try to batch compatible packets together
        void BatchPackets();

		// Get the head of the linked list
		CommandPacket* GetHead() const { return m_Head; }

		// Get the tail of the linked list
		CommandPacket* GetTail() const { return m_Tail; }

		// Convert the packet list to a vector for parallel processing
		std::vector<CommandPacket*> ToVector();

		// Rebuild the linked list from a vector
		void FromVector(const std::vector<CommandPacket*>& packets);

		// Find a packet by command type
		CommandPacket* FindPacketByType(CommandType type);

		// Split the list into multiple lists based on a predicate
		// Returns a vector of lists, each containing packets that match the predicate
		template<typename Predicate>
		std::vector<CommandPacketList> Split(Predicate pred)
		{
			std::vector<CommandPacketList> result;
			
			CommandPacket* current = m_Head;
			CommandPacket* splitStart = m_Head;
			CommandPacketList currentList;
			
			while (current)
			{
				CommandPacket* next = current->GetNext();
				
				if (!pred(*current) || !next)
				{
					// End of a split or end of list
					CommandPacketList list;
					
					// Copy packets from splitStart to current
					CommandPacket* temp = splitStart;
					while (temp && temp != next)
					{
						// Create a raw command header to get the type
						const u8* commandData = static_cast<const u8*>(temp->GetRawCommandData());
						
						// Create a new packet with the raw command data
						list.CreateRawPacket(commandData, temp->GetCommandSize(), temp->GetMetadata());
						
						temp = temp->GetNext();
					}
					
					// Add the list to result if not empty
					if (list.GetPacketCount() > 0)
						result.push_back(std::move(list));
						
					// Start a new split
					splitStart = next;
				}
				
				current = next;
			}
			
			return result;
		}
        
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