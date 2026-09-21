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
// IT SHADES HAIR SINCE #1247, BUT ONLY WHERE A MATERIAL WAS AUTHORED. A
// request carrying a GroomFibreComponent is lit by the scene's lights and its
// environment through the fibre BCSDF (Groom/GroomFibreScattering.h); a request
// without one renders #1246's neutral root-to-tip ramp, unchanged. The lighting
// happens in THIS forward-style pass on all three rendering paths, which is
// what makes the material identical across them rather than three shaders that
// agree by inspection.
//
// WHAT IT STILL DOES NOT DO. It does not participate in the G-Buffer, so a
// strand casts no shadow and receives none — including from itself. Inter-fibre
// occlusion and density transport are #1248's, and they are named in the docs
// rather than approximated here, because a half-shadowed coat is the kind of
// thing that gets mistaken for a finished tier — which criterion 4 explicitly
// forbids.
//
// IT DOES NOT REPLACE GroomPreview. The debug preview draws the same asset as
// debug lines and stays, because it answers a different question (did this
// groom IMPORT correctly) and is the only view that shows roots, groups and
// curve direction. Criterion 4's "diagnostic curve rendering is not advertised
// as a finished quality tier" is satisfied by the two being separate, visibly
// named things — not by deleting one.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomCoatShadowTechnique.h"
#include "OloEngine/Groom/GroomStrandCache.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomStrandRequest.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <vector>

namespace OloEngine
{
    class Framebuffer;
    class IndexBuffer;
    class Shader;
    class Texture3D;
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

        // ── Surface binding (#1249) ──────────────────────────────────

        /// Grooms drawn deformed by a body surface this frame.
        u32 GroomsDeformed = 0;
        /// Grooms that asked to be bound and were refused. Non-zero means a coat
        /// is at its bind pose while its body animates, which is exactly the
        /// case that must never be silent.
        u32 GroomsBindingRefused = 0;
        /// Strand roots carried by a valid deformed frame this frame.
        u32 RootsDeformed = 0;
        /// Strand roots held at rest because their deformed triangle collapsed.
        u32 RootsHeldAtRest = 0;
        /// Dynamic vertex-buffer refills a deformed groom cost this frame. One
        /// per deformed groom is healthy; more means the geometry is being
        /// reallocated, not refilled.
        u32 DeformedRebuilds = 0;
        /// Grooms whose previous-frame strand positions were NOT usable, so the
        /// frame emitted zero motion for them rather than a velocity across a
        /// discontinuity.
        u32 GroomsHistoryRejected = 0;

        // == Guide simulation (#1250) ==
        //
        // Criterion 1 is stated as a TOLERANCE, so the evidence for it is a
        // NUMBER and it has to be readable from the panel -- a coat that is
        // slightly rubbery looks exactly like a coat that is not.

        /// Grooms whose guides were simulated this frame.
        u32 GroomsSimulated = 0;
        /// Guides and guide particles actually solved. The cost of the feature,
        /// and the number a per-role budget is tuned against.
        u32 GuidesSimulated = 0;
        u32 GuidePointsSimulated = 0;
        /// Rendered strands that took their motion from a guide, and the ones
        /// that could not. A non-zero Unguided count is a fact about the
        /// AUTHORING -- a group groomed with no guide -- not about the frame.
        u32 StrandsSimulated = 0;
        u32 StrandsUnguided = 0;
        /// Particle-collider overlaps resolved on the last substep.
        u32 SimulationContacts = 0;
        /// Guides solved against their bind-pose shape because their root had no
        /// deformed frame. Non-zero is a fact about the BODY under this animation,
        /// and it looks identical to a guide that is simply not moving.
        u32 GuidesWithHeldRoots = 0;
        /// Fixed steps taken, summed over every simulated groom, and whether any
        /// of them dropped arrears. A coat permanently in arrears looks fine in
        /// a still frame and lags the body by a constant offset in motion, which
        /// reads as a binding error -- so the flag is how anyone finds it.
        u32 SimulationSteps = 0;
        bool SimulationStepsClamped = false;
        /// Grooms whose particles were RE-SEEDED this frame: a teleport, a
        /// budget change, the reset control, the first frame. Such a frame emits
        /// zero motion by design, so a count that never falls to zero is a coat
        /// that is being reset every frame and can therefore never move.
        u32 SimulationReseeds = 0;
        /// THE criterion-1 number: the worst |segment| / restLength over every
        /// simulated segment of every groom, and the tightest tolerance any of
        /// them declared. In contract while |ratio - 1| <= the tolerance.
        f32 WorstStretchRatio = 1.0f;
        f32 DeclaredStretchTolerance = 1.0f;
        /// Worst distance from a particle to its groomed rest position, world
        /// units. The rest-shape half: a coat that preserves length perfectly
        /// while hanging straight down has a stretch ratio of exactly 1.
        f32 WorstRestDeviation = 0.0f;

        // ── Fibre scattering (#1247) ─────────────────────────────────

