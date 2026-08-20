#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/DDGI/DDGICommon.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace OloEngine
{
    // @brief Per-frame description of the active Realtime/Hybrid DDGI probe
    // field (issues #632 / #707). Submitted by the Scene before render-graph
    // execution; consumed by DDGIProbeUpdatePass::Execute.
    //
    // TWO MODES, one structure:
    //
    //   Cascaded == false — the AUTHORED single volume of #632.
    //     BoundsMin/BoundsMax/Resolution come from a LightProbeVolumeComponent,
    //     the field is one cascade, the blend band is 0 and sparsity is off. The
    //     atlas layout, the sampler behaviour and the capture schedule are all
    //     bit-identical to the pre-#707 pass — deliberately, so an authored
    //     volume keeps its goldens and its reference-path-tracer parity.
    //
    //   Cascaded == true — the camera-centred clipmap of #707.
    //     CascadeCount grids centred on the camera, cascade N at twice cascade
    //     N-1's spacing, blended across CascadeBlendBand. BoundsMin/BoundsMax
    //     are ignored; Resolution is the PER-CASCADE probe count. This is the
    //     path that needs no authored volume at all.
    struct DDGIVolumeDesc
    {
        glm::vec3 BoundsMin{ -10.0f };
        glm::vec3 BoundsMax{ 10.0f }; // ABSOLUTE world space (authored mode only)
        glm::ivec3 Resolution{ 4, 2, 4 };
        i32 HitCacheTexels = 16; // already snapped to 8/16/32
        f32 Hysteresis = 0.9f;   // [0, 0.98]
        f32 Intensity = 1.0f;
        f32 SelfShadowBias = 0.3f;
        i32 CaptureBudget = 4;       // probes per frame, >= 1
        i32 RelightBudget = 0;       // probes per frame, 0 = all (mapped onto UpdateRate — see below)
        u8 Mode = 0;                 // 0 none/baked, 1 Realtime, 2 Hybrid
        bool BakedAvailable = false; // hybrid: baked SH asset exists

        // --- Issue #707 ---------------------------------------------------
        bool Cascaded = false;
        i32 CascadeCount = 1;
        f32 BaseProbeSpacing = DDGI::kDefaultBaseProbeSpacing; // cascade 0 spacing, world units
        f32 CascadeBlendBand = 0.0f;                           // 0 = hard bounds (authored behaviour)

        // Sparsity: relight only probes a shaded pixel or another probe's hit
        // point asked for. OFF in authored mode, where the whole point of the
        // volume is that the author placed it around what matters.
        bool SparsityEnabled = false;

        // Variable update rate: relight 1-in-N live probes per frame. PGI
        // defaults to 1-in-8. This SUPERSEDES the RelightBudget row-scissor
        // throttle of #632 — Scene maps a non-zero RelightBudget onto the
        // nearest supported rate, so the authored knob keeps working and there
        // is only ONE mechanism deciding which probes relight. Two throttles
        // interacting was the alternative, and their product is not a rate
        // anyone can reason about.
        i32 UpdateRateDivisor = 1;

        // World radius around the camera whose probes are requested
        // unconditionally, every frame. The floor under sparsity: a feature
        // whose failure mode is "no GI and no error" needs one. 0 disables.
        f32 CameraSeedRadius = 0.0f;
    };

    // @brief One mesh caster for the amortized DDGI probe capture. Mirrors
    // ShadowMeshCaster: raw VAO + submesh range + ABSOLUTE world transform,
    // plus the minimal material data the capture mini-G-buffer needs.
    struct DDGIMeshCaster
    {
        RHI::ResourceHandle vaoID{};
        u32 indexCount = 0;
        u32 baseIndex = 0;
        glm::mat4 transform{ 1.0f }; // ABSOLUTE world
        BoundingBox worldBounds = NoBounds;
        glm::vec4 baseColor{ 1.0f };           // material base color factor
        RHI::ResourceHandle albedoTextureID{}; // invalid = fall back to the white texture
        bool twoSided = false;
    };

    // @brief Realtime DDGI probe update pass (issues #632 / #707,
    // docs/adr/0007-ddgi-hit-point-cache-gather.md).
    //
    // Owns ALL DDGI GPU state and performs request -> capture -> relocate ->
    // relight -> blend in one render-graph node:
    //
    //   1. Per-probe maintenance compute: cascade-shift invalidation, the
    //      camera-neighbourhood request seed, and the live/active counters.
    //   2. Request computes: shaded screen pixels request probes, then live
    //      probes' cached hit points request probes (ONE indirection deep).
    //   3. Amortized capture (CaptureBudget probes/frame): rasterize the
    //      submitted casters into a 3x2 cube-face mini-G-buffer around each
    //      scheduled probe (ShadowRenderPass-style dedicated pass, NOT
    //      Scene::RenderScene3D), then resample the six faces into the probe's
    //      octahedral hit-point cache (albedo / normal / distance / flag).
    //   4. GPU relocation + classification compute for the captured probes
    //      (issue #707: PGI's spring-force term, and NO readback).
    //   5. Visibility (Chebyshev mean/mean^2) blend for captured probes only —
    //      hit distances change only at capture time (ADR divergence).
    //   6. Per-frame relight of every LIVE, SCHEDULED probe's cached hit points
    //      with current shadowed direct lighting (MultiLight UBO + CSM + shadow
    //      atlas, diffuse-only) plus the previous frame's probe irradiance
    //      (infinite bounce).
    //   7. Cosine convolution of the relit radiance into the irradiance atlas
    //      under adjusted-hysteresis EMA.
    //   8. Publish: atlases bound at TEX_DDGI_* (56/64/58), DDGI UBO (51)
    //      uploaded Enabled=1.
    //
    // The atlases are consumed OUTSIDE the graph's resource tracking (engine
    // sampler slots, same pattern as VolumetricFogPass's integrated volume),
    // so the pass is flagged NeverCull.
    //
    // NO GPU->CPU READBACK HAPPENS IN Execute (issue #707 acceptance criterion
    // 3). Everything the CPU needs for capture scheduling it derives
    // analytically from the camera and the cascade layout; everything the GPU
    // decides stays on the GPU. The one readback that remains is
    // ReadbackProbeDiagnostics(), which only the editor inspector, the MCP
    // tools and the tests call — never the frame.
    class DDGIProbeUpdatePass : public RenderGraphNode
    {
      public:
        DDGIProbeUpdatePass();
        ~DDGIProbeUpdatePass() override;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;

        [[nodiscard]] bool IsReadyForExecution() const noexcept override;

        // ---------------------------------------------------------------------
        // Per-frame submission API — called before graph execution.
        // ---------------------------------------------------------------------

        // Scene found an active Realtime/Hybrid volume this frame (or the
        // cascaded path is enabled and needs no volume at all).
        void SubmitVolume(const DDGIVolumeDesc& desc);

        // True when a volume was submitted this frame && captures are pending
        // (a caster traversal is only worth paying for when the capture stage
        // will actually consume the list).
        [[nodiscard]] bool WantsCasters() const;

        void AddMeshCaster(const DDGIMeshCaster& caster);

        // ---------------------------------------------------------------------
        // Consumption info.
        // ---------------------------------------------------------------------

        // Current (post-blend) — null until the first run.
        [[nodiscard]] RHI::ResourceHandle GetIrradianceAtlasID() const;
        [[nodiscard]] RHI::ResourceHandle GetVisibilityAtlasID() const;
        [[nodiscard]] RHI::ResourceHandle GetProbeDataTextureID() const;
        // Explicit-ping accessors (issue #607): Setup() imports BOTH ping-pong
        // atlases into the render graph under stable per-ping names — "which
        // ping is current" flips every blended frame, and chasing the flip
        // with a single "current" import would invalidate the BuildFrameGraph
        // fingerprint cache every frame. The pipeline fingerprint hashes these
        // ids so atlas (re)creation triggers the rebuild that re-imports them.
        [[nodiscard]] RHI::ResourceHandle GetIrradianceAtlasID(u32 pingIndex) const;
        [[nodiscard]] RHI::ResourceHandle GetVisibilityAtlasID(u32 pingIndex) const;

        // Identity siblings of the two accessors above (issue #691 step 3).
        // The FINGERPRINT reads these, not the raw ids, and that is a
        // correctness fix rather than a type change: EnsureResources calls
        // DestroyResources BEFORE recreating, so a Resolution / HitCacheTexels
        // edit frees every atlas texture and GL is then free to hand the new
        // ones the same names. Hashing the driver name therefore could not see
        // the recreate — the fingerprint stayed put, BuildFrameGraph was not
        // rebuilt, and the graph kept an import whose Width/Height still
        // described the OLD resolution (what olo_render_list_targets and
        // olo_render_capture_target then reported). A handle's generation
        // cannot be recycled, so the rebuild now always happens.
        //
        // The raw-id accessors stay: Setup still imports natively, because
        // ImportTextureHandle leaves RenderGraph::ResolveTexture answering 0
        // and that is what the MCP capture endpoints read.
        [[nodiscard]] RHI::ResourceHandle GetIrradianceAtlasHandle(u32 pingIndex) const;
        [[nodiscard]] RHI::ResourceHandle GetVisibilityAtlasHandle(u32 pingIndex) const;
        [[nodiscard]] u32 GetIrradianceCurrentIndex() const
        {
            return m_IrradianceCurrent;
        }
        [[nodiscard]] u32 GetVisibilityCurrentIndex() const
        {
            return m_VisibilityCurrent;
        }
        [[nodiscard]] bool RanThisFrame() const;
        [[nodiscard]] f32 GetCapturedFraction() const; // captured probes / total

        // Issue #751. Mean bounce-gather attenuation over every cached hit point
        // of every ACTIVE probe — the average factor the bounce gather applies,
        // each hit point counting once: 1.0 when they all lie inside the field
        // (or within its bounce margin), falling toward 0 as the surfaces the
        // probes bounce from drift past it.
        //
        // A PROXY for how much of the bounce term survives, not that fraction
        // itself: what a hit point contributes to probe irradiance is also
        // weighted by its radiance and its cosine term, and neither enters
        // here. That is fine for the question it answers — "are these probes'
        // surfaces inside the field?" — but do not quote it as delivered
        // energy.
        //
        // Returns **-1.0** when there is nothing to measure — no active probe,
        // or no probe has a cached surface hit yet. That is deliberately not
        // 1.0: "we cannot tell" and "everything is fine" must not look alike
        // in a diagnostic whose whole job is to stop a dead feature reading as
        // healthy. Callers show the number only when it is >= 0.
        //
        // SINCE #707 the underlying sum lives on the GPU, so this SYNCHRONIZES:
        // it calls ReadbackProbeDiagnostics() for you. That is correct for
        // every caller it has (the editor inspector, the MCP tools, the parity
        // tests) and wrong for exactly one place — inside Execute, which is why
        // Execute does not call it.
        [[nodiscard]] f32 GetBounceCoverage() const;

        // Uploads an Enabled=0 DDGI UBO and binds the 1x1 black placeholder to
        // slots 56/64/58 so lit-pass samplers stay valid while DDGI is off.
        void UploadDisabledUBO();

        // Issue #707 measurement surface. Filled by ReadbackProbeDiagnostics.
        struct ProbeStats
        {
            u32 LiveProbes = 0;     // requested within the lifetime window
            u32 ActiveProbes = 0;   // captured and classified Active
            u32 RelitProbes = 0;    // shaded by the relight stage LAST readback frame
            u32 CapturedProbes = 0; // captured at their current lattice point
            u32 BlendedProbes = 0;  // written by the irradiance blend
            u32 UncapturedLive = 0; // live but still waiting for a first capture
            u32 _pad0 = 0;
            u32 _pad1 = 0;
        };
        static_assert(sizeof(ProbeStats) == 32, "ProbeStats must mirror the DDGIStatsBuffer std430 block");

        // The measured "active probe count is a small fraction of the dense
        // grid" number issue #707 asks for. SYNCHRONIZES — diagnostics only.
        [[nodiscard]] ProbeStats GetProbeStats() const;
        [[nodiscard]] i32 GetTotalProbeCount() const
        {
            return m_TotalProbes;
        }

        // The per-cascade lattice this frame is using. Empty until the first
        // Execute after a volume submission.
        [[nodiscard]] const std::array<DDGI::CascadeGrid, DDGI::kMaxCascades>& GetCascades() const
        {
            return m_Cascades;
        }
        [[nodiscard]] i32 GetCascadeCount() const
        {
            return m_CascadeCount;
        }

        // CPU scheduling record, one per probe (reset on resource recreate).
        // Public for the editor debug viz (classification-colored probe
        // spheres at the RELOCATED positions) — treat as read-only outside
        // the pass.
        //
        // SINCE #707 the fields split in two. LastCaptureFrame / Captured /
        // RelocationIteration are CPU-OWNED — the CPU issues the captures, so
        // it knows them exactly and uses them to schedule. OffsetN / State /
        // BounceWeightSum / BounceHitCount are GPU-OWNED mirrors, valid only
        // after ReadbackProbeDiagnostics(); they read as their defaults
        // otherwise. Reading a GPU-owned field without that call is the one
        // way to use this struct wrong, and it fails quietly (zeros look like
        // "un-relocated, uncaptured"), so the accessor asks for the refresh.
        struct ProbeRecord
        {
            glm::vec3 OffsetN{ 0.0f };                             // GPU-owned: relocation offset, normalized by spacing
            DDGI::ProbeState State = DDGI::ProbeState::Uncaptured; // GPU-owned
            u32 LastCaptureFrame = 0;                              // CPU-owned
            f32 BounceWeightSum = 0.0f;                            // GPU-owned (#751)
            i32 BounceHitCount = 0;                                // GPU-owned (#751)
            u8 RelocationIteration = 0;                            // CPU-owned
            bool Captured = false;                                 // CPU-owned: capture issued at this lattice point
            bool PendingRelocationRecapture = false;               // CPU-owned
        };

        // Per-probe scheduling state, indexed by DDGI::CascadedProbeIndex.
        // Empty until the first Execute after a volume submission; sized to
        // the LAST submitted field's probe count (callers must bounds-check
        // against their own grid when the component was just edited).
        //
        // Call ReadbackProbeDiagnostics() first if you need the GPU-owned
        // fields (OffsetN / State / Bounce*).
        [[nodiscard]] const std::vector<ProbeRecord>& GetProbeRecords() const
        {
            return m_Records;
        }

        // Pull the GPU-owned probe state into m_Records and refresh the stats.
        // BLOCKING, and deliberately not called from Execute — see the class
        // comment. Safe to call at any time; a no-op with no resources.
        void ReadbackProbeDiagnostics() const;

      private:
        void EnsureResources();
        void DestroyResources();
        void BuildCascades();
        void UploadVolumeUBO(bool enabled);
        void UploadComputeParams(i32 probeIndexOrTotal, i32 flags);
        void BindProbeBuffers() const;

        [[nodiscard]] std::vector<i32> PickCaptureSet(i32 budget);
        [[nodiscard]] std::vector<i32> PickCaptureSetLegacy(i32 budget);
        [[nodiscard]] std::vector<i32> PickCaptureSetPrioritized(i32 budget);
        [[nodiscard]] bool IsProbeCpuLive(i32 probeIndex) const;

        void DispatchProbeMaintain();
        void DispatchScreenRequests();
        void DispatchProbeRequests();
        void CaptureProbe(i32 probeIdx);
        void ResampleProbe(i32 probeIdx);
        void RelocateProbeGPU(i32 probeIdx, bool refreshCapture);
        void BlendVisibility(const std::vector<i32>& capturedProbes);
        void RelightProbes();
        void BlendIrradiance();
        void SetPassDataProbe(i32 probeIdx);

        [[nodiscard]] f32 ComputeCapturedFraction() const;
        [[nodiscard]] glm::vec3 ProbeGridWorldPosition(i32 probeIdx) const;

        // Shaders
        Ref<Shader> m_CaptureShader;
        Ref<Shader> m_ResampleShader;
        Ref<Shader> m_RelightShader;
        Ref<Shader> m_BlendIrradianceShader;
        Ref<Shader> m_BlendVisibilityShader;
        Ref<ComputeShader> m_MaintainCompute;
        Ref<ComputeShader> m_RequestScreenCompute;
        Ref<ComputeShader> m_RequestProbeCompute;
        Ref<ComputeShader> m_RelocateCompute;

        // UBOs
        Ref<UniformBuffer> m_DDGIUBO;          // binding 51 (UBO_DDGI)
        Ref<UniformBuffer> m_PassDataUBO;      // binding 7  (UBO_USER_0) — per-draw/per-dispatch data
        Ref<UniformBuffer> m_CaptureCameraUBO; // binding 0  (UBO_CAMERA) — per-face overwrite, ShadowRenderPass style

        // SSBOs (issue #707)
        Ref<StorageBuffer> m_ProbeAuxSSBO; // binding 79 — one record per probe
        Ref<StorageBuffer> m_StatsSSBO;    // binding 80 — per-frame counters

        // Pass-owned GPU targets (created lazily on first submitted volume,
        // recreated when the Resolution / HitCacheTexels / CascadeCount
        // fingerprint changes).
        Ref<Framebuffer> m_IrradianceFB[2]; // RGBA16F ping-pong, 8x8 tiles
        Ref<Framebuffer> m_VisibilityFB[2]; // RG16F ping-pong, 16x16 tiles
        Ref<Framebuffer> m_RadianceFB;      // RGBA16F, HitCacheTexels tiles (no border)
        Ref<Framebuffer> m_HitFB;           // MRT: RGBA8 albedo+flag, RGBA16F octN+dist+flag
        Ref<Framebuffer> m_CaptureFB;       // MRT: RGBA8 + RGBA16F + D32F, 3x2 cube-face grid
        u32 m_IrradianceCurrent = 0;        // index of the CURRENT (latest complete) atlas
        u32 m_VisibilityCurrent = 0;

        // Raw GL textures (formats / usage outside the Framebuffer abstraction)
        RHI::ResourceHandle m_ProbeDataTexture{};   // RGBA16F, one texel per probe, GPU-written since #707
        RHI::ResourceHandle m_PlaceholderTexture{}; // 1x1 black RGBA16F for slots 56/64/58 when disabled
        RHI::ResourceHandle m_WhiteTexture{};       // 1x1 white RGBA8 capture albedo fallback
        RHI::ResourceHandle m_BlackCubemap{};       // 1x1 black cubemap — env fallback for the relight sky term

        // Resource fingerprint
        glm::ivec3 m_ResourceResolution{ 0 };
        i32 m_ResourceHitTexels = 0;
        i32 m_ResourceCascadeCount = 0;

        // Per-frame submission state
        DDGIVolumeDesc m_Desc;
        bool m_VolumeSubmitted = false;
        std::vector<DDGIMeshCaster> m_Casters;

        // CPU scheduling state
        mutable std::vector<ProbeRecord> m_Records;
        mutable ProbeStats m_Stats{};
        i32 m_CaptureCursor = 0; // linear cursor over uncaptured probes (legacy path)
        u32 m_FrameIndex = 0;
        bool m_RanThisFrame = false;

        // Frame-derived values (valid during Execute)
        std::array<DDGI::CascadeGrid, DDGI::kMaxCascades> m_Cascades{};
        std::array<glm::ivec3, DDGI::kMaxCascades> m_PrevLattice{};
        bool m_CascadeShifted = false;
        bool m_HaveCascadeHistory = false;
        i32 m_CascadeCount = 1;
        glm::vec3 m_Spacing{ 1.0f };
        f32 m_MinAxialSpacing = 1.0f;
        f32 m_MaxRayDistance = 1.0f;
        glm::ivec2 m_TileDims{ 0 };
        i32 m_TotalProbes = 0;
        glm::vec3 m_RenderOrigin{ 0.0f };
        glm::vec3 m_CameraWorldPos{ 0.0f };
        glm::mat4 m_WorldViewProjection{ 1.0f };
        // Rebuilt every frame from m_WorldViewProjection. The capture
        // scheduler tests probes against it DILATED by the probe->probe
        // hop's reach — see IsProbeCpuLive for why a plain frustum test
        // strands live probes permanently.
        Frustum m_ViewFrustum{};
        glm::mat4 m_PrevWorldInvViewProjection{ 1.0f };
        bool m_HavePrevViewProjection = false;

        // Previous frame's scene depth, resolved through the graph but read
        // WITHOUT a declared dependency — see DDGI_RequestScreen.comp for why
        // one frame of latency is the right trade here.
        RGTextureHandle m_SelectedSceneDepth{};
        RHI::ResourceHandle m_SceneDepthID{};
    };
} // namespace OloEngine
