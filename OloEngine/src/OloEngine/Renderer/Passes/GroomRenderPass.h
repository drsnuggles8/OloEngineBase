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

        /// Whether this request's geometry is per-ENTITY rather than per-asset.
        ///
        /// A bound groom's vertices depend on a body's pose, so two entities
        /// sharing one groom asset cannot share one buffer — the same reason
        /// RayTracing::DeformedSurfaceCache keys its surfaces per entity. Named
        /// and public because the cache key and the rebuild path must agree
        /// about it, and a disagreement would hand one character's coat to
        /// another.
        [[nodiscard]] static bool IsDeformed(const GroomStrandRequest& request) noexcept;

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

            /// True when the buffers were created for repeated refills, which is
            /// what a bound groom needs: its vertices change every frame with
            /// the body's pose. A static entry's buffers are immutable and must
            /// never be handed to the refill path.
            bool Dynamic = false;

            // ── Coat self-shadowing (#1248) ─────────────────────────

            // The bake lives in the GEOMETRY cache, keyed the same way, because
            // it is a function of the same two things the geometry is: the
            // groom asset and the build settings. Keeping it anywhere else
            // would need a second eviction policy that could disagree with this
            // one about when a coat is still in use.

            /// The packed RGBA16F volume: xyz = the voxel's mean fibre
            /// direction times its coherence, w = fibre areal density. Null
            /// until the first successful bake.
            Ref<Texture3D> CoatVolume;
            /// The volume's object-space box, needed to map a shading point
            /// into it.
            glm::vec3 CoatBoundsMin{ 0.0f };
            glm::vec3 CoatBoundsMax{ 0.0f };
            /// Voxels on the longest axis of the bake actually resident.
            u32 CoatResolution = 0;
            /// The bake's REAL voxel size, carried rather than re-derived. Only
            /// the longest axis gets `CoatResolution` voxels, so
            /// extent/resolution is that axis's voxel size and nobody else's.
            f32 CoatVoxelSize = 0.0f;
            /// The width scale the bake was made at. It multiplies the cooked
            /// diameters and therefore the density stored, so it invalidates.
            f32 CoatWidthScale = 0.0f;
            /// GPU bytes the volume occupies.
            u64 CoatBytes = 0;
            /// The LOD step the resident bake was made at, the step the policy
            /// is currently ASKING for, and how many consecutive frames it has
            /// asked for it. The three together are the hysteresis state: a
            /// coat straddling a LOD boundary must not rebuild every frame,
            /// which is the flicker criterion's failure mode showing up as a
            /// counter before it shows up as a picture.
            u32 CoatLodStep = 0;
            u32 CoatRequestedLodStep = 0;
            u32 CoatLodStableFrames = 0;
            /// The cache tick the volume was last rebuilt at, so staleness is a
            /// number rather than an impression.
            u64 CoatBuiltTick = 0;

            /// The cache tick this entry's GEOMETRY bytes were last counted
            /// against a representation (#1252).
            ///
            /// Two unbound entities sharing one groom asset at one budget share
            /// ONE entry, so adding its bytes per DRAW reports the same
            /// allocation twice — and GroomLodStats::BytesByRepresentation is
            /// displayed as RESIDENT bytes, so it would overstate memory and
            /// could exceed CachedBytes, which is the one number it should
            /// never exceed. The strand COUNT stays per draw, because two
            /// entities really do draw those strands twice.
            u64 BytesCountedTick = 0;
            /// The mode the resident bake serves. A volume baked for one mode
            /// serves both volume modes — the isotropic arm simply does not
            /// read the direction channel — so this exists to detect a switch
            /// TO or FROM a per-light representation, not between the two
            /// volume modes.
            GroomCoatShadowTechnique CoatMode = GroomCoatShadowTechnique::None;
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

        /// Rebuilds `entry`'s coat volume if the request needs one and the
        /// resident bake is not already right. Returns the decision, so the
        /// caller records the reason rather than re-deriving it.
        [[nodiscard]] GroomCoatShadowDecision AcquireCoatVolume(const GroomStrandRequest& request, CacheEntry& entry,
                                                                u32& residentVolumes);

        /// Coat volumes currently held across the WHOLE cache, not just the
        /// ones drawn this frame. Counting live draws instead let the resident
        /// set exceed its cap: a groom that stopped being visible kept its
        /// volume, was not counted, and the next newcomer was still granted a
        /// slot — so alternating groups of eight coats retained more than
        /// kMaxResidentCoatVolumes textures indefinitely.
        [[nodiscard]] u32 CountResidentCoatVolumes() const noexcept;

        /// Frees the least-recently-used coat volume that is NOT in use this
        /// frame, so a newly visible coat can take its slot. Returns false when
        /// every resident volume belongs to a groom drawn this frame, which is
        /// the honest "budget really is full" case.
        bool ReclaimLeastRecentlyUsedCoatVolume();

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
    };
} // namespace OloEngine
