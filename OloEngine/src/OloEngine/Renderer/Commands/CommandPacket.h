#pragma once

#include "RenderCommand.h"
#include "CommandDispatch.h"
#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;
    class CommandAllocator;
    class RenderContext;
    
    // Command packet metadata
    struct PacketMetadata
    {
        // Sorting keys
        u64 shaderKey = 0;          // Primary sorting key (shader program)
        u64 materialKey = 0;         // Secondary sorting key (material properties)
        u64 textureKey = 0;          // Tertiary sorting key (textures)
        u32 stateChangeKey = 0;      // State change type identifier
        
        // Execution properties
        bool dependsOnPrevious = false;   // This command must execute after the previous one
        u32 groupId = 0;                  // Commands with the same groupId should be kept together
        u32 executionOrder = 0;           // Sequence number for preserving order when needed
        
        // Statistics/debugging
        bool isStatic = false;            // Command doesn't change between frames
        const char* debugName = nullptr;  // Optional name for debugging
    };
    
    // Command packet that wraps a command with metadata and links to other packets
    class CommandPacket
    {
    public:
        CommandPacket() = default;
        ~CommandPacket();
        
        // Disallow copying
        CommandPacket(const CommandPacket&) = delete;
        CommandPacket& operator=(const CommandPacket&) = delete;
        
        // Allow moving
        CommandPacket(CommandPacket&& other) noexcept;
        CommandPacket& operator=(CommandPacket&& other) noexcept;
        
        // Initialize a command packet with a specific command
        template<typename T>
		void Initialize(const T& commandData, const PacketMetadata& metadata = {})
		{
			static_assert(sizeof(T) <= MAX_COMMAND_SIZE, "Command exceeds maximum size");
			
			// Copy the command data
			std::memcpy(m_CommandData, &commandData, sizeof(T));
			m_CommandSize = sizeof(T);
			m_CommandType = commandData.header.type;
			m_DispatchFn = CommandDispatch::GetDispatchFunction(m_CommandType);

			if (!m_DispatchFn && m_CommandType != CommandType::Invalid)
			{
				OLO_CORE_WARN("No dispatch function found for command type {}", static_cast<int>(m_CommandType));
			}
			
			// Set metadata
			m_Metadata = metadata;
			
			// Set default keys if not specified in metadata
			if (m_Metadata.shaderKey == 0 && m_CommandType == CommandType::DrawMesh)
			{
				auto* cmd = reinterpret_cast<DrawMeshCommand*>(m_CommandData);
				// Use shader's renderer ID instead of shaderID
				if (cmd->shader)
					m_Metadata.shaderKey = cmd->shader->GetRendererID();
			}
			
			if (m_Metadata.textureKey == 0 && m_CommandType == CommandType::DrawMesh)
			{
				auto* cmd = reinterpret_cast<DrawMeshCommand*>(m_CommandData);
				if (cmd->useTextureMaps)
				{
					// Use texture renderer IDs instead of texture IDs
					u64 diffuseID = cmd->diffuseMap ? cmd->diffuseMap->GetRendererID() : 0;
					u64 specularID = cmd->specularMap ? cmd->specularMap->GetRendererID() : 0;
					m_Metadata.textureKey = diffuseID ^ (specularID << 16);
				}
			}
		}
        
        // Execute the command with a RendererAPI
        void Execute(RendererAPI& rendererAPI) const;
        
        // Linked list functionality
        void SetNext(CommandPacket* next) { m_Next = next; }
        CommandPacket* GetNext() const { return m_Next; }
        
        // Packet comparison for sorting
        bool operator<(const CommandPacket& other) const;
        
        // Getters for command properties
        CommandType GetCommandType() const { return m_CommandType; }
        const PacketMetadata& GetMetadata() const { return m_Metadata; }
        
        // Returns true if this packet can be batched with another packet
        bool CanBatchWith(const CommandPacket& other) const;

        // Get the command data for direct access (needed for batching)
        template<typename T>
        T* GetCommandData()
        {
            static_assert(sizeof(T) <= MAX_COMMAND_SIZE, "Command exceeds maximum size");
            return reinterpret_cast<T*>(m_CommandData);
        }

        template<typename T>
        const T* GetCommandData() const
        {
            static_assert(sizeof(T) <= MAX_COMMAND_SIZE, "Command exceeds maximum size");
            return reinterpret_cast<const T*>(m_CommandData);
        }

        // Get raw command data as void pointer (for operations that don't need to know the type)
        const void* GetRawCommandData() const { return m_CommandData; }
        void* GetRawCommandData() { return m_CommandData; }

        // Return command size for memory management
        sizet GetCommandSize() const { return m_CommandSize; }

        // Clone this packet (for cases where we need to duplicate commands)
        CommandPacket* Clone(class CommandAllocator& allocator) const;

        // For batch merging - update or modify command data
        bool UpdateCommandData(const void* data, sizet size);

        // For debugging - get the command type as a string
        const char* GetCommandTypeString() const
        {
            switch (m_CommandType)
            {
                case CommandType::Clear: return "Clear";
                case CommandType::DrawMesh: return "DrawMesh";
                case CommandType::DrawMeshInstanced: return "DrawMeshInstanced";
                case CommandType::DrawQuad: return "DrawQuad";
                default: return "Unknown";
            }
        }

        // Setters for command properties when working with raw data
        void SetCommandType(CommandType type) { m_CommandType = type; }
        void SetDispatchFunction(CommandDispatchFn fn) { m_DispatchFn = fn; }
        void SetMetadata(const PacketMetadata& metadata) { m_Metadata = metadata; }
        
    private:
        // Command data
        u8 m_CommandData[MAX_COMMAND_SIZE];
        sizet m_CommandSize = 0;
        CommandType m_CommandType = CommandType::Invalid;
        CommandDispatchFn m_DispatchFn = nullptr;
        
        // Metadata
        PacketMetadata m_Metadata;
        
        // Linked list
        CommandPacket* m_Next = nullptr;
    };
}
