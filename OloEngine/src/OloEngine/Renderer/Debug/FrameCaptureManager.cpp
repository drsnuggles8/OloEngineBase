#include "OloEnginePCH.h"
#include "FrameCaptureManager.h"
#include "GPUTimerQueryPool.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <chrono>

namespace OloEngine
{
    FrameCaptureManager& FrameCaptureManager::GetInstance()
    {
        static FrameCaptureManager instance;
        return instance;
    }

    void FrameCaptureManager::CaptureNextFrame()
    {
        OLO_PROFILE_FUNCTION();
        auto expected = CaptureState::Idle;
        if (m_State.compare_exchange_strong(expected, CaptureState::CaptureNextFrame, std::memory_order_acq_rel))
        {
            m_PendingFrame = CapturedFrameData{};
            m_HasPendingPreSort = false;
            m_HasPendingPostSort = false;
            m_HasPendingPostBatch = false;
        }
    }

    void FrameCaptureManager::StartRecording()
    {
        auto expected = CaptureState::Idle;
        if (m_State.compare_exchange_strong(expected, CaptureState::Recording, std::memory_order_acq_rel))
        {
            m_PendingFrame = CapturedFrameData{};
            m_HasPendingPreSort = false;
            m_HasPendingPostSort = false;
            m_HasPendingPostBatch = false;
        }
    }

