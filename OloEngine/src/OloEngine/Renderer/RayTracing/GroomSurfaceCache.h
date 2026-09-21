#pragma once

// =============================================================================
// GroomSurfaceCache.h — the resident ray-space geometry of every coat in the
// frame, and the GPU Scene records that put it in the TLAS. Issue #1253.
//
// THE LINE THIS FILE IS ON. Groom/GroomRayTracingProxy.h owns the arithmetic —
// the tier ladder, the budget, the conversion, the error metric — and compiles
// on a machine with no GPU. This owns the buffers, the residency and the
// records, which do not. The split is the one RayTracingScene.h makes at
// IRayTracingBackend and the one VegetationSurfaceCache makes against
// VegetationPolicy, for the same reason: the policy is what CI can test.
//
// WHY THIS IS NOT PART OF GroomRenderPass. The pass runs once per CAMERA; the
// ray-traced scene is built once per FRAME. A proxy maintained in the pass
// would be converted twice in a split-screen scene and its tier hysteresis
// would advance at twice the rate — the same halving #1252 avoids by keeping
// GroomLodState in Scene. So this runs from Renderer3D::EndScene, beside
// VegetationSurfaceCache, exactly once.
//
// WHY IT REBUILDS THE STRAND MESH RATHER THAN BORROWING THE PASS'S. The pass's
// cache holds geometry built at the RASTER budget, which is the wrong size (a
// near coat's full strand set is a million ray-space triangles) and, on Vulkan,
// lives in buffers the pass owns and refills in place. Building a second, much
// smaller strand set here is the cheaper of the two, and it is the only way the
// proxy can have its own budget at all. It goes through the SAME
// BuildGroomStrandMesh, so the binding (#1249), the simulation (#1250), the
// coat's per-strand width (#1251) and the LOD level (#1252) are applied by one
// implementation rather than two that have to agree.
//
// THE UPLOAD IS ORDERED WITHOUT A BARRIER, and that is a property of the path
// rather than an omission. VertexBuffer::SetData lowers to
// VulkanOneShot::UploadToBuffer, a record-submit-WAIT on its own command
// buffer; a one-shot submitted while the frame is still recording executes
// before that frame's submission, because queue submissions execute in submit
// order. The vegetation twin needs RecordDeformToBuildBarrier only because its
// producer is a COMPUTE dispatch inside the frame's own command buffer, which
// this is not.
//
// FAIL-CLOSED, EVERYWHERE. Every refusal — no budget, no slot, no buffer —
// leaves the coat out of the ray-traced scene with a counted reason and leaves
// the RASTER tier completely untouched, which is criterion 4's working quality
// tier. There is no path here that half-represents a coat.
// =============================================================================

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomRayTracingProxy.h"
#include "OloEngine/Groom/GroomStrandRequest.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace OloEngine
{
    class GPUScene;
    class IndexBuffer;
    class VertexBuffer;
} // namespace OloEngine

namespace OloEngine::RayTracing
{
    // Render-thread producer. One call per frame does the whole job: decide
    // each coat's tier, spend the budget in a stable order, convert what it
    // affords, retire what left the scene, and stage the records.
    class GroomSurfaceCache
    {
      public:
        GroomSurfaceCache();
        ~GroomSurfaceCache();
        GroomSurfaceCache(const GroomSurfaceCache&) = delete;
        GroomSurfaceCache& operator=(const GroomSurfaceCache&) = delete;

        void SetEnabled(bool enabled);
        void Shutdown();

        /// Stage every groom the frame published. `requests` is the live vector
        /// Renderer3D holds for this frame; nothing is retained past the call,
        /// which is what keeps a second camera's replacement of that vector
        /// from leaving a dangling read behind.
        ///
        /// `enabled` false still runs: it retires the whole resident set and
        /// reports every groom as NotRequested, so a scene that switches ray
        /// tracing off does not leave a coat's structures resident forever.
        void Extract(GPUScene& scene, std::span<const GroomStrandRequest> requests, bool wanted);

        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }
        [[nodiscard]] const GroomProxyStats& GetStats() const noexcept
        {
            return m_Stats;
        }

      private:
        // Entity id and asset handle. The asset handle is in the key because a
        // groom swapped on a live entity is a different coat with a different
        // curve count, and reusing the entry would refill a buffer sized for
        // the old one.
        using Key = std::tuple<i64, u64>;

        struct Entry
        {
            Ref<VertexBuffer> Vertices;
            Ref<IndexBuffer> Indices;
            /// Bytes of the two buffers above.
            u64 Bytes = 0;
            u64 LastSeen = 0;
            /// The frame this structure was last rebuilt. Drives the
            /// oldest-first refresh order, so a scene past the per-frame
            /// budget spreads its staleness rather than starving the same
            /// animals every frame.
            u64 LastRefreshed = 0;
            /// Everything that decides the emitted bytes. A change
            /// REALLOCATES rather than refills, deliberately — see the
            /// note beside its construction in Extract.
            u64 ShapeHash = 0;
            /// Bumped on every refill. Rides the instance record as
            /// m_DeformedContentRevision, which is what lets RayTracingScene
            /// tell a coat that moved from one that did not — without it a
            /// deformed geometry either refits every frame or never refits.
            u32 Revision = 0;
            u32 VertexCount = 0;
            u32 IndexCount = 0;
            /// What this structure carries, so the frame-wide figures can
            /// be summed over the RESIDENT set rather than over the coats
            /// that happened to rebuild this frame.
            f32 Compensation = 1.0f;
            bool Deformed = false;
        };

        /// Rebuilds one coat's ray-space geometry. Returns
        /// GroomProxyRefusalReason::None on success and the REASON
        /// otherwise — never a bare bool, because the caller cannot
        /// reconstruct which gate refused: a transactional budget has not
        /// reached its caps when it declines, so asking the counters after
        /// the fact reports every budget refusal as an allocator failure.
        ///
        /// On failure the entry's byte accounting is left UNTOUCHED, so the
        /// caller's subtraction balances against the value that was counted.
        [[nodiscard]] GroomProxyRefusalReason Refresh(Entry& entry, const GroomStrandRequest& request,
                                                      const GroomProxyDecision& decision, u64 shapeHash,
                                                      GroomProxyFrameBudget& budget);

        bool m_Enabled = false;
        u64 m_Frame = 0;
        GroomProxyStats m_Stats;
        std::map<Key, Entry> m_Entries;
        /// The tier hysteresis, per entity. Outlives a frame on purpose — it IS
        /// the anti-thrash mechanism — and is pruned with the resident set.
        std::map<i64, GroomProxyState> m_TierState;

        /// Scratch, held across frames so a per-frame conversion does not
        /// allocate. The strand mesh is built into the first pair and converted
        /// into the second; neither is read outside one Extract call.
        std::vector<GroomStrandVertex> m_StrandVertices;
        std::vector<u32> m_StrandIndices;
        std::vector<Vertex> m_ProxyVertices;
        std::vector<u32> m_ProxyIndices;
    };
} // namespace OloEngine::RayTracing