        /// Grooms drawn with a fibre material this frame. The rest render
        /// #1246's neutral ramp, which is a legitimate state and not a
        /// failure — so this counter exists to tell the two apart from the
        /// panel rather than by looking at the picture.
        u32 GroomsLit = 0;
        /// The largest h-quadrature order any lit groom asked for. The pass's
        /// per-fragment cost is linear in it, so it is the one number that
        /// explains a strand pass that suddenly got expensive.
        u32 FibreSamplesPerFragment = 0;

        // ── Coat self-shadowing (#1248) ─────────────────────────────

        /// What each coat asked for, what it got, and why it is not what was
        /// asked. Acceptance criterion 3 wants the representation's resolution
        /// and update policy INSPECTABLE; these are counters rather than a log
        /// line so the panel can show them without the renderer having to have
        /// said anything.
        GroomCoatShadowStats CoatShadow;

        GroomCompositionStats Composition;

        // ── Representation LOD (#1252) ───────────────────────────────
        //
        // Which tier every groom is on, why it is not the tier its apparent
        // size selected, and what each tier cost in strands and GPU bytes.
        // Criterion 4 asks for cost and memory reported BY REPRESENTATION;
        // this is the frame-wide half of that, and the per-entity half is the
        // inspector's readout on GroomLodComponent.
        GroomLodStats Lod;

        // ── Scene shadow routing (#1323) ─────────────────────
        //
        // The CASTING half, measured by ShadowRenderPass and carried here
        // through the shared cache. It is in this struct rather than on the
        // shadow pass because the editor has exactly ONE groom readout, and a
        // coat that casts no shadow has to be explicable from the same panel
        // that asked for the shadow.
        GroomShadowCasterStats SceneShadow;

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

        /// Upper bound on the SHARED strand-buffer cache, in bytes. Exceeding
        /// it evicts least-recently-used entries at the TOP of a frame, never
        /// during one — an entry evicted while a draw was pending would leave
        /// the frame drawing from a freed buffer, and since #1323 two passes
        /// record draws against these buffers rather than one.
        void SetCacheBudgetBytes(u64 bytes) noexcept
        {
            if (m_Cache != nullptr)
            {
                m_Cache->SetBudgetBytes(bytes);
            }
        }

        /// The decision this pass WOULD make for `requested`, given the frame
        /// state it currently holds. Exposed so the editor's inspector and a
        /// test can ask without rendering — the reason a groom is not getting
        /// what it asked for should be answerable from the panel that sets it.
        [[nodiscard]] GroomCompositionDecision DecideComposition(GroomCompositionMode requested) const noexcept;

        /// The shared strand-geometry cache this pass draws from (#1323).
        /// Set once by RenderPipeline, which hands the SAME instance to
        /// ShadowRenderPass — see GroomStrandCache.h for why the cache cannot
        /// live in either pass.
        void SetStrandCache(GroomStrandCache* cache) noexcept
        {
            m_Cache = cache;
        }

      private:
        std::vector<GroomStrandRequest> m_Requests;
        GroomFrameState m_FrameState;
        GroomRenderStats m_Stats;

        /// The shared strand-geometry cache (#1323). NOT owned: RenderPipeline
        /// owns it and hands the same instance to ShadowRenderPass, because the
        /// shadow map is rasterised BEFORE this pass runs and a caster needs the
        /// buffers to already exist. Null in a unit-test harness that never went
        /// through RenderPipeline, in which case this pass draws nothing and
        /// says so once rather than building a second, private cache that the
        /// shadow pass could never see.
        GroomStrandCache* m_Cache = nullptr;
        bool m_WarnedNoCache = false;

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_ParamsUBO;
        Ref<Framebuffer> m_SceneFramebuffer;

        /// A 1x1x1 zero volume, bound whenever a draw has no coat volume of its
        /// own. A dangling sampler is undefined behaviour, not a zero read, so
        /// something valid is bound ALWAYS and the routing lane — never the
        /// binding — decides whether it is sampled. Same discipline, same
        /// reason, as VolumetricFogPass's density-volume placeholder.
        Ref<Texture3D> m_CoatPlaceholder;

        // Last reported dominant fallback, so the log line fires on a CHANGE
        // of reason rather than once per frame.
        GroomCompositionFallbackReason m_LastReportedReason = GroomCompositionFallbackReason::None;

        /// The last scene-shadow tally this pass logged (#1323), so the line
        /// fires on a CHANGE rather than once per frame.
        ///
        /// IT IS LOGGED AT ALL because the inspector cannot answer every
        /// question the counters exist for. "Which technique drew the coat" has
        /// to be readable from a HEADLESS or a scripted session too — an
        /// unwired technique is a zero next to a non-zero caster count, and a
        /// zero nobody can read is the gap
        /// virtual-geometry-into-a-second-shadow-technique.md is about.
        GroomShadowCasterStats m_LastReportedShadowStats;
    };
} // namespace OloEngine