    void FrameCaptureManager::StopRecording()
    {
        auto expected = CaptureState::Recording;
        m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel);
    }

    std::optional<CapturedFrameData> FrameCaptureManager::GetSelectedFrame() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        i32 idx = m_SelectedFrameIndex.load(std::memory_order_acquire);
        if (idx >= 0 && idx < static_cast<i32>(m_CapturedFrames.size()))
        {
            return m_CapturedFrames[idx];
        }
        return std::nullopt;
    }

    std::deque<CapturedFrameData> FrameCaptureManager::GetCapturedFramesCopy() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_CapturedFrames;
    }

    sizet FrameCaptureManager::GetCapturedFrameCount() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_CapturedFrames.size();
    }

    void FrameCaptureManager::ClearCaptures()
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        m_CapturedFrames.clear();
        m_CaptureGeneration.fetch_add(1, std::memory_order_release);
    }

    void FrameCaptureManager::OnPreSort(const CommandBucket& bucket)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        m_PendingFrame.PreSortCommands.clear();
        DeepCopyCommands(bucket, m_PendingFrame.PreSortCommands, false);
        m_HasPendingPreSort = true;
    }

    void FrameCaptureManager::OnPostSort(const CommandBucket& bucket)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        m_PendingFrame.PostSortCommands.clear();
        DeepCopyCommands(bucket, m_PendingFrame.PostSortCommands, true);
        m_HasPendingPostSort = true;
    }

    void FrameCaptureManager::OnPostBatch(const CommandBucket& bucket)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        m_PendingFrame.PostBatchCommands.clear();
        DeepCopyCommands(bucket, m_PendingFrame.PostBatchCommands, true);
        m_HasPendingPostBatch = true;
    }

    void FrameCaptureManager::OnFrameEnd(u32 frameNumber, f64 sortTimeMs, f64 batchTimeMs, f64 executeTimeMs)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        m_PendingFrame.FrameNumber = frameNumber;
        m_PendingFrame.TimestampSeconds =
            std::chrono::duration<f64>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();

        m_PendingFrame.Stats.TotalCommands = static_cast<u32>(m_PendingFrame.PreSortCommands.size());
        m_PendingFrame.Stats.SortTimeMs = sortTimeMs;
        m_PendingFrame.Stats.BatchTimeMs = batchTimeMs;
        m_PendingFrame.Stats.ExecuteTimeMs = executeTimeMs;
        m_PendingFrame.Stats.TotalFrameTimeMs = sortTimeMs + batchTimeMs + executeTimeMs;

        // Count draw calls and state changes from post-batch (or post-sort) commands
        const auto& finalCommands = m_HasPendingPostBatch
                                        ? m_PendingFrame.PostBatchCommands
                                        : (m_HasPendingPostSort ? m_PendingFrame.PostSortCommands : m_PendingFrame.PreSortCommands);

        u32 drawCalls = 0;
        u32 stateChanges = 0;
        for (const auto& cmd : finalCommands)
        {
            if (cmd.IsDrawCommand())
                drawCalls++;
            else if (cmd.IsStateCommand())
                stateChanges++;
        }
        m_PendingFrame.Stats.DrawCalls = drawCalls;
        m_PendingFrame.Stats.StateChanges = stateChanges;

        // Populate GPU timing from the previous frame's readback
        // (GPU timer uses double-buffered queries; results lag by one frame)
        auto& gpuTimer = GPUTimerQueryPool::GetInstance();
        if (gpuTimer.IsInitialized() && gpuTimer.GetReadableQueryCount() > 0)
        {
            // Apply to post-sort commands (the execution order)
            auto& timedCommands = m_HasPendingPostBatch
                                      ? m_PendingFrame.PostBatchCommands
                                      : (m_HasPendingPostSort ? m_PendingFrame.PostSortCommands : m_PendingFrame.PreSortCommands);

            for (u32 i = 0; i < static_cast<u32>(timedCommands.size()) && i < gpuTimer.GetReadableQueryCount(); ++i)
            {
                timedCommands[i].SetGpuTimeMs(gpuTimer.GetQueryResultMs(i));
            }
        }

        // Count batched commands (difference between post-sort and post-batch)
        if (m_HasPendingPostSort && m_HasPendingPostBatch)
        {
            i32 diff = static_cast<i32>(m_PendingFrame.PostSortCommands.size()) - static_cast<i32>(m_PendingFrame.PostBatchCommands.size());
            m_PendingFrame.Stats.BatchedCommands = diff > 0 ? static_cast<u32>(diff) : 0;
        }

        // Push the completed frame (lock protects concurrent UI reads)
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            m_CapturedFrames.push_back(std::move(m_PendingFrame));

            // Trim to max
            while (m_CapturedFrames.size() > m_MaxCapturedFrames.load(std::memory_order_relaxed))
            {
                m_CapturedFrames.pop_front();
                i32 sel = m_SelectedFrameIndex.load(std::memory_order_relaxed);
                if (sel > 0)
                    m_SelectedFrameIndex.fetch_sub(1, std::memory_order_relaxed);
            }

            // Auto-select the latest frame if nothing is selected
            if (m_SelectedFrameIndex.load(std::memory_order_relaxed) < 0)
            {
                m_SelectedFrameIndex.store(static_cast<i32>(m_CapturedFrames.size()) - 1, std::memory_order_relaxed);
            }

            m_CaptureGeneration.fetch_add(1, std::memory_order_release);
        }

        // State machine transition
        auto expected = CaptureState::CaptureNextFrame;
        m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel);

        // Re-init pending frame for the next capture
        m_PendingFrame = CapturedFrameData{};
        m_HasPendingPreSort = false;
        m_HasPendingPostSort = false;
        m_HasPendingPostBatch = false;
    }

    void FrameCaptureManager::DeepCopyCommands(const CommandBucket& bucket,
                                               std::vector<CapturedCommandData>& outCommands,
                                               bool useSortedOrder)
    {
        OLO_PROFILE_FUNCTION();
        outCommands.clear();

        if (useSortedOrder)
        {
            const auto& sorted = bucket.GetSortedCommands();
            outCommands.reserve(sorted.size());

            for (u32 i = 0; i < static_cast<u32>(sorted.size()); ++i)
            {
                const CommandPacket* packet = sorted[i];
                if (!packet)
                    continue;

                const auto& meta = packet->GetMetadata();
                outCommands.emplace_back(
                    packet->GetCommandType(),
                    packet->GetRawCommandData(),
                    packet->GetCommandSize(),
                    meta.m_SortKey,
                    meta.m_GroupID,
                    meta.m_ExecutionOrder,
                    meta.m_IsStatic,
                    meta.m_DependsOnPrevious,
                    meta.m_DebugName,
                    i);
            }
        }
        else
        {
            // Traverse linked list (submission order)
            outCommands.reserve(bucket.GetCommandCount());
            u32 index = 0;
            CommandPacket* current = bucket.GetCommandHead();
            while (current)
            {
                const auto& meta = current->GetMetadata();
                outCommands.emplace_back(
                    current->GetCommandType(),
                    current->GetRawCommandData(),
                    current->GetCommandSize(),
                    meta.m_SortKey,
                    meta.m_GroupID,
                    meta.m_ExecutionOrder,
                    meta.m_IsStatic,
                    meta.m_DependsOnPrevious,
                    meta.m_DebugName,
                    index++);
                current = current->GetNext();
            }
        }
    }

} // namespace OloEngine
