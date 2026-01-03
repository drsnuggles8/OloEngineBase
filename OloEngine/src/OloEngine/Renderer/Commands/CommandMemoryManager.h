#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "CommandAllocator.h"
#include "CommandPacket.h"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <array>
#include <thread>

namespace OloEngine
{
    class RendererAPI;

    // Maximum number of worker threads (must match CommandBucket's MAX_RENDER_WORKERS)
    static constexpr u32 MAX_ALLOCATOR_WORKERS = 16;

    // Cache-line padded allocator slot for per-worker allocators
    // Prevents false sharing between worker threads
    struct alignas(OLO_PLATFORM_CACHE_LINE_SIZE) WorkerAllocatorSlot
    {
        CommandAllocator* allocator = nullptr;
        std::atomic<bool> inUse{ false };
        // Padding to fill cache line
        u8 _padding[OLO_PLATFORM_CACHE_LINE_SIZE - sizeof(CommandAllocator*) - sizeof(std::atomic<bool>)];
    };
    static_assert(sizeof(WorkerAllocatorSlot) == OLO_PLATFORM_CACHE_LINE_SIZE,
                  "WorkerAllocatorSlot must be exactly one cache line");

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

        static CommandAllocator* GetFrameAllocator();
        static void ReturnAllocator(CommandAllocator* allocator);

        // ====================================================================
        // Per-Worker Allocator API for Parallel Command Generation
        // ====================================================================

        // Get a dedicated allocator for a specific worker thread
        // This allocator is reserved for the worker until released
        // No synchronization needed when using worker allocators
        // @param workerIndex The worker thread index (0 to MAX_ALLOCATOR_WORKERS-1)
        // @return Dedicated allocator for the worker
        static CommandAllocator* GetWorkerAllocator(u32 workerIndex);

        // Prepare all worker allocators for a new frame
        // Resets allocators without returning them to the pool
        // Call at the start of each frame (in BeginScene)
        static void PrepareWorkerAllocatorsForFrame();

        // Release all worker allocators back to the pool
        // Call at the end of each frame (in EndScene)
        static void ReleaseWorkerAllocators();

        // Register current thread as a worker and get a dedicated allocator
        // Combines RegisterWorkerThread() + GetWorkerAllocator()
        // @return Pair of (workerIndex, allocator)
        static std::pair<u32, CommandAllocator*> RegisterAndGetWorkerAllocator();

        // Get worker index for the current thread (-1 if not registered)
        static i32 GetCurrentWorkerIndex();

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

        static void ResetAllocators();
        static Statistics GetStatistics();

      private:
        static CommandAllocator* GetCurrentThreadAllocator();

        static std::vector<std::unique_ptr<CommandAllocator>> s_AllocatorPool;
        static std::unordered_map<std::thread::id, CommandAllocator*> s_ThreadAllocators;
        static std::mutex s_PoolMutex;
        static std::mutex s_ThreadMapMutex;
        static std::mutex s_StatsMutex;
        static Statistics s_Stats;
        static bool s_Initialized;

        // ====================================================================
        // Per-Worker Allocator Storage
        // ====================================================================

        // Cache-line-aligned per-worker allocator slots
        static std::array<WorkerAllocatorSlot, MAX_ALLOCATOR_WORKERS> s_WorkerAllocators;

        // Thread ID to worker index mapping for parallel submission
        static std::unordered_map<std::thread::id, u32> s_ThreadToWorkerIndex;
        static std::mutex s_WorkerMapMutex;
        static std::atomic<u32> s_NextWorkerIndex;
    };
} // namespace OloEngine
