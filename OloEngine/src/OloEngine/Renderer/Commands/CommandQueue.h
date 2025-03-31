#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderCommands/RenderCommandBase.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

namespace OloEngine 
{
    // Configuration for the command queue
    struct CommandQueueConfig 
    {
        sizet InitialAllocationSize = 64 * 1024;  // 64KB initial block size
        u32 MaxPendingFrames = 3;                 // Number of frames to keep in flight
        bool EnableMultithreading = true;         // Enable multi-threaded command submission
        bool EnableMerging = true;                // Enable command merging
        bool EnableSorting = true;                // Enable command sorting
    };
    
    // Statistics for the command queue
    struct CommandQueueStats 
    {
        std::atomic<u32> TotalCommands{0};
        std::atomic<u32> DrawCalls{0};
        std::atomic<u32> StateChanges{0};
        std::atomic<u32> ResourceBinds{0};
        std::atomic<u32> MergedCommands{0};
        std::atomic<u32> AllocatedBlocks{0};
        std::atomic<u32> FrameIndex{0};
        
        float LastFrameGpuTime = 0.0f;
        float LastFrameCpuTime = 0.0f;
        
        void Reset() 
        {
            TotalCommands = 0;
            DrawCalls = 0;
            StateChanges = 0;
            ResourceBinds = 0;
            MergedCommands = 0;
        }
    };
    
    // Command queue for efficient rendering
    class CommandQueue 
    {
    public:
        explicit CommandQueue(const CommandQueueConfig& config = {});
        ~CommandQueue();
        
        // Initialize the command queue
        void Init();
        
        // Shutdown and clean up resources
        void Shutdown();
        
        // Begin a new frame
        void BeginFrame();
        
        // End the current frame and prepare for rendering
        void EndFrame();
        
        // Execute all commands in the queue
        void Execute();
        
        // Submit a command to the queue
        template<typename T>
        void Submit(const T& command, const CommandKey& key = CommandKey())
        {
            static_assert(std::is_standard_layout<T>::value, "Command must be POD (standard layout)");
            
            // Calculate size needed for the command
            constexpr sizet commandSize = sizeof(T) - sizeof(CommandHeader);
            
            // Set proper command type and flags
            CommandType commandType = command.Header.Type;
            u8 flags = command.Header.Flags;
            
            // Allocate and initialize the command packet
            CommandPacket* packet = m_CurrentAllocator->AllocateCommandPacket(commandSize, commandType);
            if (!packet)
            {
                OLO_CORE_ERROR("Failed to allocate command packet!");
                return;
            }
            
            // Copy command data (excluding the header which is part of the packet)
            memcpy(
                reinterpret_cast<u8*>(&packet->Header) + sizeof(CommandHeader),
                reinterpret_cast<const u8*>(&command) + sizeof(CommandHeader),
                commandSize
            );
            
            // Set header properties
            packet->Header.Type = commandType;
            packet->Header.Size = static_cast<u16>(commandSize);
            packet->Header.Flags = flags;
            
            // Set the dispatch function
            packet->Dispatch = CommandDispatcher::GetDispatchFunction(commandType);
            
            // Add to the command bucket with the specified key
            m_CurrentBucket->AddPacket(key, packet);
            
            // Update statistics
            if (flags & CommandFlags::DrawCall)
                m_Stats.DrawCalls++;
            else if (flags & CommandFlags::StateChange)
                m_Stats.StateChanges++;
            else if (flags & CommandFlags::ResourceBind)
                m_Stats.ResourceBinds++;
                
            m_Stats.TotalCommands++;
        }
        
        // Allocate auxiliary memory that persists until the end of the frame
        void* AllocateAuxMemory(sizet size, sizet alignment = 16)
        {
            if (!m_CurrentAllocator)
            {
                OLO_CORE_ERROR("Cannot allocate memory - Command queue not initialized!");
                return nullptr;
            }
            
            return m_CurrentAllocator->AllocateAuxMemory(size, alignment);
        }
        
        // Get the current statistics
        const CommandQueueStats& GetStats() const { return m_Stats; }
        
        // Reset statistics
        void ResetStats() { m_Stats.Reset(); }
        
    private:
        // Swap frame data to the next frame
        void SwapFrameData();
        
        // Configuration
        CommandQueueConfig m_Config;
        
        // Per-frame data
        struct FrameData
        {
            Ref<CommandBucket> Bucket;
            Ref<CommandAllocator> Allocator;
        };
        
        std::vector<FrameData> m_Frames;
        u32 m_CurrentFrameIndex = 0;
        
        // Current frame resources
        CommandBucket* m_CurrentBucket = nullptr;
        CommandAllocator* m_CurrentAllocator = nullptr;
        
        // Statistics
        CommandQueueStats m_Stats;
        
        // Synchronization
        std::mutex m_QueueMutex;
        
        // Thread-local allocators for multi-threaded command submission
        std::vector<Scope<ThreadLocalCommandAllocator>> m_ThreadLocalAllocators;
        thread_local static ThreadLocalCommandAllocator* t_LocalAllocator;
    };
}