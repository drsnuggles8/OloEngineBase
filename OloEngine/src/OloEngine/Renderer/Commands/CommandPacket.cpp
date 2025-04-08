#include "OloEnginePCH.h"
#include "CommandPacket.h"
#include "CommandAllocator.h"
#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine
{
    CommandPacket::~CommandPacket()
    {
        // No dynamic resources to clean up
    }

    CommandPacket::CommandPacket(CommandPacket&& other) noexcept
        : m_CommandSize(other.m_CommandSize), 
          m_CommandType(other.m_CommandType),
          m_DispatchFn(other.m_DispatchFn),
          m_Metadata(other.m_Metadata),
          m_Next(other.m_Next)
    {
        // Copy command data
        std::memcpy(m_CommandData, other.m_CommandData, other.m_CommandSize);
        
        // Reset the source object
        other.m_CommandSize = 0;
        other.m_CommandType = CommandType::Invalid;
        other.m_DispatchFn = nullptr;
        other.m_Next = nullptr;
    }

    CommandPacket& CommandPacket::operator=(CommandPacket&& other) noexcept
    {
        if (this != &other)
        {
            // Copy command data
            std::memcpy(m_CommandData, other.m_CommandData, other.m_CommandSize);
            
            // Copy metadata
            m_CommandSize = other.m_CommandSize;
            m_CommandType = other.m_CommandType;
            m_DispatchFn = other.m_DispatchFn;
            m_Metadata = other.m_Metadata;
            m_Next = other.m_Next;
            
            // Reset the source object
            other.m_CommandSize = 0;
            other.m_CommandType = CommandType::Invalid;
            other.m_DispatchFn = nullptr;
            other.m_Next = nullptr;
        }
        return *this;
    }  
	
	void CommandPacket::Execute(RendererAPI& rendererAPI) const
    {
        if (m_CommandSize > 0)
        {
            if (m_DispatchFn)
            {
                m_DispatchFn(m_CommandData, rendererAPI);
            }
            else
            {
                OLO_CORE_ERROR("CommandPacket::Execute: No dispatch function for command type {}", 
                    static_cast<int>(m_CommandType));
            }
        }
    }

    bool CommandPacket::operator<(const CommandPacket& other) const
    {
        // First compare shader keys
        if (m_Metadata.shaderKey != other.m_Metadata.shaderKey)
            return m_Metadata.shaderKey < other.m_Metadata.shaderKey;
            
        // Then compare material keys
        if (m_Metadata.materialKey != other.m_Metadata.materialKey)
            return m_Metadata.materialKey < other.m_Metadata.materialKey;
            
        // Then compare texture keys
        if (m_Metadata.textureKey != other.m_Metadata.textureKey)
            return m_Metadata.textureKey < other.m_Metadata.textureKey;
            
        // Then compare state change keys
        if (m_Metadata.stateChangeKey != other.m_Metadata.stateChangeKey)
            return m_Metadata.stateChangeKey < other.m_Metadata.stateChangeKey;
            
        // Finally, maintain original execution order
        return m_Metadata.executionOrder < other.m_Metadata.executionOrder;
    }

    bool CommandPacket::CanBatchWith(const CommandPacket& other) const
    {
        // Different command types can't be batched
        if (m_CommandType != other.m_CommandType)
            return false;
            
        // Commands that depend on previous commands can't be batched
        if (m_Metadata.dependsOnPrevious || other.m_Metadata.dependsOnPrevious)
            return false;
            
        // Commands with different group IDs can't be batched
        if (m_Metadata.groupId != other.m_Metadata.groupId && m_Metadata.groupId != 0 && other.m_Metadata.groupId != 0)
            return false;
            
        // Specific batching logic based on command type
        switch (m_CommandType)
        {
            case CommandType::DrawMesh:
            {
                auto* cmd1 = reinterpret_cast<const DrawMeshCommand*>(m_CommandData);
                auto* cmd2 = reinterpret_cast<const DrawMeshCommand*>(other.m_CommandData);
                
                // Meshes can be batched if they use the same mesh, material and shader
                return cmd1->meshRendererID == cmd2->meshRendererID &&
                       cmd1->shaderID == cmd2->shaderID &&
                       cmd1->useTextureMaps == cmd2->useTextureMaps &&
                       cmd1->diffuseMapID == cmd2->diffuseMapID &&
                       cmd1->specularMapID == cmd2->specularMapID &&
                       cmd1->ambient == cmd2->ambient &&
                       cmd1->diffuse == cmd2->diffuse &&
                       cmd1->specular == cmd2->specular &&
                       cmd1->shininess == cmd2->shininess;
            }
            
            case CommandType::DrawQuad:
            {
                auto* cmd1 = reinterpret_cast<const DrawQuadCommand*>(m_CommandData);
                auto* cmd2 = reinterpret_cast<const DrawQuadCommand*>(other.m_CommandData);
                
                // Quads can be batched if they use the same texture and shader
                return cmd1->textureID == cmd2->textureID &&
                       cmd1->shaderID == cmd2->shaderID;
            }
            
            // State change commands generally can't be batched
            default:
                return false;
        }
    }

	bool CommandPacket::UpdateCommandData(const void* data, sizet size)
	{
		if (size > MAX_COMMAND_SIZE || !data)
		{
			OLO_CORE_ERROR("CommandPacket: Cannot update command data, invalid size or null data");
			return false;
		}
		
		std::memcpy(m_CommandData, data, size);
		m_CommandSize = size;
		return true;
	}

	CommandPacket* CommandPacket::Clone(CommandAllocator& allocator) const
	{
		OLO_PROFILE_FUNCTION();

		// Allocate memory for a new command packet
		void* packetMemory = allocator.AllocateCommandMemory(sizeof(CommandPacket));
		if (!packetMemory)
		{
			OLO_CORE_ERROR("CommandPacket: Failed to allocate memory for clone");
			return nullptr;
		}

		// Construct a new CommandPacket in the allocated memory
		auto* clone = new (packetMemory) CommandPacket();

		// Copy command data
		std::memcpy(clone->m_CommandData, m_CommandData, m_CommandSize);

		// Copy metadata
		clone->m_CommandSize = m_CommandSize;
		clone->m_CommandType = m_CommandType;
		clone->m_DispatchFn = m_DispatchFn;
		clone->m_Metadata = m_Metadata;
		clone->m_Next = nullptr; // The clone doesn't inherit the linked list position

		return clone;
	}

}
