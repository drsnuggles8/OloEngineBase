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
        OLO_PROFILE_FUNCTION();

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
        // Use the packed DrawKey for efficient sorting
        return m_Metadata.m_SortKey < other.m_Metadata.m_SortKey;
    }

    bool CommandPacket::CanBatchWith(const CommandPacket& other) const
    {
        // Different command types can't be batched
        if (m_CommandType != other.m_CommandType)
            return false;
        // Commands that depend on previous commands can't be batched
        if (m_Metadata.m_DependsOnPrevious || other.m_Metadata.m_DependsOnPrevious)
            return false;

        // Commands with different group IDs can't be batched
        if (m_Metadata.m_GroupID != other.m_Metadata.m_GroupID && m_Metadata.m_GroupID != 0 && other.m_Metadata.m_GroupID != 0)
            return false;

        // Specific batching logic based on command type
        switch (m_CommandType)
        {
            case CommandType::DrawMesh:
            {
                auto* cmd1 = reinterpret_cast<const DrawMeshCommand*>(m_CommandData);
                auto* cmd2 = reinterpret_cast<const DrawMeshCommand*>(other.m_CommandData);

                // Check if mesh pointers are the same
                if (cmd1->mesh != cmd2->mesh)
                    return false;

                // Check if shaders are the same
                if (cmd1->shader != cmd2->shader)
                    return false;

                // Check material properties
                if (cmd1->useTextureMaps != cmd2->useTextureMaps)
                    return false;

                if (cmd1->diffuseMap != cmd2->diffuseMap)
                    return false;

                if (cmd1->specularMap != cmd2->specularMap)
                    return false;

                // Check material properties
                if (cmd1->ambient != cmd2->ambient)
                    return false;

                if (cmd1->diffuse != cmd2->diffuse)
                    return false;

                if (cmd1->specular != cmd2->specular)
                    return false;

                if (cmd1->shininess != cmd2->shininess)
                    return false;

                // All checks passed, these commands can be batched
                return true;
            }

            case CommandType::DrawQuad:
            {
                auto* cmd1 = reinterpret_cast<const DrawQuadCommand*>(m_CommandData);
                auto* cmd2 = reinterpret_cast<const DrawQuadCommand*>(other.m_CommandData);

                // Quads can be batched if they use the same texture and shader
                return cmd1->texture == cmd2->texture &&
                       cmd1->shader == cmd2->shader;
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

} // namespace OloEngine
