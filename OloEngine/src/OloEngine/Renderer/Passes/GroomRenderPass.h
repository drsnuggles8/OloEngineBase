#pragma once

// =============================================================================
// GroomRenderPass.h — the production strand visibility pass. Issue #1246.
//
// WHERE IT SITS, AND WHY THERE. In the SceneColor read-modify-write chain,
// after the lit scene and before the transparent modifiers. By then the scene
// framebuffer holds lit colour AND a populated depth attachment on every path —
// forward and Forward+ write it directly, and DeferredLightingPass blits the
// G-Buffer's depth into it immediately before ForwardOverlayRenderPass runs, for
// exactly this reason. So one forward-style pass composes depth-correctly
// against opaque geometry on all three paths without a G-Buffer variant, which
// is what acceptance criterion 2 asks for and what keeps the diff one pass
// rather than three.
//
// REGISTERED ON EVERY PATH AND EVERY BACKEND, AND IT SELF-DISABLES IN Execute.
// Gating registration on a capability would cull the node for the SESSION,
// because the graph fingerprints topology and caches it — the trap
// technique-selection-seams.md §"Hash the new gate into the graph fingerprint"
// describes. A frame with no grooms costs one culled node.
//
// WHAT IT DOES NOT DO. It does not shade hair: the coat is neutral-lit on
// purpose (#1247 owns fibre scattering), and it does not participate in the
// G-Buffer, so a strand receives no deferred lighting and casts no shadow yet.
// Those are named in the docs rather than approximated here, because a
// half-lit coat is the kind of thing that gets mistaken for a finished tier —
// which criterion 4 explicitly forbids.
//
// IT DOES NOT REPLACE GroomPreview. The debug preview draws the same asset as
// debug lines and stays, because it answers a different question (did this
// groom IMPORT correctly) and is the only view that shows roots, groups and
// curve direction. Criterion 4's "diagnostic curve rendering is not advertised
// as a finished quality tier" is satisfied by the two being separate, visibly
// named things — not by deleting one.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomStrandRequest.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Framebuffer;
    class IndexBuffer;
    class Shader;
    class UniformBuffer;
    class VertexArray;
    class VertexBuffer;

    // What the frame RESOLVED, gathered by RenderPipeline and handed here.
    // Every field is a result, never a request — see GroomCompositionInputs.
    struct GroomFrameState
    {
        /// Monotonic frame counter for the stochastic hash. Must advance every
        /// frame or the "stochastic" mode is a fixed dither pattern.
        ///
        /// Fed from Renderer3D's StochasticFrameIndex — the same counter the
        /// ray-traced shadow pass jitters on — so two stochastic consumers
        /// cannot drift into agreeing with each other frame after frame.
        u32 FrameIndex = 0;
        /// TAA or a temporal upscaler is running and will consume this output.
        bool TemporalResolveActive = false;
        /// The OIT accumulation and revealage targets exist this frame.
        bool OITTargetsAvailable = false;
    };

    // What the pass did, for the editor's panel and the PR's evidence.
    struct GroomRenderStats
    {
        u32 GroomsSubmitted = 0;
        u32 GroomsDrawn = 0;
        u32 StrandsDrawn = 0;
        u32 SegmentsDrawn = 0;
        u32 TrianglesDrawn = 0;
        /// GPU bytes resident in the strand-buffer cache.
        u64 CachedBytes = 0;
        u32 CachedGrooms = 0;
        u32 CacheBuilds = 0;
        u32 CacheEvictions = 0;
        GroomCompositionStats Composition;

        void Reset() noexcept
        {
            *this = GroomRenderStats{};
        }
    };

    class GroomRenderPass : public RenderGraphNode
    {
      public:
        GroomRenderPass();
        ~GroomRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        /// Set once per frame by RenderPipeline, before Setup.
        void SetRequests(std::vector<GroomStrandRequest> requests) noexcept
        {
            m_Requests = std::move(requests);
        }
        void SetFrameState(const GroomFrameState& state) noexcept
        {
            m_FrameState = state;
        }

        [[nodiscard]] const GroomRenderStats& GetStats() const noexcept
        {
            return m_Stats;
        }

        /// Upper bound on the strand-buffer cache, in bytes. Exceeding it
        /// evicts least-recently-used entries at the END of a frame, never
        /// during one — an entry evicted while its draw was pending would
        /// leave the frame drawing from a freed buffer.
        void SetCacheBudgetBytes(u64 bytes) noexcept
        {
            m_CacheBudgetBytes = bytes;
        }

        /// The decision this pass WOULD make for `requested`, given the frame
        /// state it currently holds. Exposed so the editor's inspector and a
        /// test can ask without rendering — the reason a groom is not getting
        /// what it asked for should be answerable from the panel that sets it.
        [[nodiscard]] GroomCompositionDecision DecideComposition(GroomCompositionMode requested) const noexcept;

        /// Strand-geometry cache key: the asset handle AND the build settings.
        /// Keying on the handle alone made two entities sharing one groom at
        /// different budgets evict each other every frame, rebuilding the CPU
        /// mesh and both GPU buffers — the cost the cache exists to remove.
        ///
        /// Public because it is a pure function and the cache's correctness
        /// rests on it: it is hashed FIELD BY FIELD rather than over the object
        /// representation, because GroomStrandBuildSettings carries
        /// uninitialised padding, and that distinction is only defensible if
        /// something tests it.
        [[nodiscard]] static u64 CacheKey(const GroomStrandRequest& request) noexcept;

      private:
        // One groom's GPU geometry, keyed by asset handle.
        struct CacheEntry
        {
            Ref<VertexArray> Array;
            Ref<VertexBuffer> Vertices;
            Ref<IndexBuffer> Indices;
            GroomStrandBuildSettings Settings;
            GroomStrandMeshStats Stats;
            u64 Bytes = 0;
            u32 LastUsedFrame = 0;
        };

        [[nodiscard]] CacheEntry* AcquireGeometry(const GroomStrandRequest& request);
        void EvictToBudget();

        std::vector<GroomStrandRequest> m_Requests;
        GroomFrameState m_FrameState;
        GroomRenderStats m_Stats;

        std::unordered_map<u64, CacheEntry> m_Cache;
        u64 m_CacheBudgetBytes = 256ull * 1024ull * 1024ull;
        u64 m_CacheBytes = 0;

        /// The cache's OWN monotonic tick, not GroomFrameState::FrameIndex.
        /// That one is the stochastic sample index and is deliberately wrapped
        /// (`& 0xFFFFF` in RenderPipeline), so an entry used just before the
        /// wrap reads as a million frames old and is evicted, while one from
        /// the previous cycle reads as newly used and stays. A 64-bit counter
        /// that only this pass advances cannot do either.
        u64 m_CacheTick = 0;

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_ParamsUBO;
        Ref<Framebuffer> m_SceneFramebuffer;

        // Last reported dominant fallback, so the log line fires on a CHANGE
        // of reason rather than once per frame.
        GroomCompositionFallbackReason m_LastReportedReason = GroomCompositionFallbackReason::None;
    };
} // namespace OloEngine
