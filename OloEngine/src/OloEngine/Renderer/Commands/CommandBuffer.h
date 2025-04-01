#pragma once

#include "RenderCommand.h"
#include "CommandDispatch.h"
#include "OloEngine/Core/Base.h"
#include <vector>

namespace OloEngine
{
    // Defines the key used for sorting commands in the buffer
    struct CommandKey
    {
        u64 primary = 0;   // Primary sorting key (e.g., shader ID)
        u64 secondary = 0; // Secondary sorting key (e.g., material)
        u64 tertiary = 0;  // Third-level sorting key (e.g., texture)
        u8 order = 0;      // Preservation of order for commands that must execute in sequence
        
        // Compare operator for sorting
        bool operator<(const CommandKey& other) const
        {
            if (primary != other.primary) return primary < other.primary;
            if (secondary != other.secondary) return secondary < other.secondary;
            if (tertiary != other.tertiary) return tertiary < other.tertiary;
            return order < other.order;
        }
    };

    // Command buffer for recording and executing render commands
    class CommandBuffer
    {
    public:
        CommandBuffer(sizet initialSizeBytes = 1024 * 10); // Default 10KB initial size
        ~CommandBuffer();

        // Disallow copying
        CommandBuffer(const CommandBuffer&) = delete;
        CommandBuffer& operator=(const CommandBuffer&) = delete;

        // Allow moving
        CommandBuffer(CommandBuffer&& other) noexcept;
        CommandBuffer& operator=(CommandBuffer&& other) noexcept;

        // Command creation functions - returns pointers to command data in buffer
        template<typename T>
        T* CreateCommand()
        {
            static_assert(sizeof(T) <= MAX_COMMAND_SIZE, "Command exceeds maximum size");

            // Calculate aligned size to ensure proper padding
            const sizet alignedSize = (sizeof(T) + 15) & ~15;
            
            // Ensure buffer has enough space
            if (m_Size + alignedSize > m_Capacity)
            {
                Grow(alignedSize);
            }

            // Get pointer to command memory in buffer
            T* command = reinterpret_cast<T*>(m_Data + m_Size);
            
            // Initialize command with default values
            new (command) T{};
            
            // Set the command header
            command->header.type = GetCommandType<T>();
            command->header.dispatchFn = CommandDispatch::GetDispatchFunction(command->header.type);
            
            // Update buffer size
            m_Size += alignedSize;
            m_CommandCount++;

            return command;
        }

        // Command buffer operations
        void Reset();
        void Clear();
        void Execute(RendererAPI& api);
        void Sort();

        // Statistics
        u32 GetCommandCount() const { return m_CommandCount; }
        sizet GetSize() const { return m_Size; }
        sizet GetCapacity() const { return m_Capacity; }

    private:
        // Helper to map command type to enum
        template<typename T> static constexpr CommandType GetCommandType();
        
        // Grow the buffer to accommodate more commands
        void Grow(sizet additionalBytes);

        // Buffer memory management
        u8* m_Data = nullptr;
        sizet m_Size = 0;
        sizet m_Capacity = 0;
        u32 m_CommandCount = 0;
    };

    // Template specializations for command types
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetViewportCommand>() { return CommandType::SetViewport; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetClearColorCommand>() { return CommandType::SetClearColor; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<ClearCommand>() { return CommandType::Clear; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<ClearStencilCommand>() { return CommandType::ClearStencil; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<DrawIndexedCommand>() { return CommandType::DrawIndexed; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<DrawIndexedInstancedCommand>() { return CommandType::DrawIndexedInstanced; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<DrawArraysCommand>() { return CommandType::DrawArrays; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<DrawLinesCommand>() { return CommandType::DrawLines; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<BindDefaultFramebufferCommand>() { return CommandType::BindDefaultFramebuffer; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<BindTextureCommand>() { return CommandType::BindTexture; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetBlendStateCommand>() { return CommandType::SetBlendState; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetBlendFuncCommand>() { return CommandType::SetBlendFunc; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetBlendEquationCommand>() { return CommandType::SetBlendEquation; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetDepthTestCommand>() { return CommandType::SetDepthTest; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetDepthMaskCommand>() { return CommandType::SetDepthMask; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetDepthFuncCommand>() { return CommandType::SetDepthFunc; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetStencilTestCommand>() { return CommandType::SetStencilTest; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetStencilFuncCommand>() { return CommandType::SetStencilFunc; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetStencilMaskCommand>() { return CommandType::SetStencilMask; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetStencilOpCommand>() { return CommandType::SetStencilOp; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetCullingCommand>() { return CommandType::SetCulling; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetCullFaceCommand>() { return CommandType::SetCullFace; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetLineWidthCommand>() { return CommandType::SetLineWidth; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetPolygonModeCommand>() { return CommandType::SetPolygonMode; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetPolygonOffsetCommand>() { return CommandType::SetPolygonOffset; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetScissorTestCommand>() { return CommandType::SetScissorTest; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetScissorBoxCommand>() { return CommandType::SetScissorBox; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetColorMaskCommand>() { return CommandType::SetColorMask; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<SetMultisamplingCommand>() { return CommandType::SetMultisampling; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<DrawMeshCommand>() { return CommandType::DrawMesh; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<DrawMeshInstancedCommand>() { return CommandType::DrawMeshInstanced; }
    template<> constexpr CommandType CommandBuffer::GetCommandType<DrawQuadCommand>() { return CommandType::DrawQuad; }
}