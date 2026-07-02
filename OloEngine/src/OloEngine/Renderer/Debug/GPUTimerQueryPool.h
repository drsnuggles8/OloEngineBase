#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    /// @brief Pool of OpenGL timer query objects for per-command GPU timing.
    ///
    /// Uses GL_TIME_ELAPSED queries with double-buffered readback:
    /// Frame N issues queries, Frame N+1 reads results (avoids GPU stalls).
    /// Only active during capture — zero overhead when idle.
    class GPUTimerQueryPool
    {
      public:
        static GPUTimerQueryPool& GetInstance();

        /// @brief Allocate query objects. Call once after GL context is valid.
        void Initialize(u32 maxQueries = 512);

        /// @brief Delete all query objects.
        void Shutdown();

        /// @brief Begin a new frame of timing. Swaps read/write buffers.
        /// @return True if results from the previous frame are available for readback.
        bool BeginFrame();

        /// @brief Start timing command at the given index.
        void BeginQuery(u32 commandIndex);

        /// @brief Stop timing the current command.
        void EndQuery(u32 commandIndex) const;

        /// @brief End the current frame of timing.
        void EndFrame();

        /// @brief Read back GPU time for a command issued in the *previous* frame.
        /// @return Time in milliseconds, or 0.0 if not available.
        f64 GetQueryResultMs(u32 commandIndex) const;

        /// @brief Non-blocking readback of the queries issued in the most recent
        /// BeginFrame/EndFrame cycle, WITHOUT swapping buffers. Used by
        /// FrameCaptureManager's deferred one-shot commit: the capture frame
        /// issues the queries, then a later frame (when the GPU has caught up)
        /// resolves them straight out of the write buffer before the capture is
        /// committed. Returns false while the results are still pending.
        bool TryGetIssuedQueryResultsMs(std::vector<f64>& outResultsMs) const;

        /// @brief Number of queries issued in the previous (now-readable) frame.
        u32 GetReadableQueryCount() const
        {
            return m_ReadableQueryCount;
        }

        /// @brief Whether the pool has been initialized.
        bool IsInitialized() const
        {
            return m_Initialized;
        }

        /// @brief Whether timing is currently active (between BeginFrame/EndFrame).
        bool IsActive() const
        {
            return m_Active;
        }

        u32 GetMaxQueries() const
        {
            return m_MaxQueries;
        }

      private:
        GPUTimerQueryPool() = default;
        ~GPUTimerQueryPool();
        GPUTimerQueryPool(const GPUTimerQueryPool&) = delete;
        GPUTimerQueryPool& operator=(const GPUTimerQueryPool&) = delete;

        // Double-buffered: index 0 and 1
        std::vector<u32> m_QueryObjects[2]; // GL query IDs
        std::vector<f64> m_Results;         // Readback results in ms

        u32 m_MaxQueries = 0;
        u32 m_WriteBuffer = 0;        // Buffer currently being written to
        u32 m_WriteQueryCount = 0;    // Queries issued this frame
        u32 m_ReadableQueryCount = 0; // Queries from previous frame available for read
        bool m_Initialized = false;
        bool m_Active = false;
        bool m_FirstFrame = true; // Skip readback on very first frame
    };
} // namespace OloEngine
