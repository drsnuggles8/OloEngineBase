#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderCommands/RenderCommandBase.h"
#include <vector>
#include <mutex>
#include <atomic>

namespace OloEngine 
{
    // 64-bit key for sorting commands
    struct CommandKey 
    {
        union 
        {
            struct 
            {
                u16 MaterialID;    // Material sorting key
                u16 ShaderID;      // Shader program ID
                u16 TextureID;     // Primary texture ID
                u8 RenderPass;     // Render pass priority
                u8 CommandType;    // Higher priority for state changes vs. draw calls
            };
            u64 Value;
        };
        
        CommandKey() : Value(0) {}
        explicit CommandKey(u64 value) : Value(value) {}
        
        // Comparison operators
        bool operator<(const CommandKey& other) const { return Value < other.Value; }
        bool operator==(const CommandKey& other) const { return Value == other.Value; }
        bool operator!=(const CommandKey& other) const { return Value != other.Value; }
    };

    class CommandBucket 
    {
    public:
        CommandBucket() = default;
        ~CommandBucket();

        // Add a command packet with a key
        void AddPacket(const CommandKey& key, CommandPacket* packet);
        
        // Sort all commands by key
        void Sort();
        
        // Execute all commands in sorted order
        void Execute();
        
        // Merge compatible commands in the bucket
        void MergeCommands();
        
        // Clear the bucket
        void Clear();

        // Accessors
        [[nodiscard]] sizet GetCommandCount() const { return m_CommandCount; }
        [[nodiscard]] const std::vector<CommandPacket*>& GetCommands() const { return m_OrderedCommands; }
        
    private:
        struct KeyedPacket 
        {
            CommandKey Key;
            CommandPacket* Packet;
            
            bool operator<(const KeyedPacket& other) const { return Key < other.Key; }
        };
        
        std::vector<KeyedPacket> m_Commands;   // Key-value pairs for sorting
        std::vector<CommandPacket*> m_OrderedCommands; // Ordered commands after sorting
        std::mutex m_Mutex;
        std::atomic<sizet> m_CommandCount{0};
        bool m_IsSorted = false;
    };
}