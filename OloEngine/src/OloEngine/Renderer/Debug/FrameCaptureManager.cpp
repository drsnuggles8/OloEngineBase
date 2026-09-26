#include "OloEnginePCH.h"
#include "FrameCaptureManager.h"
#include "GPUTimerQueryPool.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
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
            m_CurrentPassIndex = -1;
        }
    }

    void FrameCaptureManager::StartRecording()
    {
        auto expected = CaptureState::Idle;
        if (m_State.compare_exchange_strong(expected, CaptureState::Recording, std::memory_order_acq_rel))
        {
            m_PendingFrame = CapturedFrameData{};
            m_CurrentPassIndex = -1;
        }
    }

    void FrameCaptureManager::StopRecording()
    {
        // Stop from Recording state
        auto expected = CaptureState::Recording;
        if (m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel))
        {
            return;
        }

        // Also stop from CaptureNextFrame state
        expected = CaptureState::CaptureNextFrame;
        if (m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel))
        {
            return;
        }

        // And from AwaitingGpuResults — the parked one-shot frame is dropped.
        expected = CaptureState::AwaitingGpuResults;
        if (m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel))
        {
            m_PendingFrame = CapturedFrameData{};
            m_CurrentPassIndex = -1;
        }
    }

    bool FrameCaptureManager::CancelCapture()
    {
        // State only, so any thread may call it. The pending frame the game
        // thread may be filling is left alone: once the state is Idle every
        // capture hook and CommitFrame ignore it, and the next arm
        // (CaptureNextFrame / StartRecording) resets it. Compare-exchange, never
        // a blind store: the capture may have completed since the caller gave
        // up on it, and the state may since have moved to Recording, which a
        // store to Idle would stop.
        auto expected = CaptureState::CaptureNextFrame;
        if (m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel))
            return true;
        expected = CaptureState::AwaitingGpuResults;
        return m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel);
    }

    std::optional<CapturedFrameData> FrameCaptureManager::GetSelectedFrame() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        if (i32 idx = m_SelectedFrameIndex.load(std::memory_order_acquire); idx >= 0 && idx < static_cast<i32>(m_CapturedFrames.Num()))
        {
            auto* node = m_CapturedFrames.GetHead();
            while (idx-- > 0)
                node = node->GetNextNode();
            return node->GetValue();
        }
        return std::nullopt;
    }

    TArray<CapturedFrameData> FrameCaptureManager::GetCapturedFramesCopy() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        TArray<CapturedFrameData> frames;
        frames.Reserve(m_CapturedFrames.Num());
        for (auto* node = m_CapturedFrames.GetHead(); node != nullptr; node = node->GetNextNode())
            frames.Add(node->GetValue());
        return frames;
    }

    sizet FrameCaptureManager::GetCapturedFrameCount() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_CapturedFrames.Num();
    }

    void FrameCaptureManager::ClearCaptures()
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        m_CapturedFrames.Empty();
        m_CaptureGeneration.fetch_add(1, std::memory_order_release);
    }

    void FrameCaptureManager::SetSourcePass(std::string_view passName)
    {
        if (!IsCapturing())
            return;

        m_PendingFrame.SourcePassName = FString(passName);
    }

    CapturedPassData& FrameCaptureManager::EnsureCurrentPass()
    {
        if (m_CurrentPassIndex < 0 || m_CurrentPassIndex >= static_cast<i32>(m_PendingFrame.Passes.Num()))
        {
            m_PendingFrame.Passes.Emplace();
            m_CurrentPassIndex = static_cast<i32>(m_PendingFrame.Passes.Num()) - 1;
        }
        return m_PendingFrame.Passes[static_cast<sizet>(m_CurrentPassIndex)];
    }

    void FrameCaptureManager::BeginPass(std::string_view passName)
    {
        if (!IsCapturing())
            return;

        m_PendingFrame.Passes.Emplace();
        m_CurrentPassIndex = static_cast<i32>(m_PendingFrame.Passes.Num()) - 1;
        m_PendingFrame.Passes[static_cast<sizet>(m_CurrentPassIndex)].PassName = FString(passName);
    }

    void FrameCaptureManager::OnPreSort(const CommandBucket& bucket)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        CapturedPassData& pass = EnsureCurrentPass();
        pass.PreSortCommands.Reset();
        DeepCopyCommands(bucket, pass.PreSortCommands, false);
        pass.HasPreSort = true;
    }

    void FrameCaptureManager::OnPostSort(const CommandBucket& bucket)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        CapturedPassData& pass = EnsureCurrentPass();
        pass.PostSortCommands.Reset();
        DeepCopyCommands(bucket, pass.PostSortCommands, true);
        pass.HasPostSort = true;
    }

    void FrameCaptureManager::OnPostBatch(const CommandBucket& bucket)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        CapturedPassData& pass = EnsureCurrentPass();
        pass.PostBatchCommands.Reset();
        DeepCopyCommands(bucket, pass.PostBatchCommands, true);
        pass.HasPostBatch = true;
    }

    void FrameCaptureManager::RecordPassTimings(f64 sortTimeMs, f64 batchTimeMs, f64 executeTimeMs)
    {
        if (!IsCapturing())
            return;

        CapturedPassData& pass = EnsureCurrentPass();
        pass.Stats.SortTimeMs = sortTimeMs;
        pass.Stats.BatchTimeMs = batchTimeMs;
        pass.Stats.ExecuteTimeMs = executeTimeMs;
        pass.Stats.TotalFrameTimeMs = sortTimeMs + batchTimeMs + executeTimeMs;
    }

    CapturedPassData* FrameCaptureManager::FindSourcePass()
    {
        if (!m_PendingFrame.SourcePassName.IsEmpty())
        {
            for (auto& pass : m_PendingFrame.Passes)
            {
                if (pass.PassName == m_PendingFrame.SourcePassName)
                    return &pass;
            }
        }
        return m_PendingFrame.Passes.IsEmpty() ? nullptr : &m_PendingFrame.Passes[0];
    }

    void FrameCaptureManager::ApplyGpuTimingsToSource(const TArray<f64>& resultsMs)
    {
        if (resultsMs.IsEmpty())
            return;

        CapturedPassData* source = FindSourcePass();
        if (!source)
            return;

        // The query index is the command's execution-order position in the
        // bucket ExecuteWithGPUTiming ran, so apply positionally onto the
        // final (executed) list.
        auto& timedCommands = source->HasPostBatch
                                  ? source->PostBatchCommands
                                  : (source->HasPostSort ? source->PostSortCommands : source->PreSortCommands);
        const u32 count = std::min(static_cast<u32>(timedCommands.Num()), static_cast<u32>(resultsMs.Num()));
        for (u32 i = 0; i < count; ++i)
            timedCommands[i].SetGpuTimeMs(resultsMs[i]);
    }

    void FrameCaptureManager::CommitFrame()
    {
        OLO_PROFILE_FUNCTION();

        switch (m_State.load(std::memory_order_acquire))
        {
            case CaptureState::CaptureNextFrame:
            {
                // The per-draw GL_TIME_ELAPSED queries this frame just issued
                // are not readable yet (the GPU is still executing the frame).
                // Park the pending frame and resolve+commit on a later
                // CommitFrame call instead of committing all-zero timings.
                // CAS rather than store: a concurrent StopRecording() may have
                // cancelled the capture since the switch loaded the state — an
                // unconditional store would resurrect it.
                m_GpuResolveWaitFrames = 0;
                auto expected = CaptureState::CaptureNextFrame;
                m_State.compare_exchange_strong(expected, CaptureState::AwaitingGpuResults, std::memory_order_acq_rel);
                break;
            }
            case CaptureState::AwaitingGpuResults:
            {
                const auto& gpuTimer = GPUTimerQueryPool::GetInstance();
                TArray<f64> resultsMs;
                bool resolved = false;
                if (gpuTimer.IsInitialized())
                {
                    resolved = gpuTimer.TryGetIssuedQueryResultsMs(resultsMs);
                    ++m_GpuResolveWaitFrames;
                    if (!resolved && m_GpuResolveWaitFrames < kMaxGpuResolveWaitFrames)
                        return; // try again next frame
                }
                // Pool never initialized (the capture frame issued no queries):
                // nothing will ever resolve — commit immediately without timings.

                // Claim the frame before publishing it (#1504). CancelCapture
                // runs on any thread, so it may have withdrawn the capture since
                // the switch read the state; a withdrawn frame must not reach
                // the ring. Once claimed, a late cancel finds Committing and
                // leaves the publication alone.
                auto claim = CaptureState::AwaitingGpuResults;
                if (!m_State.compare_exchange_strong(claim, CaptureState::Committing, std::memory_order_acq_rel))
                    break;

                if (resolved)
                    ApplyGpuTimingsToSource(resultsMs);
                else
                    OLO_CORE_WARN("FrameCaptureManager: GPU timer results not available — committing capture without per-draw GPU times");

                CommitPendingFrame(++m_CommittedFrameCounter);
                break;
            }
            case CaptureState::Recording:
            {
                // Continuous recording: ExecuteWithGPUTiming ticks the query pool
                // every frame, so the pool's readable results (from the PREVIOUS
                // frame's near-identical command list) are the best available
                // approximation without holding every frame back.
                const auto& gpuTimer = GPUTimerQueryPool::GetInstance();
                if (gpuTimer.IsInitialized() && gpuTimer.GetReadableQueryCount() > 0)
                {
                    TArray<f64> resultsMs;
                    resultsMs.SetNum(static_cast<i32>(gpuTimer.GetReadableQueryCount()), EAllowShrinking::No);
                    for (u32 i = 0; i < gpuTimer.GetReadableQueryCount(); ++i)
                        resultsMs[i] = gpuTimer.GetQueryResultMs(i);
                    ApplyGpuTimingsToSource(resultsMs);
                }
                CommitPendingFrame(++m_CommittedFrameCounter);
                break;
            }
            case CaptureState::Idle:
            default:
                break;
        }
    }

    void FrameCaptureManager::OnFrameEnd(u32 frameNumber, f64 sortTimeMs, f64 batchTimeMs, f64 executeTimeMs)
    {
        OLO_PROFILE_FUNCTION();
        if (!IsCapturing())
            return;

        // Legacy single-pass entrypoint: stash the timings on the current/implicit
        // pass, then commit with the caller-supplied frame number. GPU timings use
        // the pool's readable (previous-tick) results, matching the old behaviour.
        RecordPassTimings(sortTimeMs, batchTimeMs, executeTimeMs);
        if (const auto& gpuTimer = GPUTimerQueryPool::GetInstance(); gpuTimer.IsInitialized() && gpuTimer.GetReadableQueryCount() > 0)
        {
            TArray<f64> resultsMs;
            resultsMs.SetNum(static_cast<i32>(gpuTimer.GetReadableQueryCount()), EAllowShrinking::No);
            for (u32 i = 0; i < gpuTimer.GetReadableQueryCount(); ++i)
                resultsMs[i] = gpuTimer.GetQueryResultMs(i);
            ApplyGpuTimingsToSource(resultsMs);
        }
        CommitPendingFrame(frameNumber);
    }

    void FrameCaptureManager::CommitPendingFrame(u32 frameNumber)
    {
        OLO_PROFILE_FUNCTION();

        // Pick the source / primary pass whose stage lists become the top-level
        // (legacy) view consumed by the markdown report, olo_perf_capture_frame and
        // the single-pass tests. Prefer the recorded SourcePassName; fall back to
        // the first captured pass (the implicit entry the direct-API hooks create
        // when no BeginPass() was issued). Per-command GPU timings have already
        // been applied by ApplyGpuTimingsToSource (deferred one-shot resolve, or
        // previous-tick results for Recording / the legacy OnFrameEnd path), so
        // the copies below carry them into the top-level view.
        CapturedPassData* source = FindSourcePass();

        bool hasPostSort = false;
        bool hasPostBatch = false;
        FrameCaptureStats sourceStats;
        if (source)
        {
            m_PendingFrame.PreSortCommands = source->PreSortCommands;
            m_PendingFrame.PostSortCommands = source->PostSortCommands;
            m_PendingFrame.PostBatchCommands = source->PostBatchCommands;
            hasPostSort = source->HasPostSort;
            hasPostBatch = source->HasPostBatch;
            sourceStats = source->Stats;

            // Legacy implicit pass (no BeginPass/SetSourcePass): adopt its name so
            // the breakdown still labels the bucket.
            if (m_PendingFrame.SourcePassName.IsEmpty())
                m_PendingFrame.SourcePassName = source->PassName;
        }

        m_PendingFrame.FrameNumber = frameNumber;
        m_PendingFrame.TimestampSeconds =
            std::chrono::duration<f64>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();

        m_PendingFrame.Stats.TotalCommands = static_cast<u32>(m_PendingFrame.PreSortCommands.Num());
        m_PendingFrame.Stats.SortTimeMs = sourceStats.SortTimeMs;
        m_PendingFrame.Stats.BatchTimeMs = sourceStats.BatchTimeMs;
        m_PendingFrame.Stats.ExecuteTimeMs = sourceStats.ExecuteTimeMs;
        m_PendingFrame.Stats.TotalFrameTimeMs = sourceStats.SortTimeMs + sourceStats.BatchTimeMs + sourceStats.ExecuteTimeMs;

        // Count draw calls and state changes from post-batch (or post-sort) commands
        const auto& finalCommands = hasPostBatch
                                        ? m_PendingFrame.PostBatchCommands
                                        : (hasPostSort ? m_PendingFrame.PostSortCommands : m_PendingFrame.PreSortCommands);

        u32 drawCalls = 0;
        u32 stateChanges = 0;
        for (const auto& cmd : finalCommands)
        {
            if (cmd.IsDrawCommand())
                ++drawCalls;
            else if (cmd.IsStateCommand())
                ++stateChanges;
            else
            {
                // No additional handling required.
            }
        }
        m_PendingFrame.Stats.DrawCalls = drawCalls;
        m_PendingFrame.Stats.StateChanges = stateChanges;

        // Count batched commands (difference between post-sort and post-batch)
        if (hasPostSort && hasPostBatch)
        {
            i32 diff = static_cast<i32>(m_PendingFrame.PostSortCommands.Num()) - static_cast<i32>(m_PendingFrame.PostBatchCommands.Num());
            m_PendingFrame.Stats.BatchedCommands = diff > 0 ? static_cast<u32>(diff) : 0;
        }

        // Snapshot render state and material data tables so the debugger can inspect
        // the exact data from this frame rather than reading the live FrameDataBuffer.
        {
            const auto& fdb = FrameDataBufferManager::Get();
            u16 rsCount = fdb.GetRenderStateCount();
            m_PendingFrame.RenderStateSnapshot.SetNum(rsCount, EAllowShrinking::No);
            for (u16 i = 0; i < rsCount; ++i)
                m_PendingFrame.RenderStateSnapshot[i] = fdb.GetRenderState(i);

            u16 mdCount = fdb.GetMaterialDataCount();
            m_PendingFrame.MaterialDataSnapshot.SetNum(mdCount, EAllowShrinking::No);
            for (u16 i = 0; i < mdCount; ++i)
                m_PendingFrame.MaterialDataSnapshot[i] = fdb.GetMaterialData(i);
        }

        // Push the completed frame (lock protects concurrent UI reads)
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            m_CapturedFrames.AddTail(std::move(m_PendingFrame));

            // Trim to max
            while (m_CapturedFrames.Num() > m_MaxCapturedFrames.load(std::memory_order_relaxed))
            {
                m_CapturedFrames.RemoveNode(m_CapturedFrames.GetHead());
                i32 sel = m_SelectedFrameIndex.load(std::memory_order_relaxed);
                if (sel > 0)
                    m_SelectedFrameIndex.fetch_sub(1, std::memory_order_relaxed);
            }

            // Auto-select the latest frame if nothing is selected
            if (m_SelectedFrameIndex.load(std::memory_order_relaxed) < 0)
            {
                m_SelectedFrameIndex.store(static_cast<i32>(m_CapturedFrames.Num()) - 1, std::memory_order_relaxed);
            }

            m_CaptureGeneration.fetch_add(1, std::memory_order_release);
        }

        // State machine transition (a one-shot capture — committed either directly
        // via the legacy OnFrameEnd path or from the claimed deferred hold —
        // returns to Idle; recording stays in Recording for the next frame).
        auto expected = CaptureState::Committing;
        if (!m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel))
        {
            expected = CaptureState::CaptureNextFrame;
            m_State.compare_exchange_strong(expected, CaptureState::Idle, std::memory_order_acq_rel);
        }

        // Re-init pending frame for the next capture
        m_PendingFrame = CapturedFrameData{};
        m_CurrentPassIndex = -1;
    }

    void FrameCaptureManager::DeepCopyCommands(const CommandBucket& bucket,
                                               TArray<CapturedCommandData>& outCommands,
                                               bool useSortedOrder) const
    {
        OLO_PROFILE_FUNCTION();
        outCommands.Reset();

        if (useSortedOrder)
        {
            const auto& sorted = bucket.GetSortedCommands();
            outCommands.Reserve(sorted.size());

            for (u32 i = 0; i < static_cast<u32>(sorted.size()); ++i)
            {
                const CommandPacket* packet = sorted[i];
                if (!packet)
                    continue;

                const auto& meta = packet->GetMetadata();
                outCommands.Emplace(
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
            // Iterate flat packet array (submission order before sorting)
            const auto& packets = bucket.GetPackets();
            outCommands.Reserve(packets.size());

            for (u32 i = 0; i < static_cast<u32>(packets.size()); ++i)
            {
                const CommandPacket* packet = packets[i];
                if (!packet)
                    continue;

                const auto& meta = packet->GetMetadata();
                outCommands.Emplace(
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
    }

} // namespace OloEngine
