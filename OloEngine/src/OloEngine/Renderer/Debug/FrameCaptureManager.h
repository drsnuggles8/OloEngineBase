#pragma once

#include "OloEngine/Core/Base.h"
#include "CapturedFrameData.h"
#include "OloEngine/Threading/Mutex.h"

#include <atomic>
#include <deque>
#include <optional>
#include <string_view>

namespace OloEngine
{
    class CommandBucket;

    // Recording state machine
    enum class CaptureState : u8
    {
        Idle = 0,         // Not capturing
        CaptureNextFrame, // Will capture the next frame, then return to Idle
        Recording         // Continuously capturing until stopped
    };

    // Manages frame capture/recording for the command bucket visualization tool
    class FrameCaptureManager
    {
      public:
        static FrameCaptureManager& GetInstance();

        // State machine control
        void CaptureNextFrame();
        void StartRecording();
        void StopRecording();
        CaptureState GetState() const
        {
            return m_State.load(std::memory_order_acquire);
        }
        bool IsCapturing() const
        {
            return m_State.load(std::memory_order_acquire) != CaptureState::Idle;
        }

        // Configuration
        void SetMaxCapturedFrames(u32 maxFrames)
        {
            m_MaxCapturedFrames.store(maxFrames, std::memory_order_release);
        }
        u32 GetMaxCapturedFrames() const
        {
            return m_MaxCapturedFrames.load(std::memory_order_acquire);
        }

        // Records the name of the SOURCE render-graph pass — the pass whose
        // command bucket becomes CapturedFrameData's top-level PreSort/PostSort/
        // PostBatch lists (SceneRenderPass today). No-op when not capturing. Lets
        // the breakdown attribute the top-level bucket to a real graph pass instead
        // of a hard-coded label.
        void SetSourcePass(std::string_view passName);

        // Per-pass capture (issue #463 / #316 Part 4) ------------------------
        // Each command-bucket pass (Scene, Water, Foliage, Decal, ForwardOverlay)
        // calls BeginPass(GetName()) at the top of its Execute() before any early
        // return, so even a pass with an empty bucket registers itself. The
        // subsequent OnPreSort/OnPostSort/OnPostBatch hooks then fill THIS pass's
        // per-pass entry (CapturedFrameData::Passes), not a single shared list, so
        // the whole graph's buckets accumulate across passes before the frame is
        // committed. No-op when not capturing.
        void BeginPass(std::string_view passName);

        // Capture hooks — called by each command-bucket pass's Execute(). They
        // target the CURRENT per-pass entry (created by BeginPass, or implicitly
        // created on first use for the legacy single-pass direct-API path).
        void OnPreSort(const CommandBucket& bucket);
        void OnPostSort(const CommandBucket& bucket);
        void OnPostBatch(const CommandBucket& bucket);

        // Record the current pass's sort/batch/execute timings (the scene pass
        // calls this at the end of its Execute()). These feed the committed
        // frame's top-level Stats for the source pass. No-op when not capturing.
        void RecordPassTimings(f64 sortTimeMs, f64 batchTimeMs, f64 executeTimeMs);

        // Central post-graph commit (issue #463). Called once from
        // Renderer3D::EndScene AFTER the whole render graph has executed, so the
        // pending frame has accumulated every command-bucket pass. Finalises the
        // frame (derives the top-level source-pass view, computes stats, snapshots
        // render state, pushes it to the captured-frame ring) and advances the
        // state machine. No-op when not capturing. Frame numbers auto-increment.
        void CommitFrame();

        // Legacy single-pass commit. Records the current pass's timings and commits
        // with an explicit frame number in one call. Retained for the direct-API
        // unit tests (FrameCaptureTest) and any single-pass driver; the production
        // multi-pass path uses RecordPassTimings + CommitFrame instead.
        void OnFrameEnd(u32 frameNumber, f64 sortTimeMs, f64 batchTimeMs, f64 executeTimeMs);

        // Access captured data (thread-safe copies for UI consumption)
        std::deque<CapturedFrameData> GetCapturedFramesCopy() const;
        sizet GetCapturedFrameCount() const;
        u64 GetCaptureGeneration() const
        {
            return m_CaptureGeneration.load(std::memory_order_acquire);
        }
        void ClearCaptures();

        // Selection
        void SetSelectedFrameIndex(i32 index)
        {
            m_SelectedFrameIndex.store(index, std::memory_order_release);
        }
        i32 GetSelectedFrameIndex() const
        {
            return m_SelectedFrameIndex.load(std::memory_order_acquire);
        }
        std::optional<CapturedFrameData> GetSelectedFrame() const;

      private:
        FrameCaptureManager() = default;
        ~FrameCaptureManager() = default;
        FrameCaptureManager(const FrameCaptureManager&) = delete;
        FrameCaptureManager& operator=(const FrameCaptureManager&) = delete;

        // Deep-copy all commands from a bucket into a vector
        void DeepCopyCommands(const CommandBucket& bucket, std::vector<CapturedCommandData>& outCommands, bool useSortedOrder) const;

        // Return the current per-pass entry being built, creating an implicit one
        // when no BeginPass() has run yet (the legacy single-pass direct-API path).
        CapturedPassData& EnsureCurrentPass();

        // Finalise m_PendingFrame and push it to the captured-frame ring. Shared
        // by CommitFrame() (central, auto frame number) and OnFrameEnd() (legacy,
        // explicit frame number). Resets the pending frame afterwards.
        void CommitPendingFrame(u32 frameNumber);

        std::atomic<CaptureState> m_State = CaptureState::Idle;
        std::atomic<u32> m_MaxCapturedFrames = 60;
        std::atomic<i32> m_SelectedFrameIndex = -1;
        std::atomic<u64> m_CaptureGeneration = 0;

        // In-progress frame being built during capture hooks. The per-pass entries
        // accumulate in m_PendingFrame.Passes; m_CurrentPassIndex points at the
        // entry the OnPreSort/OnPostSort/OnPostBatch hooks currently fill (-1 = none
        // yet). m_CommittedFrameCounter feeds CommitFrame()'s auto frame numbers.
        CapturedFrameData m_PendingFrame;
        i32 m_CurrentPassIndex = -1;
        u32 m_CommittedFrameCounter = 0;

        std::deque<CapturedFrameData> m_CapturedFrames;

        mutable FMutex m_Mutex;
    };
} // namespace OloEngine
