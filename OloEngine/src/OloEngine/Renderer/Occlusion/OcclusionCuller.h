#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Mesh.h"

#include <vector>

namespace OloEngine
{
    /// @brief Submits proxy bounding-box geometry into occlusion queries.
    ///
    /// During the occlusion query pass, for each object that needs a visibility
    /// test, this renders a scaled unit cube matching the object's AABB with
    /// color writes off and depth writes off. The GPU then records whether any
    /// fragments passed the depth test (i.e. the bounding box is visible).
    ///
    /// Proxy submissions are deferred via QueueBoundingBox() during scene
    /// traversal, then flushed via FlushQueuedQueries() after the depth
    /// buffer is populated (e.g. after a depth prepass or first Execute()).
    class OcclusionCuller
    {
      public:
        static OcclusionCuller& GetInstance();

        /// @brief Initialize resources (proxy cube mesh). Call once after GL context.
        void Initialize();

        /// @brief Release resources.
        void Shutdown();

        /// @brief Queue a bounding box for deferred occlusion query submission.
        /// Called during scene traversal (DrawMesh). Does NOT draw immediately.
        void QueueBoundingBox(u32 queryIndex, const BoundingBox& worldBounds);

        /// @brief Execute all queued occlusion query proxy draws.
        /// Call AFTER the depth buffer is populated (e.g. after depth prepass).
        void FlushQueuedQueries();

        bool IsInitialized() const
        {
            return m_Initialized;
        }

      private:
        OcclusionCuller() = default;

        struct PendingQuery
        {
            u32 QueryIndex;
            BoundingBox Bounds;
        };

        Ref<Mesh> m_ProxyCube;
        std::vector<PendingQuery> m_PendingQueries;
        bool m_Initialized = false;
    };
} // namespace OloEngine
