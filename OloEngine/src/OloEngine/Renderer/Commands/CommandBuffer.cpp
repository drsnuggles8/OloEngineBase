#include "OloEnginePCH.h"
#include "CommandBuffer.h"
#include <algorithm>
#include <vector>

namespace OloEngine
{
    CommandBuffer::CommandBuffer(sizet initialSizeBytes)
        : m_Capacity(initialSizeBytes), m_Size(0), m_CommandCount(0)
    {
        OLO_CORE_ASSERT(initialSizeBytes > 0, "Command buffer size must be greater than zero!");
        m_Data = new u8[initialSizeBytes];
        std::memset(m_Data, 0, initialSizeBytes);
    }

    CommandBuffer::~CommandBuffer()
    {
        delete[] m_Data;
        m_Data = nullptr;
        m_Size = 0;
        m_Capacity = 0;
        m_CommandCount = 0;
    }

    CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
        : m_Data(other.m_Data), m_Size(other.m_Size), m_Capacity(other.m_Capacity), m_CommandCount(other.m_CommandCount)
    {
        other.m_Data = nullptr;
        other.m_Size = 0;
        other.m_Capacity = 0;
        other.m_CommandCount = 0;
    }

    CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept
    {
        if (this != &other)
        {
            delete[] m_Data;

            m_Data = other.m_Data;
            m_Size = other.m_Size;
            m_Capacity = other.m_Capacity;
            m_CommandCount = other.m_CommandCount;

            other.m_Data = nullptr;
            other.m_Size = 0;
            other.m_Capacity = 0;
            other.m_CommandCount = 0;
        }
        return *this;
    }

    void CommandBuffer::Reset()
    {
        // Reset size and command count, but keep allocated memory
        m_Size = 0;
        m_CommandCount = 0;
    }

    void CommandBuffer::Clear()
    {
        // Similar to Reset, but clear memory to zero
        std::memset(m_Data, 0, m_Size);
        m_Size = 0;
        m_CommandCount = 0;
    }

    void CommandBuffer::Execute(RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();

        // Execute all commands in sequence
        u8* ptr = m_Data;
        u8* end = m_Data + m_Size;

        while (ptr < end)
        {
            CommandHeader* header = reinterpret_cast<CommandHeader*>(ptr);
            OLO_CORE_ASSERT(header->dispatchFn, "Command dispatch function is null!");
            
            // Execute the command
            header->dispatchFn(ptr, api);

            // Move to next command (using 16-byte alignment)
            // We need to figure out the command size based on type
            sizet commandSize;
            
            switch (header->type)
            {
                case CommandType::SetViewport:              commandSize = sizeof(SetViewportCommand); break;
                case CommandType::SetClearColor:            commandSize = sizeof(SetClearColorCommand); break;
                case CommandType::Clear:                    commandSize = sizeof(ClearCommand); break;
                case CommandType::ClearStencil:             commandSize = sizeof(ClearStencilCommand); break;
                case CommandType::DrawIndexed:              commandSize = sizeof(DrawIndexedCommand); break;
                case CommandType::DrawIndexedInstanced:     commandSize = sizeof(DrawIndexedInstancedCommand); break;
                case CommandType::DrawArrays:               commandSize = sizeof(DrawArraysCommand); break;
                case CommandType::DrawLines:                commandSize = sizeof(DrawLinesCommand); break;
                case CommandType::BindDefaultFramebuffer:   commandSize = sizeof(BindDefaultFramebufferCommand); break;
                case CommandType::BindTexture:              commandSize = sizeof(BindTextureCommand); break;
                case CommandType::SetBlendState:            commandSize = sizeof(SetBlendStateCommand); break;
                case CommandType::SetBlendFunc:             commandSize = sizeof(SetBlendFuncCommand); break;
                case CommandType::SetBlendEquation:         commandSize = sizeof(SetBlendEquationCommand); break;
                case CommandType::SetDepthTest:             commandSize = sizeof(SetDepthTestCommand); break;
                case CommandType::SetDepthMask:             commandSize = sizeof(SetDepthMaskCommand); break;
                case CommandType::SetDepthFunc:             commandSize = sizeof(SetDepthFuncCommand); break;
                case CommandType::SetStencilTest:           commandSize = sizeof(SetStencilTestCommand); break;
                case CommandType::SetStencilFunc:           commandSize = sizeof(SetStencilFuncCommand); break;
                case CommandType::SetStencilMask:           commandSize = sizeof(SetStencilMaskCommand); break;
                case CommandType::SetStencilOp:             commandSize = sizeof(SetStencilOpCommand); break;
                case CommandType::SetCulling:               commandSize = sizeof(SetCullingCommand); break;
                case CommandType::SetCullFace:              commandSize = sizeof(SetCullFaceCommand); break;
                case CommandType::SetLineWidth:             commandSize = sizeof(SetLineWidthCommand); break;
                case CommandType::SetPolygonMode:           commandSize = sizeof(SetPolygonModeCommand); break;
                case CommandType::SetPolygonOffset:         commandSize = sizeof(SetPolygonOffsetCommand); break;
                case CommandType::SetScissorTest:           commandSize = sizeof(SetScissorTestCommand); break;
                case CommandType::SetScissorBox:            commandSize = sizeof(SetScissorBoxCommand); break;
                case CommandType::SetColorMask:             commandSize = sizeof(SetColorMaskCommand); break;
                case CommandType::SetMultisampling:         commandSize = sizeof(SetMultisamplingCommand); break;
                case CommandType::DrawMesh:                 commandSize = sizeof(DrawMeshCommand); break;
                case CommandType::DrawMeshInstanced:        commandSize = sizeof(DrawMeshInstancedCommand); break;
                case CommandType::DrawQuad:                 commandSize = sizeof(DrawQuadCommand); break;
                default:
                    OLO_CORE_ASSERT(false, "Unknown command type!");
                    commandSize = 0;
                    break;
            }
            
            // Account for alignment
            sizet alignedSize = (commandSize + 15) & ~15;
            ptr += alignedSize;
        }
    }

    void CommandBuffer::Sort()
    {
        // This is just a stub for now - we'll implement sorting in a later step
        // Sorting requires more work with key generation and command reorganization
        OLO_CORE_WARN("CommandBuffer::Sort() is not implemented yet!");
    }

    void CommandBuffer::Grow(sizet additionalBytes)
    {
        OLO_PROFILE_FUNCTION();

        // Calculate new capacity (double the current size, or add the requested size if larger)
        sizet newCapacity = std::max(m_Capacity * 2, m_Capacity + additionalBytes);
        
        // Allocate new buffer
        u8* newData = new u8[newCapacity];
        std::memset(newData, 0, newCapacity);
        
        // Copy old data to new buffer
        if (m_Size > 0)
        {
            std::memcpy(newData, m_Data, m_Size);
        }
        
        // Delete old buffer
        delete[] m_Data;
        
        // Update buffer pointers and capacity
        m_Data = newData;
        m_Capacity = newCapacity;
    }
}