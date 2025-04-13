#pragma once

#include "OloEngine/Core/Base.h"
#include "CommandAllocator.h"
#include "CommandPacket.h"
#include <memory>
#include <vector>
#include <mutex>

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;

    // Command Memory Manager - manages all command memory allocations and provides a central interface
    class CommandMemoryManager
    {
    public:
        struct Statistics
        {
            u32 ActiveAllocatorCount = 0;
            sizet TotalAllocations = 0;
            sizet ActivePacketCount = 0;
            sizet FramePacketCount = 0;
            sizet PeakPacketCount = 0;
        };

        static void Init();
        static void Shutdown();

        // Get a fresh allocator for a new frame
        static CommandAllocator* GetFrameAllocator();

        // Return an allocator to the pool after use
        static void ReturnAllocator(CommandAllocator* allocator);

        // Create a command packet using the current allocator
        template<typename T>
        static CommandPacket* AllocateCommandPacket(const T& commandData, const PacketMetadata& metadata = {})
        {
            CommandAllocator* allocator = GetCurrentThreadAllocator();
            if (!allocator)
            {
                OLO_CORE_ERROR("CommandMemoryManager: No allocator available for the current thread");
                return nullptr;
            }

            CommandPacket* packet = allocator->CreateCommandPacket(commandData, metadata);
            if (packet)
            {
                std::scoped_lock<std::mutex> lock(s_StatsMutex);
                s_Stats.ActivePacketCount++;
                s_Stats.FramePacketCount++;
                s_Stats.TotalAllocations++;
                s_Stats.PeakPacketCount = std::max(s_Stats.PeakPacketCount, s_Stats.ActivePacketCount);
            }
            
            return packet;
        }

        // Release a command packet (doesn't actually free memory, just marks it as available for reuse)
        static void ReleaseCommandPacket(CommandPacket* packet);

        // Reset all allocators for a new frame
        static void ResetAllocators();

        // Get statistics
        static Statistics GetStatistics();

    private:
        // Get the allocator for the current thread
        static CommandAllocator* GetCurrentThreadAllocator();

        static std::vector<std::unique_ptr<CommandAllocator>> s_AllocatorPool;
        static std::unordered_map<std::thread::id, CommandAllocator*> s_ThreadAllocators;
        static std::mutex s_PoolMutex;
        static std::mutex s_ThreadMapMutex;
        static std::mutex s_StatsMutex;
        static Statistics s_Stats;
        static bool s_Initialized;
    };
}