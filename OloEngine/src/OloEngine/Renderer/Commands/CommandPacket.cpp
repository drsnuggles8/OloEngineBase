#include "OloEnginePCH.h"
#include "CommandPacket.h"
#include "CommandAllocator.h"
#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine
{
    // Static dispatch resolver — set at engine startup by CommandDispatch::Initialize().
    CommandPacket::DispatchResolverFn CommandPacket::s_DispatchResolver = nullptr;

    void CommandPacket::SetDispatchResolver(DispatchResolverFn resolver)
    {
        s_DispatchResolver = resolver;
    }

    CommandPacket::~CommandPacket()
    {
        // No dynamic resources to clean up
    }

    void CommandPacket::Execute(RendererAPI& rendererAPI) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_CommandSize > 0)
        {
            // Resolve dispatch function: prefer per-packet fn, fallback to
            // global resolver (set at engine startup by CommandDispatch::Initialize).
            CommandDispatchFn dispatchFn = m_DispatchFn;
            if (!dispatchFn && s_DispatchResolver)
            {
                dispatchFn = s_DispatchResolver(m_CommandType);
            }

            if (dispatchFn)
            {
                dispatchFn(GetInlineData(), rendererAPI);
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
                auto* cmd1 = reinterpret_cast<const DrawMeshCommand*>(GetInlineData());
                auto* cmd2 = reinterpret_cast<const DrawMeshCommand*>(other.GetInlineData());

                // Check if mesh handles are the same (POD)
                if (cmd1->meshHandle != cmd2->meshHandle)
                    return false;

                // Check if material data is the same (covers shader, textures, and all material properties)
                if (cmd1->materialDataIndex != cmd2->materialDataIndex)
                    return false;

                // Check if render state is the same (blend, depth, stencil, etc.)
                if (cmd1->renderStateIndex != cmd2->renderStateIndex)
                    return false;

                // All checks passed, these commands can be batched
                return true;
            }

            case CommandType::DrawQuad:
            {
                auto* cmd1 = reinterpret_cast<const DrawQuadCommand*>(GetInlineData());
                auto* cmd2 = reinterpret_cast<const DrawQuadCommand*>(other.GetInlineData());

                // Quads can be batched if they use the same texture, shader, and render state
                return cmd1->textureID == cmd2->textureID &&
                       cmd1->shaderRendererID == cmd2->shaderRendererID &&
                       cmd1->renderStateIndex == cmd2->renderStateIndex;
            }

            // State change commands generally can't be batched
            default:
                return false;
        }
    }

    CommandPacket* CommandPacket::Clone(CommandAllocator& allocator) const
    {
        OLO_PROFILE_FUNCTION();

        // Allocate memory for packet + command data together
        void* block = allocator.AllocateCommandMemory(sizeof(CommandPacket) + m_CommandSize);
        if (!block)
        {
            OLO_CORE_ERROR("CommandPacket: Failed to allocate memory for clone");
            return nullptr;
        }

        // Construct a new CommandPacket in the allocated memory
        auto* clone = new (block) CommandPacket();

        // Copy command data into clone's inline storage
        clone->m_CommandSize = m_CommandSize;
        std::memcpy(clone->GetInlineData(), GetInlineData(), m_CommandSize);

        // Copy metadata
        clone->m_CommandType = m_CommandType;
        clone->m_DispatchFn = m_DispatchFn;
        clone->m_Metadata = m_Metadata;

        return clone;
    }

} // namespace OloEngine
