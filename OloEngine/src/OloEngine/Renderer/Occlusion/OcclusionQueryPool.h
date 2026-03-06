#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    /// @brief Pool of GPU occlusion query objects for visibility testing.
    ///
    /// Uses GL_ANY_SAMPLES_PASSED queries with double-buffered readback:
    /// Frame N issues queries, Frame N+1 reads results (avoids GPU stalls).
    /// Each query maps to an object ID for per-object visibility tracking.
    class OcclusionQueryPool
    {
      public:
        static OcclusionQueryPool& GetInstance();

        /// @brief Allocate query objects. Call once after GL context is valid.
        void Initialize(u32 maxQueries = 1024);

        /// @brief Delete all query objects.
        void Shutdown();

        /// @brief Begin a new frame. Swaps read/write buffers and reads back previous frame results.
        /// @return True if results from the previous frame are available for readback.
        bool BeginFrame();

        /// @brief Start an occlusion query for the given object index.
        void BeginQuery(u32 objectIndex);

        /// @brief Stop the current occlusion query.
        void EndQuery(u32 objectIndex);

        /// @brief End the current frame of occlusion queries.
        void EndFrame();

        /// @brief Check if an object was visible in the *previous* frame.
        /// @return True if any samples passed, false if fully occluded.
        bool WasVisible(u32 objectIndex) const;

        /// @brief Get the GL query object ID for conditional rendering.
        /// Returns the read-buffer query ID for the given object index.
        u32 GetQueryID(u32 objectIndex) const;

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

        /// @brief Whether queries are currently active (between BeginFrame/EndFrame).
        bool IsActive() const
        {
            return m_Active;
        }

        u32 GetMaxQueries() const
        {
            return m_MaxQueries;
        }

      private:
        OcclusionQueryPool() = default;
        ~OcclusionQueryPool();
        OcclusionQueryPool(const OcclusionQueryPool&) = delete;
        OcclusionQueryPool& operator=(const OcclusionQueryPool&) = delete;

        // Double-buffered: index 0 and 1
        std::vector<u32> m_QueryObjects[2]; // GL query IDs
        std::vector<bool> m_Results;        // Readback visibility results

        u32 m_MaxQueries = 0;
        u32 m_WriteBuffer = 0;        // Buffer currently being written to
        u32 m_WriteQueryCount = 0;    // Queries issued this frame
        u32 m_ReadableQueryCount = 0; // Queries from previous frame available for read
        bool m_Initialized = false;
        bool m_Active = false;
        bool m_FirstFrame = true; // Skip readback on very first frame
    };
} // namespace OloEngine
