#pragma once

#include "OloEngine/Core/Base.h"
#include "CapturedFrameData.h"
#include "OloEngine/Threading/Mutex.h"

#include <deque>

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
            return m_State;
        }
        bool IsCapturing() const
        {
            return m_State != CaptureState::Idle;
        }

        // Configuration
        void SetMaxCapturedFrames(u32 maxFrames)
        {
            m_MaxCapturedFrames = maxFrames;
        }
        u32 GetMaxCapturedFrames() const
        {
            return m_MaxCapturedFrames;
        }

        // Capture hooks — called from SceneRenderPass::Execute()
        void OnPreSort(const CommandBucket& bucket);
        void OnPostSort(const CommandBucket& bucket);
        void OnPostBatch(const CommandBucket& bucket);
        void OnFrameEnd(u32 frameNumber, f64 sortTimeMs, f64 batchTimeMs, f64 executeTimeMs);

        // Access captured data (thread-safe copies for UI consumption)
        std::deque<CapturedFrameData> GetCapturedFramesCopy() const;
        const std::deque<CapturedFrameData>& GetCapturedFrames() const
        {
            return m_CapturedFrames;
        }
        sizet GetCapturedFrameCount() const;
        void ClearCaptures();

        // Selection
        void SetSelectedFrameIndex(i32 index)
        {
            m_SelectedFrameIndex = index;
        }
        i32 GetSelectedFrameIndex() const
        {
            return m_SelectedFrameIndex;
        }
        const CapturedFrameData* GetSelectedFrame() const;

      private:
        FrameCaptureManager() = default;
        ~FrameCaptureManager() = default;
        FrameCaptureManager(const FrameCaptureManager&) = delete;
        FrameCaptureManager& operator=(const FrameCaptureManager&) = delete;

        // Deep-copy all commands from a bucket into a vector
        void DeepCopyCommands(const CommandBucket& bucket, std::vector<CapturedCommandData>& outCommands, bool useSortedOrder);

        CaptureState m_State = CaptureState::Idle;
        u32 m_MaxCapturedFrames = 60;
        i32 m_SelectedFrameIndex = -1;

        // In-progress frame being built during capture hooks
        CapturedFrameData m_PendingFrame;
        bool m_HasPendingPreSort = false;
        bool m_HasPendingPostSort = false;
        bool m_HasPendingPostBatch = false;

        std::deque<CapturedFrameData> m_CapturedFrames;

        mutable FMutex m_Mutex;
    };
} // namespace OloEngine
