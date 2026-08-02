#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

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
        void EndQuery(u32 objectIndex) const;

        /// @brief End the current frame of occlusion queries.
        void EndFrame();

        /// @brief Check if an object was visible in the *previous* frame.
        /// @return True if any samples passed, false if fully occluded.
        bool WasVisible(u32 objectIndex) const;

        /// @brief Get the query object's identity for conditional rendering.
        /// Returns the read-buffer query handle for the given object index, or
        /// RHI::NullResource when the pool is down or the index is out of range.
        ///
        /// This is a FRAME-CROSSING read — the handle names a query issued in the
        /// previous frame — which is why the identity matters: a Shutdown() +
        /// Initialize() in between frees the native names, and GL may hand one of
        /// them back to an unrelated query. A retired handle resolves to 0 and
        /// the conditional render is skipped; a recycled name would have gated the
        /// draw on someone else's occlusion result (issue #691 step 3, item 4).
        RHI::ResourceHandle GetQueryHandle(u32 objectIndex) const;

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
        std::vector<RHI::ResourceHandle> m_QueryObjects[2]; // query identities
        std::vector<bool> m_QueryIssued[2];                 // Per-index: was a query actually issued this frame?
        std::vector<bool> m_Results;                        // Readback visibility results

        u32 m_MaxQueries = 0;
        u32 m_WriteBuffer = 0;        // Buffer currently being written to
        u32 m_WriteQueryCount = 0;    // Queries issued this frame
        u32 m_ReadableQueryCount = 0; // Queries from previous frame available for read
        bool m_Initialized = false;
        bool m_Active = false;
        bool m_FirstFrame = true; // Skip readback on very first frame
    };
} // namespace OloEngine
