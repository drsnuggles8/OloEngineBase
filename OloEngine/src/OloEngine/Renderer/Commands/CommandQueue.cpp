#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandQueue.h"
#include "OloEngine/Renderer/Commands/CommandDispatcher.h"

namespace OloEngine
{
    // Initialize the thread-local allocator pointer
    thread_local ThreadLocalCommandAllocator* CommandQueue::t_LocalAllocator = nullptr;

    CommandQueue::CommandQueue(const CommandQueueConfig& config)
        : m_Config(config)
    {
    }

    CommandQueue::~CommandQueue()
    {
        Shutdown();
    }

    void CommandQueue::Init()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing CommandQueue");

        // Create per-frame data
        m_Frames.resize(m_Config.MaxPendingFrames);
        for (auto& frameData : m_Frames)
        {
            frameData.Bucket = CreateRef<CommandBucket>();
            frameData.Allocator = CreateRef<CommandAllocator>(m_Config.InitialAllocationSize);
        }

        // Set initial frame
        m_CurrentFrameIndex = 0;
        m_CurrentBucket = m_Frames[m_CurrentFrameIndex].Bucket.get();
        m_CurrentAllocator = m_Frames[m_CurrentFrameIndex].Allocator.get();

        // Initialize command dispatcher
        CommandDispatcher::Init();

        // Reset statistics
        m_Stats.Reset();
    }

    void CommandQueue::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Shutting down CommandQueue");

        // Clear thread-local allocators
        m_ThreadLocalAllocators.clear();
        
        // Clear frame data
        m_Frames.clear();
        m_CurrentBucket = nullptr;
        m_CurrentAllocator = nullptr;

        // Shutdown command dispatcher
        CommandDispatcher::Shutdown();
    }

    void CommandQueue::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();
        
        // Swap to next frame
        SwapFrameData();
        
        // Reset current frame allocator
        m_CurrentAllocator->Reset();
        
        // Clear current frame bucket
        m_CurrentBucket->Clear();
        
        // Reset per-frame stats
        m_Stats.DrawCalls = 0;
        m_Stats.StateChanges = 0;
        m_Stats.ResourceBinds = 0;
        m_Stats.TotalCommands = 0;
        m_Stats.MergedCommands = 0;
        
        // Update frame index in stats
        m_Stats.FrameIndex++;
    }

    void CommandQueue::EndFrame()
    {
        OLO_PROFILE_FUNCTION();
        
        // Sort and merge commands if enabled
        if (m_Config.EnableSorting)
        {
            m_CurrentBucket->Sort();
        }
        
        if (m_Config.EnableMerging)
        {
            m_CurrentBucket->MergeCommands();
        }
        
        // Update block count in statistics
        m_Stats.AllocatedBlocks = static_cast<u32>(m_CurrentAllocator->GetBlockCount());
    }

    void CommandQueue::Execute()
    {
        OLO_PROFILE_FUNCTION();
        
        // Begin GPU timer for this frame
        // TODO: Add GPU timer implementation
        
        // Execute all commands in the bucket
        m_CurrentBucket->Execute();
        
        // End GPU timer
        // TODO: Capture GPU time
    }

    void CommandQueue::SwapFrameData()
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        
        // Move to next frame
        m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % m_Frames.size();
        
        // Update current frame resources
        m_CurrentBucket = m_Frames[m_CurrentFrameIndex].Bucket.get();
        m_CurrentAllocator = m_Frames[m_CurrentFrameIndex].Allocator.get();
        
        // Reset thread-local allocators if needed
        if (m_Config.EnableMultithreading)
        {
            for (auto& allocator : m_ThreadLocalAllocators)
            {
                allocator->Reset();
            }
        }
    }
}