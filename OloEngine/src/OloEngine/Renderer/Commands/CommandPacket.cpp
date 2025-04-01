#include "OloEnginePCH.h"
#include "CommandPacket.h"
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

    void CommandPacket::Execute(RendererAPI& api) const
    {
        if (m_DispatchFn && m_CommandSize > 0)
        {
            m_DispatchFn(m_CommandData, api);
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
}