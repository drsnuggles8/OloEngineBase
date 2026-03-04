#pragma once

#include "RenderCommand.h"
#include "DrawKey.h"
#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

/*
 * TODO: CommandPacket Asset Management Integration
 *
 * The Initialize() method has been updated to work with ID-based commands,
 * but needs further updates once asset management system is complete:
 *
 * - Sort key generation should use proper asset handles
 * - Material ID generation should be based on asset system, not texture IDs
 * - Consider caching resolved asset pointers for performance
 */

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;
    class CommandAllocator;
    class RenderContext;

    // Command packet metadata
    struct PacketMetadata
    {
        // Primary sorting key - packed bitfield for maximum performance
        DrawKey m_SortKey;

        // Execution properties
        bool m_DependsOnPrevious = false; // This command must execute after the previous one
        u32 m_GroupID = 0;                // Commands with the same groupId should be kept together
        u32 m_ExecutionOrder = 0;         // Sequence number for preserving order when needed

        // Statistics/debugging
        bool m_IsStatic = false;           // Command doesn't change between frames
        const char* m_DebugName = nullptr; // Optional name for debugging
    };

    // Command packet that wraps a command with metadata and links to other packets
    class CommandPacket
    {
      public:
        CommandPacket() = default;
        ~CommandPacket();

        // Non-copyable, non-movable — packets live in bump-allocated memory
        // with inline command data appended right after sizeof(CommandPacket).
        CommandPacket(const CommandPacket&) = delete;
        CommandPacket& operator=(const CommandPacket&) = delete;
        CommandPacket(CommandPacket&&) = delete;
        CommandPacket& operator=(CommandPacket&&) = delete;

        // Initialize a command packet with a specific command
        // WARNING: This uses memcpy which is only safe for trivially copyable types.
        // For non-trivial types (containing std::vector, smart pointers, etc.),
        // use CommandAllocator::AllocatePacketWithCommand() instead.
        template<typename T>
        void Initialize(const T& commandData, const PacketMetadata& metadata = {})
        {
            static_assert(sizeof(T) <= MAX_COMMAND_SIZE, "Command exceeds maximum size");
            static_assert(std::is_trivially_copyable_v<T>,
                          "Initialize() uses memcpy and requires trivially copyable types. "
                          "For non-trivial types like DrawMeshInstancedCommand, use "
                          "CommandAllocator::AllocatePacketWithCommand() instead.");

            // Copy the command data into inline storage right after the packet header
            std::memcpy(GetInlineData(), &commandData, sizeof(T));
            m_CommandSize = sizeof(T);
            m_CommandType = commandData.header.type;

            // NOTE: Dispatch function is resolved lazily in Execute() rather than
            // here, to avoid a compile-time dependency on CommandDispatch.cpp
            // (which transitively includes Application, Renderer3D, AssetManager,
            //  glad, etc. — heavy statics that crash test executables).

            // Set metadata
            m_Metadata = metadata; // Set default keys if not specified in metadata
            if (m_Metadata.m_SortKey.GetShaderID() == 0 && m_CommandType == CommandType::DrawMesh)
            {
                const auto* cmd = reinterpret_cast<const DrawMeshCommand*>(GetInlineData());
                if (cmd->shaderRendererID != 0)
                    m_Metadata.m_SortKey.SetShaderID(cmd->shaderRendererID);
            }

            if (m_Metadata.m_SortKey.GetMaterialID() == 0 && m_CommandType == CommandType::DrawMesh)
            {
                const auto* cmd = reinterpret_cast<const DrawMeshCommand*>(GetInlineData());
                if (cmd->useTextureMaps)
                {
                    // Use POD renderer IDs for material hashing
                    u64 diffuseID = static_cast<u64>(cmd->diffuseMapID);
                    u64 specularID = static_cast<u64>(cmd->specularMapID);

                    // Improved 64-bit mixing using FNV-like hash before folding to 32 bits
                    u64 hash = diffuseID;
                    hash ^= specularID + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                    // Fold to 16 bits to fit the DrawKey material field
                    u32 materialID = static_cast<u32>(hash ^ (hash >> 32)) & 0xFFFF;

                    m_Metadata.m_SortKey.SetMaterialID(materialID);
                }
            }
        }

        // Execute the command with a RendererAPI
        void Execute(RendererAPI& rendererAPI) const;

        // Packet comparison for sorting
        bool operator<(const CommandPacket& other) const;

        // Getters for command properties
        CommandType GetCommandType() const
        {
            return m_CommandType;
        }
        const PacketMetadata& GetMetadata() const
        {
            return m_Metadata;
        }

        // Returns true if this packet can be batched with another packet
        bool CanBatchWith(const CommandPacket& other) const;

        // Set command data size (inline data lives at this + sizeof(CommandPacket))
        void SetCommandSize(sizet size)
        {
            m_CommandSize = size;
        }

        template<typename T>
        T* GetCommandData()
        {
            return reinterpret_cast<T*>(GetInlineData());
        }

        template<typename T>
        const T* GetCommandData() const
        {
            return reinterpret_cast<const T*>(GetInlineData());
        }

        // Get raw command data as void pointer (for operations that don't need to know the type)
        const void* GetRawCommandData() const
        {
            return GetInlineData();
        }
        void* GetRawCommandData()
        {
            return GetInlineData();
        }

        // Return command size for memory management
        sizet GetCommandSize() const
        {
            return m_CommandSize;
        }

        // Clone this packet (for cases where we need to duplicate commands)
        CommandPacket* Clone(class CommandAllocator& allocator) const;

        // For batch merging - update or modify command data
        bool UpdateCommandData(const void* data, sizet size);

        // For debugging - get the command type as a string
        const char* GetCommandTypeString() const
        {
            return CommandTypeToString(m_CommandType);
        }

        // Setters for command properties when working with raw data
        void SetCommandType(CommandType type)
        {
            m_CommandType = type;
        }
        void SetDispatchFunction(CommandDispatchFn fn)
        {
            m_DispatchFn = fn;
        }
        void SetMetadata(const PacketMetadata& metadata)
        {
            m_Metadata = metadata;
        }

        // Runtime dispatch resolver — set once during engine init
        // (e.g. CommandDispatch::Initialize sets this to CommandDispatch::GetDispatchFunction).
        // Keeps CommandPacket.obj free of any link-time dependency on CommandDispatch.obj.
        using DispatchResolverFn = CommandDispatchFn (*)(CommandType);
        static void SetDispatchResolver(DispatchResolverFn resolver);

      private:
        // Inline command data lives in the allocation immediately after the packet header.
        // Eliminates pointer indirection — data is always at (u8*)this + sizeof(CommandPacket).
        void* GetInlineData()
        {
            return reinterpret_cast<u8*>(this) + sizeof(CommandPacket);
        }
        const void* GetInlineData() const
        {
            return reinterpret_cast<const u8*>(this) + sizeof(CommandPacket);
        }

        static DispatchResolverFn s_DispatchResolver;
        sizet m_CommandSize = 0;
        CommandType m_CommandType = CommandType::Invalid;
        CommandDispatchFn m_DispatchFn = nullptr;
        PacketMetadata m_Metadata;
    };
} // namespace OloEngine
