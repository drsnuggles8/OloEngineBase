#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <string>
#include <vector>

namespace OloEngine
{
    /// @brief Always-on GPU timing for the whole frame and each render-graph pass.
    ///
    /// Ring-buffered GL_TIMESTAMP query pairs: frame N stamps begin/end timestamps
    /// around the frame's render and around every executed pass; the results are
    /// resolved a few frames later via a non-blocking availability check, so the
    /// published numbers always describe the most recent fully-completed frame.
    ///
    /// Deliberately uses GL_TIMESTAMP (glQueryCounter) instead of GL_TIME_ELAPSED:
    /// TIME_ELAPSED query scopes must not nest, and the frame-capture path
    /// (GPUTimerQueryPool) already owns per-draw TIME_ELAPSED scopes *inside* the
    /// pass brackets this pool times. Timestamps are scope-free and coexist.
    class GPUPassTimerPool
    {
      public:
        struct PassTiming
        {
            std::string Name;
            f64 GpuMs = 0.0;
        };

        static GPUPassTimerPool& GetInstance();

        /// @brief Allocate query objects. Call once after the GL context is valid.
        void Initialize(u32 maxPassesPerFrame = 96);

        /// @brief Delete all query objects.
        void Shutdown();

        /// @brief Advance to the next ring slot, resolve any completed older
        /// slots (non-blocking), and stamp the frame-begin timestamp.
        void BeginFrame();

        /// @brief Stamp the frame-end timestamp and mark the slot for readback.
        void EndFrame();

        /// @brief Stamp the begin timestamp for the named pass. Passes must not
        /// overlap; a Begin without a matching End is dropped.
        void BeginPass(const std::string& name);

        /// @brief Stamp the end timestamp for the pass opened by BeginPass.
        void EndPass();

        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }

        /// @brief Whole-frame GPU time of the most recently resolved frame in ms
        /// (frame-begin to frame-end timestamp span on the GPU timeline).
        [[nodiscard]] f64 GetLastFrameGpuMs() const
        {
            return m_LastFrameGpuMs;
        }

        /// @brief Per-pass GPU times of the most recently resolved frame, in
        /// execution order. Returns a copy so callers reading via a main-thread
        /// marshal (e.g. the MCP diagnostics server) get a stable snapshot.
        [[nodiscard]] std::vector<PassTiming> GetLastPassTimingsCopy() const
        {
            return m_LastPassTimings;
        }

        /// @brief Frame counter value of the most recently resolved frame (0 when
        /// nothing has resolved yet). Compare against GetCurrentFrameNumber() to
        /// see how many frames the published results lag.
        [[nodiscard]] u64 GetLastResolvedFrameNumber() const
        {
            return m_LastResolvedFrame;
        }

        [[nodiscard]] u64 GetCurrentFrameNumber() const
        {
            return m_FrameCounter;
        }

      private:
        GPUPassTimerPool() = default;
        ~GPUPassTimerPool();
        GPUPassTimerPool(const GPUPassTimerPool&) = delete;
        GPUPassTimerPool& operator=(const GPUPassTimerPool&) = delete;

        // 4 slots: results are read back 1-3 frames after issue without ever
        // blocking; a slot still pending when its turn comes again (GPU >3
        // frames behind — pathological) is dropped instead of waited on.
        static constexpr u32 kSlotCount = 4;

        struct FrameSlot
        {
            // [0] = frame begin, [1] = frame end, then per-pass begin/end pairs
            // at [2 + 2*i] / [3 + 2*i].
            std::vector<u32> Queries;
            std::vector<std::string> PassNames;
            u32 PassCount = 0;
            u64 FrameNumber = 0;
            bool Pending = false; // stamped and awaiting readback
        };

        // Reads the slot's results if the GPU has finished them (checked via the
        // frame-end query — the last one stamped) and publishes them. When
        // `dropIfUnavailable` is set the slot is cleared even if unresolvable,
        // so it can be rewritten.
        void TryResolveSlot(FrameSlot& slot, bool dropIfUnavailable);

        std::array<FrameSlot, kSlotCount> m_Slots;
        u32 m_WriteSlot = 0;
        u32 m_MaxPasses = 0;
        u64 m_FrameCounter = 0;
        bool m_Initialized = false;
        bool m_Active = false;   // between BeginFrame/EndFrame
        bool m_PassOpen = false; // between BeginPass/EndPass

        // Published results (most recently resolved frame).
        std::vector<PassTiming> m_LastPassTimings;
        f64 m_LastFrameGpuMs = 0.0;
        u64 m_LastResolvedFrame = 0;
    };
} // namespace OloEngine
