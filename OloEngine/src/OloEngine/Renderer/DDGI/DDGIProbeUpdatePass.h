#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/DDGI/DDGICommon.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    // @brief Per-frame description of the active Realtime/Hybrid DDGI probe
    // volume (issue #632). Submitted by the Scene before render-graph
    // execution; consumed by DDGIProbeUpdatePass::Execute.
    struct DDGIVolumeDesc
    {
        glm::vec3 BoundsMin{ -10.0f };
        glm::vec3 BoundsMax{ 10.0f }; // ABSOLUTE world space
        glm::ivec3 Resolution{ 4, 2, 4 };
        i32 HitCacheTexels = 16; // already snapped to 8/16/32
        f32 Hysteresis = 0.9f;   // [0, 0.98]
        f32 Intensity = 1.0f;
        f32 SelfShadowBias = 0.3f;
        i32 CaptureBudget = 4;       // probes per frame, >= 1
        i32 RelightBudget = 0;       // probes per frame, 0 = all
        u8 Mode = 0;                 // 0 none/baked, 1 Realtime, 2 Hybrid
        bool BakedAvailable = false; // hybrid: baked SH asset exists
    };

    // @brief One mesh caster for the amortized DDGI probe capture. Mirrors
    // ShadowMeshCaster: raw VAO + submesh range + ABSOLUTE world transform,
    // plus the minimal material data the capture mini-G-buffer needs.
    struct DDGIMeshCaster
    {
        RendererID vaoID = 0;
        u32 indexCount = 0;
        u32 baseIndex = 0;
        glm::mat4 transform{ 1.0f }; // ABSOLUTE world
        BoundingBox worldBounds = NoBounds;
        glm::vec4 baseColor{ 1.0f };    // material base color factor
        RendererID albedoTextureID = 0; // 0 = white
        bool twoSided = false;
    };

    // @brief Realtime DDGI probe update pass (issue #632,
    // docs/adr/0006-ddgi-hit-point-cache-gather.md).
    //
    // Owns ALL DDGI GPU state and performs capture + relight + blend in one
    // render-graph node:
    //
    //   1. Amortized capture (CaptureBudget probes/frame): rasterize the
    //      submitted casters into a 3x2 cube-face mini-G-buffer around each
    //      scheduled probe (ShadowRenderPass-style dedicated pass, NOT
    //      Scene::RenderScene3D), then resample the six faces into the probe's
    //      octahedral hit-point cache (albedo / normal / distance / flag).
    //   2. CPU relocation + classification of the captured probes from a
    //      readback of their hit tiles (DDGI::RelocateProbe / ClassifyProbe).
    //   3. Visibility (Chebyshev mean/mean^2) blend for captured probes only —
    //      hit distances change only at capture time (ADR divergence).
    //   4. Per-frame relight of every cached hit point with current shadowed
    //      direct lighting (MultiLight UBO + CSM + shadow atlas, diffuse-only)
    //      plus the previous frame's probe irradiance (infinite bounce).
    //   5. Per-frame cosine convolution of the relit radiance into the
    //      irradiance atlas under adjusted-hysteresis EMA.
    //   6. Publish: atlases bound at TEX_DDGI_* (56/57/58), DDGI UBO (51)
    //      uploaded Enabled=1.
    //
    // The atlases are consumed OUTSIDE the graph's resource tracking (engine
    // sampler slots, same pattern as VolumetricFogPass's integrated volume),
    // so the pass is flagged NeverCull.
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

        // Scene found an active Realtime/Hybrid volume this frame.
        void SubmitVolume(const DDGIVolumeDesc& desc);

        // True when a volume was submitted this frame && captures are pending
        // (a caster traversal is only worth paying for when the capture stage
        // will actually consume the list).
        [[nodiscard]] bool WantsCasters() const;

        void AddMeshCaster(const DDGIMeshCaster& caster);

        // ---------------------------------------------------------------------
        // Consumption info.
        // ---------------------------------------------------------------------

        [[nodiscard]] u32 GetIrradianceAtlasID() const; // current (post-blend) — 0 until first run
        [[nodiscard]] u32 GetVisibilityAtlasID() const;
        [[nodiscard]] u32 GetProbeDataTextureID() const;
        [[nodiscard]] bool RanThisFrame() const;
        [[nodiscard]] f32 GetCapturedFraction() const; // captured probes / total

        // Uploads an Enabled=0 DDGI UBO and binds the 1x1 black placeholder to
        // slots 56/57/58 so lit-pass samplers stay valid while DDGI is off.
        void UploadDisabledUBO();

        // CPU scheduling record, one per probe (reset on resource recreate).
        // Public for the editor debug viz (classification-colored probe
        // spheres at the RELOCATED positions) — treat as read-only outside
        // the pass.
        struct ProbeRecord
        {
            glm::vec3 OffsetN{ 0.0f }; // relocation offset, normalized by spacing
            DDGI::ProbeState State = DDGI::ProbeState::Uncaptured;
            u32 LastCaptureFrame = 0;
            u8 RelocationIteration = 0;
            bool PendingRelocationRecapture = false;
        };

        // Per-probe scheduling state, indexed by DDGI::ProbeLinearIndex.
        // Empty until the first Execute after a volume submission; sized to
        // the LAST submitted volume's probe count (callers must bounds-check
        // against their own grid when the component was just edited).
        [[nodiscard]] const std::vector<ProbeRecord>& GetProbeRecords() const
        {
            return m_Records;
        }

      private:
        void EnsureResources();
        void DestroyResources();
        void UploadVolumeUBO(bool enabled);

        [[nodiscard]] std::vector<i32> PickCaptureSet(i32 budget);
        void CaptureProbe(i32 probeIdx);
        void ResampleProbe(i32 probeIdx);
        void RelocateAndClassifyProbe(i32 probeIdx);
        void BlendVisibility(const std::vector<i32>& capturedProbes);
        void RelightProbes();
        void BlendIrradiance();
        void SetPassDataProbe(i32 probeIdx, const glm::vec3& probeRelPos);

        [[nodiscard]] f32 ComputeCapturedFraction() const;

        // Shaders
        Ref<Shader> m_CaptureShader;
        Ref<Shader> m_ResampleShader;
        Ref<Shader> m_RelightShader;
        Ref<Shader> m_BlendIrradianceShader;
        Ref<Shader> m_BlendVisibilityShader;

        // UBOs
        Ref<UniformBuffer> m_DDGIUBO;          // binding 51 (UBO_DDGI)
        Ref<UniformBuffer> m_PassDataUBO;      // binding 7  (UBO_USER_0) — per-draw capture/resample/blend data
        Ref<UniformBuffer> m_CaptureCameraUBO; // binding 0  (UBO_CAMERA) — per-face overwrite, ShadowRenderPass style

        // Pass-owned GPU targets (created lazily on first submitted volume,
        // recreated when the Resolution / HitCacheTexels fingerprint changes).
        Ref<Framebuffer> m_IrradianceFB[2]; // RGBA16F ping-pong, 8x8 tiles
        Ref<Framebuffer> m_VisibilityFB[2]; // RG16F ping-pong, 16x16 tiles
        Ref<Framebuffer> m_RadianceFB;      // RGBA16F, HitCacheTexels tiles (no border)
        Ref<Framebuffer> m_HitFB;           // MRT: RGBA8 albedo+flag, RGBA16F octN+dist+flag
        Ref<Framebuffer> m_CaptureFB;       // MRT: RGBA8 + RGBA16F + D32F, 3x2 cube-face grid
        u32 m_IrradianceCurrent = 0;        // index of the CURRENT (latest complete) atlas
        u32 m_VisibilityCurrent = 0;

        // Raw GL textures (formats / usage outside the Framebuffer abstraction)
        u32 m_ProbeDataTexture = 0;   // RGBA16F, one texel per probe, CPU-written only
        u32 m_PlaceholderTexture = 0; // 1x1 black RGBA16F for slots 56/57/58 when disabled
        u32 m_WhiteTexture = 0;       // 1x1 white RGBA8 capture albedo fallback
        u32 m_BlackCubemap = 0;       // 1x1 black cubemap — env fallback for the relight sky term

        // Resource fingerprint
        glm::ivec3 m_ResourceResolution{ 0 };
        i32 m_ResourceHitTexels = 0;

        // Per-frame submission state
        DDGIVolumeDesc m_Desc;
        bool m_VolumeSubmitted = false;
        std::vector<DDGIMeshCaster> m_Casters;

        // CPU scheduling state
        std::vector<ProbeRecord> m_Records;
        i32 m_CaptureCursor = 0;    // linear cursor over uncaptured probes
        i32 m_RelightRowCursor = 0; // wrap cursor over radiance-atlas tile rows (RelightBudget > 0)
        u32 m_FrameIndex = 0;
        bool m_RanThisFrame = false;

        // Frame-derived volume values (valid during Execute)
        glm::vec3 m_Spacing{ 1.0f };
        f32 m_MinAxialSpacing = 1.0f;
        f32 m_MaxRayDistance = 1.0f;
        glm::ivec2 m_TileDims{ 0 };
        i32 m_TotalProbes = 0;
        glm::vec3 m_RenderOrigin{ 0.0f };
    };
} // namespace OloEngine
