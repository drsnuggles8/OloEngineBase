#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <unordered_map>

namespace OloEngine
{
    /// Number of consecutive invisible frames before re-testing an occluded object.
    /// Shared between Renderer3D::DrawMesh and tests.
    inline constexpr u32 kOcclusionRetestInterval = 4;

    /// @brief Per-object visibility state for temporal occlusion culling.
    ///
    /// Tracks whether an object was visible in recent frames, its assigned
    /// query index in the OcclusionQueryPool, and a frame counter for
    /// temporal coherence (re-test after N invisible frames).
    struct OcclusionState
    {
        u32 QueryIndex = UINT32_MAX; // Index into OcclusionQueryPool (UINT32_MAX = unassigned)
        bool WasVisible = true;      // Result from previous frame's query
        u32 InvisibleFrameCount = 0; // Consecutive frames this object was occluded
        u32 LastTestedFrame = 0;     // Frame number of last occlusion test
    };

    /// @brief Manages per-object occlusion visibility state across frames.
    ///
    /// Maps entity/object IDs to their OcclusionState, and provides
    /// a free-list for query index assignment from the OcclusionQueryPool.
    class OcclusionStateManager
    {
      public:
        static OcclusionStateManager& GetInstance();

        /// @brief Reset all state. Called on scene change.
        void Clear();

        /// @brief Get or create occlusion state for an object.
        OcclusionState& GetOrCreate(u64 objectID);

        /// @brief Check if an object has occlusion state.
        bool Has(u64 objectID) const;

        /// @brief Remove occlusion state for a destroyed object.
        void Remove(u64 objectID);

        /// @brief Allocate the next available query index from the pool.
        /// @return A valid query index, or UINT32_MAX if pool is exhausted.
        u32 AllocateQueryIndex();

        /// @brief Return a query index to the free list.
        void FreeQueryIndex(u32 index);

        /// @brief Set the maximum number of query indices available.
        void SetMaxQueries(u32 max);

        /// @brief Get the current frame number (incremented each BeginFrame).
        u32 GetCurrentFrame() const
        {
            return m_CurrentFrame;
        }

        /// @brief Advance to the next frame.
        void BeginFrame();

      private:
        OcclusionStateManager() = default;

        std::unordered_map<u64, OcclusionState> m_States;
        std::vector<u32> m_FreeQueryIndices;
        u32 m_MaxQueries = 0;
        u32 m_NextQueryIndex = 0;
        u32 m_CurrentFrame = 0;
    };
} // namespace OloEngine
