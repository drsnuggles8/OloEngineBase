#pragma once

#include "OloEngine/Containers/LinkedList.h"
#include <span>

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
// SHADOWS. It does not participate in the G-Buffer. The coat's shadow on
// ITSELF is #1248's density volume, marched per light in GroomStrand.glsl. The
// coat's shadow on the body and the scene, and the scene's shadow on the coat,
// are the engine's shadow techniques (#1323, re-landed by #1523): a groom with
// a GroomSceneShadowComponent is a caster family in ShadowRenderPass — which
// runs BEFORE this pass and so borrows this pass's geometry through
// AcquireShadowCaster — and its fragments sample the scene's shadow term at
// the coat's light-exit point. BeginFrame is what makes the two consumers of
// one cache agree about which frame it is.
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
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomStrandRequest.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class ComputeShader;
    class Framebuffer;
    class IndexBuffer;
    class Shader;
    class StorageBuffer;
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

    // When the "strand cache over budget" warning is worth a log line (#1431).
    //
    // Over budget with every entry in use is a STEADY STATE, not an event: a
    // scene whose coats outgrow the budget stays there every frame, and a line
    // per frame buried every other warning in the log. So the line is said on
    // the transition into the state, and again only when the overage has grown
    // materially since it was last said; dropping back under the budget re-arms
    // it. The per-frame number lives in GroomRenderStats::CacheOverBudgetBytes,
    // which is where a reader who wants it continuously looks.
    class GroomCacheBudgetWarningGate
    {
      public:
        /// "Materially": at least a quarter more than the overage last logged,
        /// and at least a MiB more, so a small overage creeping up a few
        /// kilobytes at a time does not re-log at every step.
        static constexpr u64 kGrowthNumerator = 5;
        static constexpr u64 kGrowthDenominator = 4;
        static constexpr u64 kMinGrowthBytes = 1024ull * 1024ull;

        /// Feeds one frame's overage (0 when within budget). True when this
        /// frame should log it.
        [[nodiscard]] bool Observe(u64 overBytes) noexcept
        {
            if (overBytes == 0)
            {
                m_LoggedOverBytes = 0;
                return false;
            }
            const bool entered = m_LoggedOverBytes == 0;
            const bool grew = overBytes >= m_LoggedOverBytes + kMinGrowthBytes &&
                              overBytes * kGrowthDenominator >= m_LoggedOverBytes * kGrowthNumerator;
            if (!entered && !grew)
            {
                return false;
            }
            m_LoggedOverBytes = overBytes;
            return true;
        }

        [[nodiscard]] u64 LoggedOverBytes() const noexcept
        {
            return m_LoggedOverBytes;
        }

      private:
        u64 m_LoggedOverBytes = 0;
    };

    // What the pass did, for the editor's panel and the PR's evidence.
    /// The groom caster family's tallies for one frame (#1323), written by
    /// ShadowRenderPass while it draws and read back through GroomRenderStats,
    /// so the editor has ONE groom readout that answers both directions.
    struct GroomShadowCasterStats
    {
        /// Requests whose GroomSceneShadowComponent asked them to cast.
        u32 GroomsAskedToCast = 0;
        /// Of those, the ones that had geometry and were submitted as casters.
        u32 GroomsCasting = 0;
        /// Of those, the ones that produced NO geometry — an empty build, or a
        /// budget that selected nothing. Non-zero is the honest answer to a
        /// coat that renders and casts nothing, and it is a different fault
        /// from "the family was never wired into this technique".
        u32 GroomsWithoutGeometry = 0;

        /// Draws issued, per technique. THREE COUNTERS RATHER THAN ONE, and
        /// that is the point: virtual-geometry-into-a-second-shadow-technique.md
        /// says a family reaches a technique only if somebody wired it there and
        /// that NOTHING detects the gap. A zero here, next to a non-zero
        /// GroomsCasting, is what detects it.
        u32 CascadeDraws = 0;
        u32 AtlasDraws = 0;
        u32 VirtualShadowLevelDraws = 0;
        /// Strand segments those draws rasterised, summed over every view, and
        /// what they would have rasterised casting every strand (#1533 E1). A
        /// view casts the share of a coat its width floor allows
        /// (GroomShadowCasterFraction), so the ratio is the saving; equal means
        /// every view cast whole coats.
        u64 SegmentsCast = 0;
        u64 SegmentsWhole = 0;
        /// Virtual Shadow Map page footprints invalidated for a moving or
        /// deforming coat this frame. A coat that moves while this stays zero
        /// leaves its old silhouette in the cached pages.
        u32 VirtualShadowInvalidations = 0;
        /// Local lights the Virtual Shadow Map served from its LAYER pool this
        /// frame while a groom was casting. Groom casters reach the VSM's clip
        /// levels (the sun) and the local-light ATLAS, not the layer pool — the
        /// same limit virtual geometry has — so each of these lamps casts no
        /// groom shadow. Non-zero is that gap, counted rather than silent; turning
        /// VSM LocalLights off routes lamps through the atlas, where grooms cast.
        u32 VirtualShadowLocalLightsWithoutGrooms = 0;

        /// Which directional technique owned the sun this frame, so a zero in
        /// one of the two directional counters can be read as "not this
        /// frame's technique" rather than as a hole.
        bool VirtualShadowMapActive = false;

        void Reset() noexcept
        {
            *this = GroomShadowCasterStats{};
        }
    };

    struct GroomRenderStats
    {
        u32 GroomsSubmitted = 0;
        u32 GroomsDrawn = 0;
        u32 StrandsDrawn = 0;
        u32 SegmentsDrawn = 0;
        u32 TrianglesDrawn = 0;
        /// GPU bytes the strand cache holds, the aggregate its budget and
        /// eviction work against: strand geometry, caster orders, deformation
        /// buffers AND coat volumes (Memory breaks it down; the coat volumes
        /// are in it, so never add CoatShadow.ResidentBytes on top).
        u64 CachedBytes = 0;
        /// What the pass holds, by allocation (#1533). LOGICAL bytes -- what the
        /// pass asked the RHI and the host allocator for -- not a driver-reported
        /// VRAM measurement: a driver pads, aligns and may keep staging copies.
        struct MemoryBreakdown
        {
            // GPU, inside the cache budget. Each shared rest stream counts once,
            // however many entities draw it. These five sum to CachedBytes.
            u64 StrandVertexBytes = 0; ///< rest streams' and CPU-path entries' vertex buffers
            u64 StrandIndexBytes = 0;  ///< their main index buffers
            u64 CasterIndexBytes = 0;  ///< the shadow casters' hashed-order index buffers
            u64 DeformBufferBytes = 0; ///< each GPU-deformed entity's frame buffer
            u64 CoatVolumeBytes = 0;   ///< coat-volume textures, every ring slot
            // CPU, OUTSIDE the budget: retained host arrays by allocated
            // CAPACITY (what they hold on to), not by the size in use.
            u64 CpuPoseSegmentBytes = 0;  ///< rest streams' pose segments + entries' bake subsets
            u64 CpuDeformMirrorBytes = 0; ///< each GPU-deformed entity's packed frame buffer, host side
            u64 CpuBakeInputBytes = 0;    ///< a posed bake's captured pose, card fibre scales and their tables
            u64 CpuRootTableBytes = 0;    ///< rest streams' root slot -> curve tables
            u64 CpuScratchBytes = 0;      ///< the pass's reusable bake, pose and CPU-stream scratch
            u32 RestStreams = 0;          ///< distinct rest streams counted
            u32 Entries = 0;              ///< cache entries counted

            [[nodiscard]] u64 GpuBytes() const noexcept
            {
                return StrandVertexBytes + StrandIndexBytes + CasterIndexBytes + DeformBufferBytes + CoatVolumeBytes;
            }
            [[nodiscard]] u64 CpuBytes() const noexcept
            {
                return CpuPoseSegmentBytes + CpuDeformMirrorBytes + CpuBakeInputBytes + CpuRootTableBytes +
                       CpuScratchBytes;
            }
        };
        MemoryBreakdown Memory;
        /// The strand-cache budget in force, and how far over it the cache
        /// stayed after this frame's eviction: non-zero means every resident
        /// entry was in use, nothing could be freed and every groom was still
        /// drawn. Reported every frame; the log line says it only on change.
        u64 CacheBudgetBytes = 0;
        u64 CacheOverBudgetBytes = 0;
        u32 CachedGrooms = 0;
        u32 CacheBuilds = 0;
        u32 CacheEvictions = 0;
        /// Rest streams whose pose segments were built this frame: once per
        /// stream, on the first posed bake that asks (#1533).
        u32 PoseSegmentBuilds = 0;

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
        /// Per-frame refills a deformed groom cost this frame: its frame buffer
        /// on the GPU path (#1427), its whole vertex stream on the CPU path. One
        /// per deformed groom is healthy; more means the geometry is being
        /// reallocated, not refilled.
        u32 DeformedRebuilds = 0;
        /// Deformed grooms the VERTEX SHADER moved this frame (#1427). The rest
        /// of GroomsDeformed took the CPU-deformed reference path — the
        /// RendererSettings::GroomGpuDeformation lever, or a binding the rest
        /// stream could not be built against.
        u32 GroomsGpuDeformed = 0;
        /// Grooms whose strands sampled the OPAQUE cascades at their own
        /// position this frame (#1533): the body they grow on shadows them in
        /// the sun. Zero with a coat receiving the sun's shadow means it fell
        /// back to the light-exit receiver (VSM owns the sun, or the copy could
        /// not be made) and its own body does not shadow it.
        u32 GroomsShadowedByOpaqueCascades = 0;
        /// The same for the local-light atlas: grooms drawn with its opaque
        /// copy bound, so a shadowed spot or point light is stopped by the body.
        u32 GroomsShadowedByOpaqueAtlas = 0;
        /// Of those, the grooms whose drawn roots the GPU evaluated (#1533 E1;
        /// compute/GroomRootFrames.comp), and how many roots that was. The rest
        /// packed CPU-evaluated roots.
        u32 GroomsRootsOnGpu = 0;
        u32 RootsEvaluatedOnGpu = 0;
        /// Bound grooms the GPU path REFUSED this frame because their
        /// deformation buffer could not be created; they were drawn through the
        /// CPU path. Non-zero is a device problem, and a count so it is visible.
        u32 GpuDeformationRefused = 0;
        /// Bytes the deformed grooms sent to the GPU this frame, and the CPU
        /// time spent producing them (#1427). The cost of a bound coat as two
        /// numbers, so "where does the frame go" is answerable from the panel
        /// rather than from a profiler capture. Build time covers packing the
        /// frame buffer on the GPU path and rebuilding the stream on the CPU
        /// path; the drawn-pose evaluation the coat bake needs is counted
        /// separately, in DeformedPoseMicroseconds.
        u64 DeformedUploadBytes = 0;
        u64 DeformedBuildMicroseconds = 0;
        u64 DeformedUploadMicroseconds = 0;
        /// CPU time spent producing the drawn pose the coat self-shadow bake
        /// reads (#1426). On the GPU path it is an evaluation of every drawn
        /// segment, paid only by a coat that asked for a self-shadow.
        u64 DeformedPoseMicroseconds = 0;
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
        /// Guides whose group's stiffness scale lifted them past the step's
        /// stability ceiling, so they were solved softer than authored (#1533).
        /// A fact about the authoring against the simulation rate.
        u32 GuidesStiffnessCapped = 0;
        /// Fixed steps taken, summed over every simulated groom, and whether any
        /// of them dropped arrears. A coat permanently in arrears looks fine in
        /// a still frame and lags the body by a constant offset in motion, which
        /// reads as a binding error -- so the flag is how anyone finds it.
        u32 SimulationSteps = 0;
        bool SimulationStepsClamped = false;
        /// CPU microseconds the solver took this frame, summed over grooms.
        u64 SimulationMicroseconds = 0;
        /// CPU microseconds the Scene spent choosing the drawn curves and
        /// evaluating their roots on the skinned surface, summed over grooms.
        u64 CurveSelectMicroseconds = 0;
        u64 RootEvaluateMicroseconds = 0;
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

        // ── Scene shadows (#1323) ───────────────────────────────────
        //
        // The CASTING half, written by ShadowRenderPass earlier in the same
        // frame through MutableSceneShadowStats. Reset by BeginFrame, never by
        // Execute, or the shadow pass's tallies would be wiped by the pass that
        // reports them.
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

        // Setup() declares nothing without a strand request (#1246).
        void AppendDeclarationInputs(RGDeclarationKey& key) const override
        {
            key.Add(!m_Requests.empty());
        }
        void Execute(RGCommandContext& context) override;

        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void ReleaseStaleFramebuffers(const std::function<bool(const Framebuffer*)>& isStale) override
        {
            RenderGraphNode::ReleaseStaleFramebuffers(isStale);
            ReleaseIfStale(m_SceneFramebuffer, isStale);
        }
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        /// Set once per frame by RenderPipeline, before Setup. READ IN PLACE,
        /// not copied (#1533 E1): the caller's storage must outlive this frame's
        /// Execute, which drops the view at its end. Renderer3D's published list
        /// does (it is recycled at the next BeginScene), and so do a test's
        /// locals. The copy it replaces deep-copied every request each frame,
        /// root transforms and all -- 26 MB a frame for the dog's 410k curves,
        /// undoing the pool Renderer3D keeps so the producer need not allocate.
        void SetRequests(std::span<const GroomStrandRequest> requests) noexcept
        {
            m_Requests = requests;
        }
        void SetRequests(const TArray64<GroomStrandRequest>& requests) noexcept
        {
            m_Requests = std::span<const GroomStrandRequest>(requests.GetData(), static_cast<sizet>(requests.Num()));
        }
        void SetFrameState(const GroomFrameState& state) noexcept
        {
            m_FrameState = state;
        }

        [[nodiscard]] const GroomRenderStats& GetStats() const noexcept
        {
            return m_Stats;
        }

        /// Starts a frame of this pass's cache (#1323): advances the cache tick
        /// and resets the stats, BEFORE any consumer acquires geometry.
        /// RenderPipeline calls it right after SetRequests. A render with NO
        /// requests starts no frame (the node is culled and nothing acquires, so
        /// the stats keep describing the last render that drew grooms, as they
        /// did before). AcquireShadowCaster and Execute start one themselves
        /// (StartFrame) when nobody has, so a test driving the pass directly
        /// still gets one tick per frame.
        ///
        /// WHY IT EXISTS. The cache has two consumers in one frame now — the
        /// groom caster family in ShadowRenderPass, which runs first, and this
        /// pass's own draw. A tick advanced inside Execute would give the shadow
        /// pass LAST frame's tick: a bound coat's deformation would be skipped
        /// as "already uploaded this frame" and its shadow cast from the
        /// previous pose, and the shadow pass's counters would be wiped by the
        /// Reset at the top of Execute.
        void BeginFrame();

        /// What the groom caster family draws for one request (#1323). All of it
        /// is owned by this pass and stays valid until this pass's next frame:
        /// eviction happens only at the end of Execute, after both consumers.
        struct ShadowCasterGeometry
        {
            RHI::ResourceHandle Vao{};
            u32 IndexCount = 0;
            /// The caster order's runs (GroomCasterOrder::Runs, #1533): `Vao`
            /// then draws the strands run by run, each run in a hashed order, and
            /// a view casts a prefix of each (DecideGroomCasterRun). EMPTY when
            /// the stream has no caster order -- `Vao` is then the stream's own
            /// and is cast whole.
            std::span<const GroomCasterRun> CasterRuns{};
            /// The coat's box in GROOM OBJECT SPACE in THIS pose — the posed
            /// roots padded by the longest strand's reach for a GPU-deformed
            /// coat, whose stream bounds are bind-local and mean nothing here.
            glm::vec3 BoundsMin{ 0.0f };
            glm::vec3 BoundsMax{ 0.0f };
            bool BoundsValid = false;
            /// The GPU deformation (#1427) the strand draw uses: this entity's
            /// frame buffer, or the zeroed placeholder at mode 0 (a final stream).
            StorageBuffer* DeformBuffer = nullptr;
            glm::ivec4 DeformModes{ 0 };
            glm::ivec4 DeformBases{ 0 };
        };

        /// The geometry `request` casts with, found, refilled or built exactly as
        /// Execute would find it, and at most once per frame for a GPU-deformed
        /// coat. RENDER THREAD ONLY and outside any parallel-recording region:
        /// it can create buffers (ADR 0011 amendment (92) rule 7). False when the
        /// request produced no geometry.
        [[nodiscard]] bool AcquireShadowCaster(const GroomStrandRequest& request, ShadowCasterGeometry& out);

        /// The caster family's tallies, for ShadowRenderPass to write while it
        /// draws. Valid between BeginFrame and the end of Execute.
        [[nodiscard]] GroomShadowCasterStats& MutableSceneShadowStats() noexcept
        {
            return m_Stats.SceneShadow;
        }

        /// Read-only memory-report rows (#1342): shared rest streams once each, per-entity
        /// deformation state, coat-volume rings and the CPU pose/shadow storage. Defined in
        /// GroomRenderPassMemory.cpp.
        void AppendMemoryCapacityRows(TArray<MemoryCapacityRow>& rows) const;

        /// Upper bound on the strand-buffer cache, in bytes. Exceeding it
        /// evicts least-recently-used entries at the END of a frame, never
        /// during one — an entry evicted while its draw was pending would
        /// leave the frame drawing from a freed buffer.
        void SetCacheBudgetBytes(u64 bytes) noexcept
        {
            m_CacheBudgetBytes = bytes;
        }
        [[nodiscard]] u64 GetCacheBudgetBytes() const noexcept
        {
            return m_CacheBudgetBytes;
        }

        /// When a DEFORMED coat's volume is rebuilt (#1426). Engine-wide rather
        /// than per groom: it trades CPU time against shadow lag, which is a
        /// budget decision like the cache's, not an authored look. Sanitised on
        /// the way in — see GroomCoatShadow::SanitizeCoatRebakePolicy.
        void SetCoatRebakePolicy(const GroomCoatShadow::CoatRebakePolicy& policy) noexcept
        {
            m_CoatRebakePolicy = GroomCoatShadow::SanitizeCoatRebakePolicy(policy);
        }
        [[nodiscard]] const GroomCoatShadow::CoatRebakePolicy& GetCoatRebakePolicy() const noexcept
        {
            return m_CoatRebakePolicy;
        }

        /// Whether a bound coat is deformed by the vertex shader (true, #1427)
        /// or rebuilt and re-uploaded on the CPU every frame (false — the path
        /// that existed before, kept as the reference the GPU path is compared
        /// against). RendererSettings::GroomGpuDeformation, handed over per
        /// frame like the rebake policy.
        void SetGpuDeformationEnabled(bool enabled) noexcept
        {
            m_GpuDeformation = enabled;
        }
        [[nodiscard]] bool IsGpuDeformationEnabled() const noexcept
        {
            return m_GpuDeformation;
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

        /// The cache key a request gets on the path `gpuDeformation` selects.
        /// A deformed groom's key also carries its BINDING and the path: the
        /// GPU path's rest stream is written in the binding's bind frames, so a
        /// swapped binding is different geometry, and the two paths encode the
        /// same sixteen floats differently, so flipping the lever must never
        /// serve one path the other's stream.
        [[nodiscard]] static u64 CacheKey(const GroomStrandRequest& request, bool gpuDeformation) noexcept;

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
        /// One stream's strands in the shadow caster's order (#1533 E1): the
        /// stream's OWN vertex buffer under a second index buffer, so a shadow
        /// view casting a subset draws fewer indices and touches no extra vertex
        /// memory. Built with the stream, never refilled: a deformed coat moves
        /// its vertices and never its topology.
        struct GroomCasterStream
        {
            Ref<VertexArray> Array;
            Ref<IndexBuffer> Indices;
            /// One run per group (GroomCasterOrder::Runs), covering the index
            /// buffer end to end.
            std::vector<GroomCasterRun> Runs;
            /// The whole stream's length-weighted mean radius, for the log.
            f32 MeanRadius = 0.0f;
            /// GPU bytes of the index buffer, counted with the stream's.
            u64 Bytes = 0;
        };

        /// The caster order of a stream just built, one run per group of
        /// `strands` (the builder's caster summaries), or an empty one when the
        /// stream gave none (the caster then casts the stream whole).
        [[nodiscard]] static GroomCasterStream BuildCasterStream(const Ref<VertexBuffer>& vertexBuffer,
                                                                 std::span<const GroomStrandVertex> vertices,
                                                                 std::span<const u32> indices,
                                                                 std::span<const u32> strandFirstIndex,
                                                                 std::span<const GroomCasterStrand> strands);

        /// A bound coat's REST stream (#1427), shared by every entity that wears
        /// the same groom at the same budget, coat and binding. Unlike the frame
        /// buffer, nothing in it depends on the entity's pose, so a herd of
        /// animals sharing one cooked coat holds one copy instead of one each.
        /// Held by Ref from the per-entity entries and by the pass's map, and
        /// dropped once only the map holds it.
        struct GroomRestStream : public RefCounted
        {
            Ref<VertexArray> Array;
            Ref<VertexBuffer> Vertices;
            Ref<IndexBuffer> Indices;
            GroomStrandBuildSettings Settings;
            GroomStrandMeshStats Stats;
            /// The shadow caster's order over the same vertices (#1533 E1).
            GroomCasterStream Caster;
            /// GPU bytes of the three buffers, counted once against the cache.
            u64 Bytes = 0;
            /// The binding the bind frames came from, held so a binding RELOADED
            /// under the same handle (a re-bind writes the same file) is a
            /// different object and rebuilds the stream. The key has the handle;
            /// this has the identity. The CPU path never needed either, because
            /// it read the live binding every frame.
            Ref<GroomBindingAsset> Binding;
            /// Root slot -> base curve, the order every frame buffer over this
            /// stream is packed in.
            std::vector<u32> RootCurves;
            /// The centrelines a POSED coat bake evaluates (#1426), built on the
            /// first AcquireDrawnPose of any entity sharing the stream, straight
            /// from the walk (BuildGroomRestPoseSegments), and kept: a coat baked
            /// at rest never reads them and never pays for them (#1533).
            std::vector<GroomRestPoseSegment> PoseSegments;
            bool PoseSegmentsBuilt = false;
            /// The farthest any drawn point sits from its own root, and the widest
            /// cooked radius, in bind-local units (#1323). The stream's points
            /// ARE root-relative, so both are properties of the stream alone, and
            /// they are what bounds a posed coat from its posed roots: nothing the
            /// vertex shader does moves a point further from its root than the
            /// strand is long, save the guide displacements, which the caller
            /// pads for separately.
            f32 MaxReach = 0.0f;
            f32 MaxRadius = 0.0f;
            /// The cache tick these bytes were last reported at (#1252), so a
            /// shared stream counts once per frame however many draw it.
            u64 BytesCountedTick = 0;
        };

        // One groom's GPU geometry, keyed by asset handle.
        /// Coat volumes kept per coat (#1445): enough that a coat rebaking every
        /// frame always finds a slot no recording still to be submitted reads.
        static constexpr u32 kCoatVolumeRing = 3;

        struct CacheEntry
        {
            Ref<VertexArray> Array;
            Ref<VertexBuffer> Vertices;
            Ref<IndexBuffer> Indices;
            GroomStrandBuildSettings Settings;
            GroomStrandMeshStats Stats;
            /// The shadow caster's order (#1533 E1) of an entry that owns its
            /// stream. A GPU-deformed entry casts its rest stream's instead.
            GroomCasterStream Caster;
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
            /// until the first successful bake. Always CoatRing[CoatSlot]'s
            /// texture.
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
            /// The coat authoring (GroomCoatDigest) an un-posed bake was made
            /// with (#1533): a rest or static volume is built from the drawn walk,
            /// which the coat shapes, so re-authoring the coat rebuilds it.
            u64 CoatBakedCoatDigest = 0;
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

            // ── A deformed coat (#1426) ─────────────────────────────

            /// The drawn pose the resident volume was baked from: one centreline
            /// midpoint per segment (GroomCoatShadow::CaptureCoatPose). Empty
            /// for a bake of the asset's rest curves; WHICH kind of bake is
            /// resident is CoatBakedFromPose's job, not this vector's emptiness.
            std::vector<glm::vec3> CoatBakedPose;
            /// The resident volume was baked from a DRAWN pose (true) or from
            /// the asset's rest curves (false). Stated, not inferred from
            /// CoatBakedPose being empty; cleared with the volume.
            bool CoatBakedFromPose = false;
            /// How far the drawn coat is from CoatBakedPose THIS frame, in
            /// voxels, after any rebake. Zero for an undeformed coat.
            f32 CoatDriftVoxels = 0.0f;

            /// A CARD-tier entry's fibre scale per drawn segment (#1428), from
            /// GroomCardFibreScales: per coat group, the base groom's fibre
            /// area over the card level's. A card carries the width its members
            /// COVER on screen, a different fraction of their fibre in every
            /// group, and the self-shadow volume stores fibre, not coverage.
            /// Applied where the pose is formed, before the bake subset.
            /// Empty on the strand tier.
            std::vector<f32> CoatFibreScales;
            /// The level CoatFibreScales was measured from.
            const GroomLodLevel* CoatFibreScalesSource = nullptr;
            /// A scale list that did not match the pose's length was reported.
            bool CoatFibreMismatchReported = false;

            // ── The bake subset (#1445) ─────────────────────────────

            /// Segments in the FULL drawn pose, the voxels the last FULL bake
            /// occupied, and the resolution it ran at: what
            /// GroomCoatShadow::CoatBakeSubsetStride turns into CoatBakeStride,
            /// the stride the NEXT pose is taken at. A resolution change
            /// re-measures the occupancy with one full bake, so a coat first
            /// baked coarse at range is not held at a coarse bake's stride up
            /// close.
            u64 CoatFullPoseSegments = 0;
            u32 CoatFullOccupiedVoxels = 0;
            u32 CoatOccupancyResolution = 0;
            u32 CoatBakeStride = 1;
            /// The stride THIS frame's pose was taken at. 1 means the bake that
            /// follows is a full one, and it is the one that measures.
            u32 CoatPoseStride = 1;
            /// The stride the RESIDENT volume was baked at. A different stride
            /// is a different segment set, so it is a rebuild, never drift.
            u32 CoatBakedStride = 1;
            /// The GPU path's subset of the rest stream's pose segments at
            /// CoatPoseSubsetStride, radii already scaled. Rebuilt when the
            /// stride or the stream changes; empty at a stride of 1.
            std::vector<GroomRestPoseSegment> CoatPoseSubset;
            u32 CoatPoseSubsetStride = 0;
            const GroomRestStream* CoatPoseSubsetSource = nullptr;

            // ── The volume ring (#1445) ─────────────────────────────
            //
            // A walking coat rebakes every frame. It used to create a new
            // texture each time and drop the old one into the deferred-deletion
            // queue, which on eight coats was 32 MB of new textures a frame plus
            // every in-flight frame's worth waiting to be freed, none of it in
            // m_CacheBytes. Now it rewrites a ring slot in place -- one that no
            // recording still to be submitted can read (see BakeCoatVolume) --
            // and every slot is counted in CoatBytes.
            struct CoatVolumeSlot
            {
                Ref<Texture3D> Texture;
                /// The FRAME a draw last bound it in (GroomFrameState::
                /// FrameIndex, shared by every camera's Execute in one frame);
                /// meaningless until Bound.
                u32 LastBoundFrame = 0;
                bool Bound = false;
            };
            std::array<CoatVolumeSlot, kCoatVolumeRing> CoatRing;
            u32 CoatSlot = 0;

            // ── GPU deformation (#1427) ─────────────────────────────

            /// `Array` is the SHARED rest stream's (BuildGroomStrandRestMesh),
            /// which the vertex shader deforms from `DeformGpu` — not a final
            /// stream. Set only by the GPU path; the stream is immutable and
            /// shared, and only this entity's frame buffer is refilled. `Bytes`
            /// then counts the frame buffer alone; the stream's bytes are the
            /// shared stream's, counted once.
            bool GpuDeformed = false;
            Ref<GroomRestStream> Rest;
            /// The frame buffer, CPU side: the bytes the GPU reads, which the
            /// coat bake evaluates the drawn pose from.
            GroomDeformBuffer DeformCpu;
            /// The same bytes on the GPU, at SSBO_GROOM_DEFORMATION.
            Ref<StorageBuffer> DeformGpu;
            /// The cache tick the frame buffer was last packed and uploaded at
            /// (#1323). The shadow pass and the strand pass both acquire a
            /// casting coat in one frame; the second acquire finds this equal to
            /// the tick and draws the bytes the first uploaded.
            u64 DeformUploadedTick = 0;
            /// The influence table the static guide weights were written from.
            /// Held so a coat that starts or stops being simulated, or whose
            /// asset re-derives its table, rewrites them rather than reading a
            /// freed table's weights.
            Ref<GroomGuideInfluenceTable> DeformWeightsFrom;
            /// The bound surface the GPU root evaluation's static regions were
            /// written from (GroomStrandRequest::GpuRootSurfaceKey); a different
            /// one relays the buffer out.
            u64 RootSkinKey = 0;
            /// Where the kernel's roots can be, for the posed box while the
            /// CPU holds no drawn root (see BuildGroomRootBoneBounds).
            GroomRootBoneBounds RootBoneBounds;
            /// The root slots the kernel evaluates (WriteSurfaceSkin); the rest
            /// of the layout's roots are held at rest.
            u32 RootsReached = 0;
        };

        /// The groom's geometry for this frame: cached, refilled or built. For a
        /// deformed groom this is also where its per-frame deformation is
        /// produced — the frame buffer on the GPU path, the whole stream on the
        /// CPU path (left in m_DeformedVertices for the coat bake).
        [[nodiscard]] CacheEntry* AcquireGeometry(const GroomStrandRequest& request);
        /// The GPU path's half of AcquireGeometry: returns null when the rest
        /// stream cannot be built for this request, so the caller can take the
        /// CPU path instead.
        [[nodiscard]] CacheEntry* AcquireGpuDeformedGeometry(const GroomStrandRequest& request, u64 key);
        /// The shared rest stream this request draws, found or built. Null when
        /// it cannot be built (a binding that does not span the base groom, or
        /// a selection that emits nothing).
        [[nodiscard]] Ref<GroomRestStream> AcquireRestStream(const GroomStrandRequest& request);
        /// The rest stream's key: CacheKey without the ENTITY, because nothing
        /// in the stream depends on it, plus the binding handle.
        [[nodiscard]] static u64 RestStreamKey(const GroomStrandRequest& request) noexcept;
        /// Drops every rest stream no entity entry still holds.
        void PruneRestStreams();
        /// Packs and uploads this frame's deformation into `entry`'s buffer,
        /// (re)creating the buffer when the request outgrew it.
        void UploadDeformation(const GroomStrandRequest& request, CacheEntry& entry);
        /// The GPU root evaluation's kernel, loaded on first use; false when it cannot run.
        [[nodiscard]] bool EnsureRootFrameKernel();
        /// Records GroomRootFrames.comp over `entry`'s drawn roots (#1533 E1).
        void DispatchRootFrames(const GroomStrandRequest& request, CacheEntry& entry, bool usePreviousPose);
        /// The drawn pose of a DEFORMED groom, as centrelines, for the coat bake
        /// (#1426). Evaluated from the frame buffer on the GPU path and read
        /// from the rebuilt stream on the CPU path. Empty for an undeformed
        /// groom, and for a deformed one whose pose could not be produced.
        [[nodiscard]] std::span<const GroomCoatShadow::CoatSegment> AcquireDrawnPose(const GroomStrandRequest& request,
                                                                                     CacheEntry& entry);
        /// The cache's numbers into m_Stats -- the budget aggregate and its
        /// breakdown -- at every exit Execute takes (#1533).
        void PublishCacheStats();
        /// Gives back each scratch group that has idled kScratchIdleFrames
        /// frames, and the pose scratch at once on a frame with no bound groom.
        void ReleaseIdleScratch(bool noDeformedGroom);
        /// The breakdown, walked from the cache: each rest stream in
        /// m_RestStreams once, each entry once. Its GPU categories sum to
        /// m_CacheBytes, which tests hold it to.
        [[nodiscard]] GroomRenderStats::MemoryBreakdown CollectMemoryBreakdown() const;
        /// The coat's box in groom object space in THIS pose (#1323): the posed
        /// roots padded by the stream's reach for a GPU-deformed coat, the build
        /// bounds otherwise. False when there is nothing to bound.
        [[nodiscard]] bool PosedObjectBounds(const GroomStrandRequest& request, const CacheEntry& entry,
                                             glm::vec3& outMin, glm::vec3& outMax) const;
        /// Resets the stats and advances the tick, unconditionally: the lazy
        /// start AcquireShadowCaster and Execute take when no BeginFrame did.
        void StartFrame();
        void EvictToBudget();

        // A view of the frame's requests, never an owner: see SetRequests.
        std::span<const GroomStrandRequest> m_Requests;
        GroomFrameState m_FrameState;
        GroomRenderStats m_Stats;

        std::unordered_map<u64, CacheEntry> m_Cache;
        /// Shared rest streams (#1427), keyed by RestStreamKey.
        std::unordered_map<u64, Ref<GroomRestStream>> m_RestStreams;
        // 1 GiB (#1533): above one hero coat. The showcase dog alone holds
        // 651 MiB in use (Dog_Cost.txt's memory table, 474 MiB of it strand
        // vertices), so the 256 MiB this started at could evict nothing and
        // the scene logged the over-budget warning every time it opened. The
        // budget bounds what coats NOT drawn this frame may keep; it never
        // stops a coat being drawn.
        u64 m_CacheBudgetBytes = 1024ull * 1024ull * 1024ull;
        u64 m_CacheBytes = 0;
        GroomCacheBudgetWarningGate m_CacheBudgetWarning;

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
        ///
        /// `drawnPose` is this frame's drawn pose for a bound groom (empty
        /// otherwise); see AcquireDrawnPose. `bakeAtRest` bakes a bound groom
        /// from its rest curves instead, once (#1533): its draw looks the
        /// volume up at each fragment's bind point.
        [[nodiscard]] GroomCoatShadowDecision AcquireCoatVolume(const GroomStrandRequest& request, CacheEntry& entry,
                                                                u32& residentVolumes,
                                                                std::span<const GroomCoatShadow::CoatSegment> drawnPose,
                                                                bool bakeAtRest);

        /// Bakes `segments` into `entry`'s coat volume. The one place a volume
        /// is created, for the rest-curve bake and the drawn-pose bake alike, so
        /// the two cannot disagree about packing, format or byte accounting.
        /// Returns false, leaving the resident volume untouched, when the bake
        /// produced nothing.
        /// Sets CoatBakeStride from the current rebake policy and the coat's
        /// measured occupancy (#1445).
        void RefreshCoatBakeStride(CacheEntry& entry) const noexcept;

        /// GroomCardFibreByGroup for this request's LOD level, measured once
        /// per level and shared by every entry and stream that draws it.
        const std::vector<f32>& CardFibreByGroup(const GroomStrandRequest& request);

        /// The entry's CoatFibreScales for this request's LOD level, measured
        /// once per level; empty on the strand tier.
        const std::vector<f32>& CardFibreScales(const GroomStrandRequest& request, CacheEntry& entry);

        /// Multiplies a card level's rest pose by GroomCardFibreScales; a
        /// no-op on the strand tier. Every GPU-path pose goes through it.
        void ScaleRestPoseToCardFibre(const GroomStrandRequest& request, std::vector<GroomRestPoseSegment>& pose);

        /// Per LOD level, the per-group fibre table (#1428). Keyed on the level
        /// AND its size, so a recooked level at a reused address is measured
        /// again rather than served its predecessor's factors.
        struct CardFibreTable
        {
            const GroomLodLevel* Level = nullptr;
            u32 Curves = 0;
            sizet Points = 0;
            std::vector<f32> ByGroup;
        };
        std::vector<CardFibreTable> m_CardFibreTables;

        bool BakeCoatVolume(CacheEntry& entry, std::span<const GroomCoatShadow::CoatSegment> segments,
                            u32 resolution, bool ring, u32* outOccupiedVoxels = nullptr);

        /// The CPU path's rebuilt stream for the groom being processed, reused
        /// across draws so a bound coat does not allocate it twice. Empty on the
        /// GPU path, which never builds one.
        std::vector<GroomStrandVertex> m_DeformedVertices;
        /// The drawn pose handed to the coat bake, reused across draws.
        std::vector<GroomCoatShadow::CoatSegment> m_DrawnPose;
        /// The CPU path's FULL drawn pose, before the bake subset (#1445).
        std::vector<GroomCoatShadow::CoatSegment> m_DrawnPoseFull;
        /// The bake's scratch, reused across bakes so a coat that rebakes every
        /// frame does not allocate its segments, its volume and its packed
        /// texels every frame (#1445).
        std::vector<GroomCoatShadow::CoatSegment> m_CoatSegments;
        std::vector<GroomRestCentreline> m_RestCentrelines;
        GroomCoatShadow::DensityVolume m_CoatVolumeScratch;
        std::vector<u16> m_CoatPackHalf;
        std::vector<f32> m_CoatPackFloat;
        /// THE SCRATCH IS GIVEN BACK WHEN IT IDLES (#1533). Each group keeps the
        /// capacity of the largest job it served, so a coat that rebakes or
        /// re-poses every frame never reallocates; a coat that did its job once
        /// -- a rest bake, a static coat's one bake -- would otherwise pin that
        /// peak for the session, outside every budget. A group unused for this
        /// many frames is released (see Execute's end).
        static constexpr u64 kScratchIdleFrames = 120;
        u64 m_BakeScratchUsedTick = 0; ///< m_CoatSegments, m_RestCentrelines, the volume and its packing
        u64 m_PoseScratchUsedTick = 0; ///< m_DrawnPose(Full), m_DeformedVertices, m_CpuRootScratch

        GroomCoatShadow::CoatRebakePolicy m_CoatRebakePolicy;
        bool m_GpuDeformation = true;

        /// Bound at SSBO_GROOM_DEFORMATION for every draw that reads no frame
        /// buffer. The binding is shared with the terrain VT under a
        /// rebound-per-use rule, so a draw that left it alone would inherit
        /// whatever the last user bound — and on Vulkan a declared block with no
        /// occupant is a logged error, not a zero read.
        Ref<StorageBuffer> m_DeformPlaceholder;
        /// Latched so a device that cannot create deformation buffers logs once.
        bool m_ReportedDeformBufferFailure = false;

        /// The GPU root evaluation (#1533 E1): its kernel and its params block,
        /// created on first use. A kernel that fails to load is said once and
        /// the drawn roots are then evaluated on the CPU instead.
        Ref<ComputeShader> m_RootFrameShader;
        Ref<UniformBuffer> m_RootFrameParams;
        bool m_RootFrameShaderFailed = false;
        /// The drawn roots evaluated on the CPU for a request that left them to
        /// the GPU, when the pass takes a CPU path after all.
        TArray<GroomRootTransform> m_CpuRootScratch;

        /// Set by BeginFrame, consumed at the top of Execute (#1323). While it
        /// is set a lazy BeginFrame is a no-op, so the shadow pass and the
        /// strand pass share one tick; once Execute has consumed it the next
        /// consumer starts the next frame.
        bool m_FrameBegun = false;
        /// The caster tallies last written to the log, so the line fires on a
        /// change rather than every frame.
        GroomShadowCasterStats m_LastReportedShadowStats;

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
        /// Bald grooms (TemporalResolveUnavailable) last reported, latched on
        /// their own so the warning fires whatever reason dominates (#1429).
        u32 m_LastReportedBaldGrooms = 0;
    };
} // namespace OloEngine
