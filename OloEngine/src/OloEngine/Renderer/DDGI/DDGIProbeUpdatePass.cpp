#include "OloEnginePCH.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Cube-face look directions / up vectors — the SAME orientation tables
        // as ReflectionProbeBaker / IBLPrecompute (GL cubemap face order
        // +X,-X,+Y,-Y,+Z,-Z), so DDGI_Resample.glsl's face-selection basis
        // tables match the rasterized faces exactly.
        constexpr glm::vec3 kFaceTargets[6] = {
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
        };

        constexpr glm::vec3 kFaceUps[6] = {
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
        };

        // Bounce-feedback albedo clamp (ADR 0006: albedo <= 0.9 in the bounce
        // term keeps the infinite-bounce feedback loop contractive).
        constexpr f32 kEnergyConservation = 0.9f;

        // Relocation-driven recapture: stop after 4 iterations (RTXGI-style
        // fixed-point cutoff) and when the offset moved < 1% of a cell.
        constexpr u8 kMaxRelocationIterations = 4;
        constexpr f32 kRelocationSettleThreshold = 0.01f; // normalized units

        // Per-draw data for the DDGI shaders, bound at UBO_USER_0 (binding 7).
        // Mirrors the `DDGIPassData` std140 block declared in
        // DDGI_Capture/DDGI_Resample/DDGI_BlendVisibility.glsl. A dedicated
        // block is used instead of ModelMatrices(3)/MaterialProperties(2):
        // those blocks carry deferred-path fields (EntityID, PrevModel, texture
        // flags) the capture has no use for, and reusing them would force this
        // pass to fight the scene pass over engine-global UBO contents.
        struct DDGIPassDataUBO
        {
            glm::mat4 Model;         // render-relative model matrix (capture)
            glm::mat4 NormalMatrix;  // transpose(inverse(Model)) (capture)
            glm::vec4 BaseColor;     // material base color factor (capture)
            glm::vec4 ProbePosition; // xyz = render-relative probe pos, w = probe linear index
        };
        static_assert(sizeof(DDGIPassDataUBO) == 160, "DDGIPassDataUBO std140 size drifted from GLSL expectation (160 B)");
        static_assert(sizeof(DDGIPassDataUBO) % 16 == 0, "DDGIPassDataUBO must be 16-byte aligned for std140");

        void SetAtlasTextureParams(u32 texID, GLint filter)
        {
            RenderCommand::SetTextureParameter(texID, GL_TEXTURE_MIN_FILTER, filter);
            RenderCommand::SetTextureParameter(texID, GL_TEXTURE_MAG_FILTER, filter);
            RenderCommand::SetTextureParameter(texID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            RenderCommand::SetTextureParameter(texID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        // Shared render state for the fullscreen-triangle stages (resample /
        // relight / blend) — mirrors SSGIRenderPass::Execute's state block.
        void SetFullscreenPassState()
        {
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
        }

        // Conservative caster reach test: skip casters entirely beyond the
        // capture far plane. NoBounds casters are always included (matches
        // ShadowRenderPass::ShouldCull's convention).
        [[nodiscard]] bool CasterInRange(const DDGIMeshCaster& caster, const glm::vec3& probeWorld, f32 radius)
        {
            if (caster.worldBounds.Min.x >= std::numeric_limits<f32>::max())
            {
                return true;
            }
            const glm::vec3 closest = glm::clamp(probeWorld, caster.worldBounds.Min, caster.worldBounds.Max);
            const glm::vec3 d = closest - probeWorld;
            return glm::dot(d, d) <= radius * radius;
        }
    } // namespace

    DDGIProbeUpdatePass::DDGIProbeUpdatePass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("DDGIProbeUpdatePass");
        // The atlases are consumed outside the graph's resource tracking (lit
        // passes sample them at engine slots 56/57/58), so the reachability
        // cull must never drop this pass — same reasoning as VolumetricFogPass.
        SetSideEffects(SideEffect::NeverCull);
    }

    DDGIProbeUpdatePass::~DDGIProbeUpdatePass()
    {
        DestroyResources();
        if (m_PlaceholderTexture != 0)
        {
            RenderCommand::DeleteTexture(m_PlaceholderTexture);
            m_PlaceholderTexture = 0;
        }
        if (m_WhiteTexture != 0)
        {
            RenderCommand::DeleteTexture(m_WhiteTexture);
            m_WhiteTexture = 0;
        }
        if (m_BlackCubemap != 0)
        {
            RenderCommand::DeleteTexture(m_BlackCubemap);
            m_BlackCubemap = 0;
        }
    }

    void DDGIProbeUpdatePass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_CaptureShader = Shader::Create("assets/shaders/DDGI_Capture.glsl");
        m_ResampleShader = Shader::Create("assets/shaders/DDGI_Resample.glsl");
        m_RelightShader = Shader::Create("assets/shaders/DDGI_Relight.glsl");
        m_BlendIrradianceShader = Shader::Create("assets/shaders/DDGI_BlendIrradiance.glsl");
        m_BlendVisibilityShader = Shader::Create("assets/shaders/DDGI_BlendVisibility.glsl");

        m_DDGIUBO = UniformBuffer::Create(UBOStructures::DDGIVolumeUBO::GetSize(),
                                          ShaderBindingLayout::UBO_DDGI);
        m_PassDataUBO = UniformBuffer::Create(sizeof(DDGIPassDataUBO),
                                              ShaderBindingLayout::UBO_USER_0);
        m_CaptureCameraUBO = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(),
                                                   ShaderBindingLayout::UBO_CAMERA);

        if (m_PlaceholderTexture == 0)
        {
            // 1x1 black RGBA16F — a single texture serves all three disabled
            // slots (sampler2D reads of .rg / .rgb / .w all see zero, and
            // state 0 == Uncaptured makes the sampler skip every probe).
            m_PlaceholderTexture = RenderCommand::CreateTexture2D(1, 1, GL_RGBA16F);
            glClearTexImage(m_PlaceholderTexture, 0, GL_RGBA, GL_FLOAT, nullptr);
            SetAtlasTextureParams(m_PlaceholderTexture, GL_NEAREST);
        }
        if (m_WhiteTexture == 0)
        {
            m_WhiteTexture = RenderCommand::CreateTexture2D(1, 1, GL_RGBA8);
            constexpr u8 white[4] = { 255, 255, 255, 255 };
            RenderCommand::UploadTextureSubImage2D(m_WhiteTexture, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
            SetAtlasTextureParams(m_WhiteTexture, GL_NEAREST);
        }
        if (m_BlackCubemap == 0)
        {
            // Environment fallback for the relight sky term when no global
            // IBL environment cubemap exists this frame.
            m_BlackCubemap = RenderCommand::CreateTextureCubemap(1, 1, GL_RGBA16F);
            glClearTexImage(m_BlackCubemap, 0, GL_RGBA, GL_FLOAT, nullptr);
            RenderCommand::SetTextureParameter(m_BlackCubemap, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            RenderCommand::SetTextureParameter(m_BlackCubemap, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            RenderCommand::SetTextureParameter(m_BlackCubemap, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            RenderCommand::SetTextureParameter(m_BlackCubemap, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            RenderCommand::SetTextureParameter(m_BlackCubemap, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }

        OLO_CORE_INFO("DDGIProbeUpdatePass: Initialized (atlases created lazily on first submitted volume)");
    }

    void DDGIProbeUpdatePass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // The relight stage samples the CSM + shadow atlas — order after the
        // shadow pass (registered as "ShadowPass" by the pipeline builder) and
        // declare the reads so the graph keeps the maps alive for us.
        builder.DependsOnPass("ShadowPass");

        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            [[maybe_unused]] const auto csmRead = builder.Read(blackboard.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }
        if (blackboard.Shadows.ShadowMapAtlas.IsValid())
        {
            [[maybe_unused]] const auto atlasRead = builder.Read(blackboard.Shadows.ShadowMapAtlas, RGReadUsage::ShaderSample);
        }

        // The atlases themselves are NOT declared as graph resources: the
        // render graph has no cheap import path for externally-owned pass
        // textures (VolumetricFogPass / PlanarReflectionRenderPass publish
        // through Renderer3D accessors for the same reason), so MCP capture of
        // the DDGI atlases goes through the pass accessors instead.
    }

    bool DDGIProbeUpdatePass::IsReadyForExecution() const noexcept
    {
        return m_CaptureShader && m_CaptureShader->IsReady() &&
               m_ResampleShader && m_ResampleShader->IsReady() &&
               m_RelightShader && m_RelightShader->IsReady() &&
               m_BlendIrradianceShader && m_BlendIrradianceShader->IsReady() &&
               m_BlendVisibilityShader && m_BlendVisibilityShader->IsReady() &&
               m_DDGIUBO != nullptr && m_PassDataUBO != nullptr && m_CaptureCameraUBO != nullptr;
    }

    void DDGIProbeUpdatePass::SubmitVolume(const DDGIVolumeDesc& desc)
    {
        m_Desc = desc;
        m_Desc.Resolution = glm::max(m_Desc.Resolution, glm::ivec3(1));
        m_Desc.HitCacheTexels = DDGI::HitCacheResolutionForRayCount(m_Desc.HitCacheTexels * m_Desc.HitCacheTexels);
        m_Desc.Hysteresis = glm::clamp(m_Desc.Hysteresis, 0.0f, 0.98f);
        m_Desc.CaptureBudget = std::max(m_Desc.CaptureBudget, 1);
        m_VolumeSubmitted = true;
    }

    bool DDGIProbeUpdatePass::WantsCasters() const
    {
        if (!m_VolumeSubmitted || m_Desc.CaptureBudget <= 0)
        {
            return false;
        }
        const i64 total = static_cast<i64>(m_Desc.Resolution.x) * m_Desc.Resolution.y * m_Desc.Resolution.z;
        return total > 0;
    }

    void DDGIProbeUpdatePass::AddMeshCaster(const DDGIMeshCaster& caster)
    {
        m_Casters.push_back(caster);
    }

    u32 DDGIProbeUpdatePass::GetIrradianceAtlasID() const
    {
        return m_IrradianceFB[m_IrradianceCurrent]
                   ? m_IrradianceFB[m_IrradianceCurrent]->GetColorAttachmentRendererID(0)
                   : 0;
    }

    u32 DDGIProbeUpdatePass::GetVisibilityAtlasID() const
    {
        return m_VisibilityFB[m_VisibilityCurrent]
                   ? m_VisibilityFB[m_VisibilityCurrent]->GetColorAttachmentRendererID(0)
                   : 0;
    }

    u32 DDGIProbeUpdatePass::GetProbeDataTextureID() const
    {
        return m_ProbeDataTexture;
    }

    bool DDGIProbeUpdatePass::RanThisFrame() const
    {
        return m_RanThisFrame;
    }

    f32 DDGIProbeUpdatePass::GetCapturedFraction() const
    {
        return ComputeCapturedFraction();
    }

    f32 DDGIProbeUpdatePass::ComputeCapturedFraction() const
    {
        if (m_Records.empty())
        {
            return 0.0f;
        }
        sizet captured = 0;
        for (const auto& r : m_Records)
        {
            if (r.State != DDGI::ProbeState::Uncaptured)
            {
                ++captured;
            }
        }
        return static_cast<f32>(captured) / static_cast<f32>(m_Records.size());
    }

    void DDGIProbeUpdatePass::UploadDisabledUBO()
    {
        if (m_DDGIUBO)
        {
            UBOStructures::DDGIVolumeUBO ubo{};
            ubo.Enabled = 0;
            m_DDGIUBO->SetData(&ubo, sizeof(ubo));
            m_DDGIUBO->Bind();
        }
        if (m_PlaceholderTexture != 0)
        {
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_DDGI_IRRADIANCE, m_PlaceholderTexture);
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_DDGI_VISIBILITY, m_PlaceholderTexture);
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_DDGI_PROBE_DATA, m_PlaceholderTexture);
        }
    }

    void DDGIProbeUpdatePass::DestroyResources()
    {
        m_IrradianceFB[0] = nullptr;
        m_IrradianceFB[1] = nullptr;
        m_VisibilityFB[0] = nullptr;
        m_VisibilityFB[1] = nullptr;
        m_RadianceFB = nullptr;
        m_HitFB = nullptr;
        m_CaptureFB = nullptr;
        if (m_ProbeDataTexture != 0)
        {
            RenderCommand::DeleteTexture(m_ProbeDataTexture);
            m_ProbeDataTexture = 0;
        }
        m_ResourceResolution = glm::ivec3(0);
        m_ResourceHitTexels = 0;
    }

    void DDGIProbeUpdatePass::EnsureResources()
    {
        OLO_PROFILE_FUNCTION();

        if (m_ResourceResolution == m_Desc.Resolution && m_ResourceHitTexels == m_Desc.HitCacheTexels)
        {
            return;
        }

        DestroyResources();

        const glm::ivec2 tileDims = DDGI::AtlasTileDimensions(m_Desc.Resolution);
        const i32 t = m_Desc.HitCacheTexels;

        auto makeAtlasFB = [&](FramebufferTextureFormat format, i32 tileTexels) -> Ref<Framebuffer>
        {
            FramebufferSpecification spec;
            spec.Width = static_cast<u32>(tileDims.x * tileTexels);
            spec.Height = static_cast<u32>(tileDims.y * tileTexels);
            spec.Attachments = { format };
            return Framebuffer::Create(spec);
        };

        // Irradiance ping-pong (RGBA16F, 8x8 tiles) — LINEAR: the sampler
        // bilinears across the border gutter.
        for (u32 i = 0; i < 2; ++i)
        {
            m_IrradianceFB[i] = makeAtlasFB(FramebufferTextureFormat::RGBA16F, DDGI::kIrradianceTileTexels);
            SetAtlasTextureParams(m_IrradianceFB[i]->GetColorAttachmentRendererID(0), GL_LINEAR);
            m_IrradianceFB[i]->Bind();
            m_IrradianceFB[i]->ClearAllAttachments(glm::vec4(0.0f), -1);
        }

        // Visibility ping-pong (RG16F, 16x16 tiles) — LINEAR.
        for (u32 i = 0; i < 2; ++i)
        {
            m_VisibilityFB[i] = makeAtlasFB(FramebufferTextureFormat::RG16F, DDGI::kVisibilityTileTexels);
            SetAtlasTextureParams(m_VisibilityFB[i]->GetColorAttachmentRendererID(0), GL_LINEAR);
            m_VisibilityFB[i]->Bind();
            m_VisibilityFB[i]->ClearAllAttachments(glm::vec4(0.0f), -1);
        }

        // Radiance cache (RGBA16F, HitCacheTexels tiles, no border) — NEAREST
        // (texelFetch-only consumer).
        m_RadianceFB = makeAtlasFB(FramebufferTextureFormat::RGBA16F, t);
        SetAtlasTextureParams(m_RadianceFB->GetColorAttachmentRendererID(0), GL_NEAREST);
        m_RadianceFB->Bind();
        m_RadianceFB->ClearAllAttachments(glm::vec4(0.0f), -1);

        // Hit-point cache MRT: RT0 RGBA8 albedo + flag, RT1 RGBA16F octNormal
        // + distance + flag (the canonical flag home per DDGICommon.glsl's
        // DDGI_HIT_* contract) — NEAREST.
        {
            FramebufferSpecification spec;
            spec.Width = static_cast<u32>(tileDims.x * t);
            spec.Height = static_cast<u32>(tileDims.y * t);
            spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RGBA16F };
            m_HitFB = Framebuffer::Create(spec);
            SetAtlasTextureParams(m_HitFB->GetColorAttachmentRendererID(0), GL_NEAREST);
            SetAtlasTextureParams(m_HitFB->GetColorAttachmentRendererID(1), GL_NEAREST);
            m_HitFB->Bind();
            m_HitFB->ClearAttachment(0, glm::vec4(0.0f));
            // Geo cleared to "sky" so never-resampled tiles read as misses.
            m_HitFB->ClearAttachment(1, glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
        }

        // Capture target: 3x2 grid of cube faces, faceRes = 2 * HitCacheTexels.
        // RT0 RGBA8 albedo + gl_FrontFacing flag, RT1 RGBA16F octNormal +
        // linear distance, D32F depth.
        {
            const i32 faceRes = 2 * t;
            FramebufferSpecification spec;
            spec.Width = static_cast<u32>(3 * faceRes);
            spec.Height = static_cast<u32>(2 * faceRes);
            spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RGBA16F,
                                 FramebufferTextureFormat::ShadowDepth };
            m_CaptureFB = Framebuffer::Create(spec);
            SetAtlasTextureParams(m_CaptureFB->GetColorAttachmentRendererID(0), GL_NEAREST);
            SetAtlasTextureParams(m_CaptureFB->GetColorAttachmentRendererID(1), GL_NEAREST);
        }

        // Probe data: one texel per probe (xyz = relocation offset normalized
        // by spacing, w = state). CPU-written ONLY (glTextureSubImage2D from
        // the relocation/classification step); cleared to zero == Uncaptured.
        m_ProbeDataTexture = RenderCommand::CreateTexture2D(static_cast<u32>(tileDims.x),
                                                            static_cast<u32>(tileDims.y), GL_RGBA16F);
        glClearTexImage(m_ProbeDataTexture, 0, GL_RGBA, GL_FLOAT, nullptr);
        SetAtlasTextureParams(m_ProbeDataTexture, GL_NEAREST);

        // Reset the CPU scheduling mirror — a new grid invalidates every record.
        const sizet total = static_cast<sizet>(m_Desc.Resolution.x) *
                            static_cast<sizet>(m_Desc.Resolution.y) *
                            static_cast<sizet>(m_Desc.Resolution.z);
        m_Records.assign(total, ProbeRecord{});
        m_CaptureCursor = 0;
        m_RelightRowCursor = 0;
        m_IrradianceCurrent = 0;
        m_VisibilityCurrent = 0;

        m_ResourceResolution = m_Desc.Resolution;
        m_ResourceHitTexels = m_Desc.HitCacheTexels;

        OLO_CORE_INFO("DDGIProbeUpdatePass: (re)created atlases for {}x{}x{} probes, {} hit texels/probe",
                      m_Desc.Resolution.x, m_Desc.Resolution.y, m_Desc.Resolution.z, t);
    }

    void DDGIProbeUpdatePass::UploadVolumeUBO(bool enabled)
    {
        UBOStructures::DDGIVolumeUBO ubo{};
        ubo.BoundsMin = glm::vec4(m_Desc.BoundsMin - m_RenderOrigin, 0.0f);
        ubo.BoundsMax = glm::vec4(m_Desc.BoundsMax - m_RenderOrigin, 0.0f);
        ubo.GridDimensions = glm::ivec4(m_Desc.Resolution, m_TotalProbes);
        ubo.ProbeSpacing = glm::vec4(m_Spacing, m_MinAxialSpacing);
        ubo.Enabled = enabled ? 1 : 0;
        ubo.Intensity = m_Desc.Intensity;
        ubo.Hysteresis = m_Desc.Hysteresis;
        ubo.SelfShadowBias = m_Desc.SelfShadowBias;
        ubo.HitCacheTexels = m_Desc.HitCacheTexels;
        ubo.FrameIndex = static_cast<i32>(m_FrameIndex);
        ubo.HybridBlend = (m_Desc.Mode == 2 && m_Desc.BakedAvailable) ? ComputeCapturedFraction() : 1.0f;
        ubo.EnergyConservation = kEnergyConservation;
        ubo.MaxRayDistance = m_MaxRayDistance;
        m_DDGIUBO->SetData(&ubo, sizeof(ubo));
        m_DDGIUBO->Bind();
    }

    std::vector<i32> DDGIProbeUpdatePass::PickCaptureSet(i32 budget)
    {
        std::vector<i32> result;
        const i32 total = m_TotalProbes;
        if (total <= 0 || budget <= 0)
        {
            return result;
        }
        budget = std::min(budget, total);
        result.reserve(static_cast<sizet>(budget));
        std::vector<u8> picked(static_cast<sizet>(total), 0u);

        // 1) Probes whose relocation moved them — recapture from the new spot.
        for (i32 i = 0; i < total && static_cast<i32>(result.size()) < budget; ++i)
        {
            if (m_Records[i].PendingRelocationRecapture)
            {
                result.push_back(i);
                picked[i] = 1u;
            }
        }

        // 2) Never-captured probes, linear cursor. Scan indices derive from a
        //    FROZEN copy of the cursor: advancing m_CaptureCursor on each
        //    selection while also using it in the index expression made the
        //    scan jump ahead by the accumulated selections, skipping probes
        //    and underfilling the budget within a single frame's pass.
        const i32 scanStart = m_CaptureCursor;
        for (i32 n = 0; n < total && static_cast<i32>(result.size()) < budget; ++n)
        {
            const i32 idx = (scanStart + n) % total;
            if (picked[idx] == 0u && m_Records[idx].State == DDGI::ProbeState::Uncaptured)
            {
                result.push_back(idx);
                picked[idx] = 1u;
                m_CaptureCursor = (idx + 1) % total;
            }
        }

        // 3) Continuous refresh — only reachable once no uncaptured probes
        //    remain (step 2 otherwise consumes the budget): re-capture the
        //    oldest so moved static geometry heals at bounded cost. The
        //    steady-state refresh runs at 1/8 of the capture budget (Lumen's
        //    CardCaptureRefreshFraction=0.125 idea) — without this cap the
        //    pass re-rasterized the FULL budget every frame forever, ~8x the
        //    intended steady-state cost (measured 9.7 ms GPU on the 32-probe
        //    bring-up scene; ~2.5 ms with the cap).
        if (!result.empty())
        {
            return result; // initial fill / relocation still in progress — no refresh this frame
        }
        const i32 refreshBudget = std::max(1, budget / 8);
        while (static_cast<i32>(result.size()) < refreshBudget)
        {
            i32 oldest = -1;
            u32 oldestFrame = std::numeric_limits<u32>::max();
            for (i32 i = 0; i < total; ++i)
            {
                if (picked[i] == 0u && m_Records[i].State != DDGI::ProbeState::Uncaptured &&
                    m_Records[i].LastCaptureFrame < oldestFrame)
                {
                    oldest = i;
                    oldestFrame = m_Records[i].LastCaptureFrame;
                }
            }
            if (oldest < 0)
            {
                break;
            }
            result.push_back(oldest);
            picked[oldest] = 1u;
        }
        return result;
    }

    void DDGIProbeUpdatePass::SetPassDataProbe(i32 probeIdx, const glm::vec3& probeRelPos)
    {
        DDGIPassDataUBO data{};
        data.Model = glm::mat4(1.0f);
        data.NormalMatrix = glm::mat4(1.0f);
        data.BaseColor = glm::vec4(1.0f);
        data.ProbePosition = glm::vec4(probeRelPos, static_cast<f32>(probeIdx));
        m_PassDataUBO->SetData(&data, sizeof(data));
        m_PassDataUBO->Bind();
    }

    void DDGIProbeUpdatePass::CaptureProbe(i32 probeIdx)
    {
        OLO_PROFILE_FUNCTION();

        const glm::ivec3 coord = DDGI::ProbeGridCoord(probeIdx, m_Desc.Resolution);
        const glm::vec3 probeWorld = DDGI::ProbeWorldPosition(coord, m_Desc.BoundsMin, m_Desc.BoundsMax,
                                                              m_Desc.Resolution, m_Records[probeIdx].OffsetN);
        const glm::vec3 probeRel = probeWorld - m_RenderOrigin;

        const i32 faceRes = 2 * m_Desc.HitCacheTexels;
        const f32 farPlane = 2.0f * m_MaxRayDistance;

        m_CaptureFB->Bind();
        // Whole-grid clears (glClearTexImage under the hood — viewport-free):
        // RT0 -> (0,0,0,0), RT1 -> (0,0,-1,0) so unrendered texels read as sky.
        m_CaptureFB->ClearAttachment(0, glm::vec4(0.0f));
        m_CaptureFB->ClearAttachment(1, glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
        RenderCommand::ClearDepthOnly();

        // Capture render state: depth-tested opaque mini-G-buffer. Culling is
        // DISABLED for every caster — the capture must SEE backfaces so
        // in-wall probes read backface-heavy caches (classification signal);
        // the fragment stage tags them via gl_FrontFacing.
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        m_CaptureShader->Bind();
        m_CaptureCameraUBO->Bind();
        m_PassDataUBO->Bind();

        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, farPlane);

        for (u32 face = 0; face < 6; ++face)
        {
            // The face VP is built directly in render-relative space (the eye
            // is the RELATIVE probe position), which is exactly
            // MakeViewProjectionRelative(worldVP, origin) evaluated
            // analytically — the casters below shift by the same origin via
            // MakeModelRelative, mirroring ShadowRenderPass's lightVP recipe.
            const glm::mat4 view = glm::lookAt(probeRel, probeRel + kFaceTargets[face], kFaceUps[face]);
            const glm::mat4 vp = proj * view;

            UBOStructures::CameraUBO camera{};
            camera.ViewProjection = vp;
            camera.View = view;
            camera.Projection = proj;
            camera.Position = probeRel;
            camera._padding0 = 0.0f;
            camera.PrevViewProjection = vp;
            camera.RenderOrigin = m_RenderOrigin;
            m_CaptureCameraUBO->SetData(&camera, UBOStructures::CameraUBO::GetSize());

            RenderCommand::SetViewport(static_cast<u32>((face % 3u) * static_cast<u32>(faceRes)),
                                       static_cast<u32>((face / 3u) * static_cast<u32>(faceRes)),
                                       static_cast<u32>(faceRes), static_cast<u32>(faceRes));

            for (const auto& caster : m_Casters)
            {
                if (caster.vaoID == 0 || caster.indexCount == 0)
                {
                    continue;
                }
                if (!CasterInRange(caster, probeWorld, farPlane))
                {
                    continue;
                }

                RenderCommand::BindTexture(0, caster.albedoTextureID != 0 ? caster.albedoTextureID : m_WhiteTexture);

                DDGIPassDataUBO data{};
                data.Model = MakeModelRelative(caster.transform, m_RenderOrigin);
                data.NormalMatrix = glm::transpose(glm::inverse(data.Model));
                data.BaseColor = caster.baseColor;
                data.ProbePosition = glm::vec4(probeRel, static_cast<f32>(probeIdx));
                m_PassDataUBO->SetData(&data, sizeof(data));

                RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount, caster.baseIndex);
            }
        }
    }

    void DDGIProbeUpdatePass::ResampleProbe(i32 probeIdx)
    {
        OLO_PROFILE_FUNCTION();

        const i32 t = m_Desc.HitCacheTexels;
        const glm::ivec2 tile = DDGI::ProbeTileCoord(probeIdx, m_Desc.Resolution);

        m_HitFB->Bind();
        SetFullscreenPassState();
        RenderCommand::SetViewport(static_cast<u32>(tile.x * t), static_cast<u32>(tile.y * t),
                                   static_cast<u32>(t), static_cast<u32>(t));

        m_ResampleShader->Bind();
        RenderCommand::BindTexture(0, m_CaptureFB->GetColorAttachmentRendererID(0));
        RenderCommand::BindTexture(1, m_CaptureFB->GetColorAttachmentRendererID(1));
        SetPassDataProbe(probeIdx, glm::vec3(0.0f));

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
    }

    void DDGIProbeUpdatePass::RelocateAndClassifyProbe(i32 probeIdx)
    {
        OLO_PROFILE_FUNCTION();

        const i32 t = m_Desc.HitCacheTexels;
        const glm::ivec2 tile = DDGI::ProbeTileCoord(probeIdx, m_Desc.Resolution);
        const u32 geoTex = m_HitFB->GetColorAttachmentRendererID(1);

        // Read the probe's hit-geo tile back (rg = octNormal, b = distance
        // [< 0 = sky], a = DDGI_HIT_* flag). RGBA16F -> GL converts to float.
        std::vector<glm::vec4> texels(static_cast<sizet>(t) * static_cast<sizet>(t));
        glGetTextureSubImage(geoTex, 0, tile.x * t, tile.y * t, 0, t, t, 1, GL_RGBA, GL_FLOAT,
                             static_cast<GLsizei>(texels.size() * sizeof(glm::vec4)), texels.data());

        DDGI::ProbeHitAggregates agg;
        i32 backfaceCount = 0;
        const f32 cellReach = glm::length(m_Spacing);
        for (i32 y = 0; y < t; ++y)
        {
            for (i32 x = 0; x < t; ++x)
            {
                const glm::vec4& v = texels[static_cast<sizet>(y) * static_cast<sizet>(t) + static_cast<sizet>(x)];
                const f32 dist = v.z;
                const f32 flag = v.w;
                if (dist < 0.0f || flag < 0.25f)
                {
                    // Sky: not a hit — neither backface nor frontface, and it
                    // never satisfies AnyHitWithinCell.
                    continue;
                }
                const glm::vec3 dir = DDGI::TexelDirection({ x, y }, t);
                if (flag < 0.75f)
                {
                    ++backfaceCount;
                    if (agg.ClosestBackfaceDist < 0.0f || dist < agg.ClosestBackfaceDist)
                    {
                        agg.ClosestBackfaceDist = dist;
                        agg.ClosestBackfaceDir = dir;
                    }
                }
                else
                {
                    if (agg.ClosestFrontfaceDist < 0.0f || dist < agg.ClosestFrontfaceDist)
                    {
                        agg.ClosestFrontfaceDist = dist;
                        agg.ClosestFrontfaceDir = dir;
                    }
                    if (dist > agg.FarthestFrontfaceDist)
                    {
                        agg.FarthestFrontfaceDist = dist;
                        agg.FarthestFrontfaceDir = dir;
                    }
                    if (dist <= cellReach)
                    {
                        agg.AnyHitWithinCell = true;
                    }
                }
            }
        }
        agg.BackfaceFraction = static_cast<f32>(backfaceCount) / static_cast<f32>(t * t);

        const f32 minFrontfaceDistance = 0.25f * m_MinAxialSpacing;
        auto& rec = m_Records[probeIdx];
        const glm::vec3 newOffset = DDGI::RelocateProbe(rec.OffsetN, agg, m_Spacing, minFrontfaceDistance);
        const DDGI::ProbeState newState = DDGI::ClassifyProbe(agg);

        const glm::vec3 delta = newOffset - rec.OffsetN;
        const bool moved = glm::dot(delta, delta) > kRelocationSettleThreshold * kRelocationSettleThreshold;
        if (moved && rec.RelocationIteration < kMaxRelocationIterations)
        {
            // The NEXT frame re-captures from the relocated position.
            rec.PendingRelocationRecapture = true;
            ++rec.RelocationIteration;
        }
        else
        {
            rec.PendingRelocationRecapture = false;
        }
        rec.OffsetN = newOffset;
        rec.State = newState;
        rec.LastCaptureFrame = m_FrameIndex;

        const f32 texel[4] = { newOffset.x, newOffset.y, newOffset.z,
                               static_cast<f32>(std::to_underlying(newState)) };
        glTextureSubImage2D(m_ProbeDataTexture, 0, tile.x, tile.y, 1, 1, GL_RGBA, GL_FLOAT, texel);
    }

    void DDGIProbeUpdatePass::BlendVisibility(const std::vector<i32>& capturedProbes)
    {
        OLO_PROFILE_FUNCTION();

        const u32 prevIdx = m_VisibilityCurrent;
        const u32 currIdx = 1u - m_VisibilityCurrent;
        const u32 prevTex = m_VisibilityFB[prevIdx]->GetColorAttachmentRendererID(0);
        const u32 currTex = m_VisibilityFB[currIdx]->GetColorAttachmentRendererID(0);
        const glm::ivec2 visSize = m_TileDims * DDGI::kVisibilityTileTexels;

        // Carry every un-recaptured tile forward, then overwrite only the
        // captured probes' tiles below.
        RenderCommand::CopyImageSubData(prevTex, RendererAPI::TextureTargetType::Texture2D,
                                        currTex, RendererAPI::TextureTargetType::Texture2D,
                                        static_cast<u32>(visSize.x), static_cast<u32>(visSize.y));

        m_VisibilityFB[currIdx]->Bind();
        SetFullscreenPassState();

        m_BlendVisibilityShader->Bind();
        RenderCommand::BindTexture(0, m_HitFB->GetColorAttachmentRendererID(1)); // hit geo (dist + flag)
        RenderCommand::BindTexture(1, prevTex);                                  // EMA history

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();

        for (const i32 probeIdx : capturedProbes)
        {
            const glm::ivec2 tile = DDGI::ProbeTileCoord(probeIdx, m_Desc.Resolution);
            RenderCommand::SetViewport(static_cast<u32>(tile.x * DDGI::kVisibilityTileTexels),
                                       static_cast<u32>(tile.y * DDGI::kVisibilityTileTexels),
                                       static_cast<u32>(DDGI::kVisibilityTileTexels),
                                       static_cast<u32>(DDGI::kVisibilityTileTexels));
            SetPassDataProbe(probeIdx, glm::vec3(0.0f));
            RenderCommand::DrawIndexed(va);
        }

        // Swap AFTER all tiles so consumers see one coherent atlas.
        m_VisibilityCurrent = currIdx;
    }

    void DDGIProbeUpdatePass::RelightProbes()
    {
        OLO_PROFILE_FUNCTION();

        const i32 t = m_Desc.HitCacheTexels;
        const glm::ivec2 radianceSize = m_TileDims * t;

        m_RadianceFB->Bind();
        SetFullscreenPassState();
        RenderCommand::SetViewport(0, 0, static_cast<u32>(radianceSize.x), static_cast<u32>(radianceSize.y));

        m_RelightShader->Bind();
        RenderCommand::BindTexture(0, m_HitFB->GetColorAttachmentRendererID(0));                             // hit albedo
        RenderCommand::BindTexture(1, m_HitFB->GetColorAttachmentRendererID(1));                             // hit geo
        RenderCommand::BindTexture(2, m_IrradianceFB[m_IrradianceCurrent]->GetColorAttachmentRendererID(0)); // prev irradiance (bounce)
        RenderCommand::BindTexture(3, m_VisibilityFB[m_VisibilityCurrent]->GetColorAttachmentRendererID(0)); // current visibility
        RenderCommand::BindTexture(4, m_ProbeDataTexture);

        // Global environment cubemap for sky texels, at the engine's canonical
        // samplerCube slot (TEX_ENVIRONMENT) — the black fallback keeps the
        // declared samplerCube valid when no scene environment exists, and the
        // slot normally carries this exact texture for the lit passes anyway.
        const u32 envID = Renderer3D::GetGlobalEnvironmentMapID() != 0
                              ? Renderer3D::GetGlobalEnvironmentMapID()
                              : m_BlackCubemap;
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_ENVIRONMENT, envID);

        // CSM + shadow atlas at the binding units include/PBRCommon.glsl's
        // evaluators expect (8 / 13 comparison, 33 / 34 raw for PCSS) — same
        // placeholder discipline as DeferredLightingPass / VolumetricFogPass.
        auto& shadowMap = Renderer3D::GetShadowMap();
        const u32 csmID = shadowMap.GetCSMRendererID() != 0 ? shadowMap.GetCSMRendererID()
                                                            : ShadowMap::GetCSMPlaceholderRendererID();
        const u32 atlasID = shadowMap.GetAtlasRendererID() != 0 ? shadowMap.GetAtlasRendererID()
                                                                : ShadowMap::GetAtlasPlaceholderRendererID();
        const u32 csmRawID = shadowMap.GetCSMRawRendererID() != 0 ? shadowMap.GetCSMRawRendererID()
                                                                  : ShadowMap::GetCSMRawPlaceholderRendererID();
        const u32 atlasRawID = shadowMap.GetAtlasRawRendererID() != 0 ? shadowMap.GetAtlasRawRendererID()
                                                                      : ShadowMap::GetAtlasRawPlaceholderRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW, csmID);
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_ATLAS, atlasID);
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_CSM_RAW, csmRawID);
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, atlasRawID);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();

        // RelightBudget maps probes to contiguous radiance-atlas tile rows
        // (linear probe index / dims.x == tile row); a wrap cursor covers the
        // whole atlas over successive frames with up to two scissored draws.
        const i32 tileRows = m_TileDims.y;
        i32 rowBudget = tileRows;
        if (m_Desc.RelightBudget > 0)
        {
            const i32 probesPerRow = std::max(m_Desc.Resolution.x, 1);
            rowBudget = std::clamp((m_Desc.RelightBudget + probesPerRow - 1) / probesPerRow, 1, tileRows);
        }

        if (rowBudget >= tileRows)
        {
            RenderCommand::DrawIndexed(va);
        }
        else
        {
            RenderCommand::EnableScissorTest();
            const i32 r0 = m_RelightRowCursor % tileRows;
            const i32 n1 = std::min(rowBudget, tileRows - r0);
            RenderCommand::SetScissorBox(0, r0 * t, radianceSize.x, n1 * t);
            RenderCommand::DrawIndexed(va);
            if (const i32 n2 = rowBudget - n1; n2 > 0)
            {
                RenderCommand::SetScissorBox(0, 0, radianceSize.x, n2 * t);
                RenderCommand::DrawIndexed(va);
            }
            RenderCommand::DisableScissorTest();
            m_RelightRowCursor = (r0 + rowBudget) % tileRows;
        }
    }

    void DDGIProbeUpdatePass::BlendIrradiance()
    {
        OLO_PROFILE_FUNCTION();

        const u32 prevIdx = m_IrradianceCurrent;
        const u32 currIdx = 1u - m_IrradianceCurrent;
        const glm::ivec2 irrSize = m_TileDims * DDGI::kIrradianceTileTexels;

        m_IrradianceFB[currIdx]->Bind();
        SetFullscreenPassState();
        RenderCommand::SetViewport(0, 0, static_cast<u32>(irrSize.x), static_cast<u32>(irrSize.y));

        m_BlendIrradianceShader->Bind();
        RenderCommand::BindTexture(0, m_RadianceFB->GetColorAttachmentRendererID(0));
        RenderCommand::BindTexture(1, m_HitFB->GetColorAttachmentRendererID(1)); // hit geo (backface flags)
        RenderCommand::BindTexture(2, m_IrradianceFB[prevIdx]->GetColorAttachmentRendererID(0));
        RenderCommand::BindTexture(3, m_ProbeDataTexture);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);

        // Swap AFTER the draw.
        m_IrradianceCurrent = currIdx;
    }

    void DDGIProbeUpdatePass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        (void)context;

        m_RanThisFrame = false;

        if (!m_VolumeSubmitted)
        {
            UploadDisabledUBO();
            m_Casters.clear();
            return;
        }
        m_VolumeSubmitted = false;

        if (!IsReadyForExecution())
        {
            UploadDisabledUBO();
            m_Casters.clear();
            return;
        }

        // Deliberate end-of-pass state restoration instead of a
        // GLStateGuard(Restore) wrapper: the guard's exit restore also
        // reverted the atlas publication at slots 56-58 (an INTENDED global
        // side effect, same shape as SetGlobalIBL) to the previous frame's
        // textures, and its per-frame "N mutations escaped" trace flooded the
        // log at ~90 lines/s. See the restore block before the publish step.
        const auto prevViewport = RenderCommand::GetViewport();

        // 1. Resource fingerprint / (re)create.
        EnsureResources();

        // Frame-derived volume values.
        m_RenderOrigin = Renderer3D::GetRenderOrigin();
        m_Spacing = DDGI::ProbeSpacing(m_Desc.BoundsMin, m_Desc.BoundsMax, m_Desc.Resolution);
        m_MinAxialSpacing = glm::min(glm::min(m_Spacing.x, m_Spacing.y), m_Spacing.z);
        m_MaxRayDistance = DDGI::kMaxRayDistanceSpacingScale * glm::length(m_Spacing);
        m_TileDims = DDGI::AtlasTileDimensions(m_Desc.Resolution);
        m_TotalProbes = static_cast<i32>(m_Records.size());
        ++m_FrameIndex;

        // Upload the DDGI UBO up front (Enabled = 1): the relight and blend
        // shaders read the volume block via include/DDGICommon.glsl.
        UploadVolumeUBO(true);

        // 2-4. Amortized capture: mini-G-buffer rasterization -> octahedral
        // resample -> CPU relocation/classification, per scheduled probe.
        const std::vector<i32> captureSet = PickCaptureSet(m_Desc.CaptureBudget);
        if (!captureSet.empty())
        {
            for (const i32 probeIdx : captureSet)
            {
                CaptureProbe(probeIdx);
                ResampleProbe(probeIdx);
                RelocateAndClassifyProbe(probeIdx);
            }

            // The capture overwrote UBO binding 0 with this pass's own camera
            // buffer. Invalidate the dispatch bind-cache FIRST so the re-upload
            // actually rebinds the engine camera UBO (BindUBOIfNeeded would
            // otherwise think it never left), then restore the real camera for
            // the relight stage's CSM cascade selection — it is re-established
            // AGAIN at the end of Execute, after the last Shader::Bind (whose
            // resource-registry side effects can re-bind UBO point 0).
            CommandDispatch::InvalidateRenderStateCache();
            CommandDispatch::UploadCameraUBO();

            // 5. Visibility blend — only captured probes carry new distances
            // (ADR: visibility updates at capture time, not per frame).
            BlendVisibility(captureSet);
        }

        // Once-in-a-while scheduling diagnostics (issue #632 bring-up): probe
        // state histogram + caster/capture counts, cheap enough to keep.
        if ((m_FrameIndex % 300u) == 1u)
        {
            i32 uncaptured = 0, active = 0, inactive = 0;
            for (const auto& rec : m_Records)
            {
                switch (rec.State)
                {
                    case DDGI::ProbeState::Uncaptured:
                        ++uncaptured;
                        break;
                    case DDGI::ProbeState::Active:
                        ++active;
                        break;
                    case DDGI::ProbeState::Inactive:
                        ++inactive;
                        break;
                }
            }
            OLO_CORE_INFO("DDGI: frame {} casters={} captured {}/frame, probes active={} inactive={} uncaptured={}",
                          m_FrameIndex, m_Casters.size(), captureSet.size(), active, inactive, uncaptured);
        }

        // 6. Relight every cached hit point with current direct lighting +
        // previous-frame probe irradiance (infinite bounce).
        RelightProbes();

        // 7. Cosine-convolve the relit radiance into the irradiance atlas
        // under adjusted-hysteresis EMA, ping-pong swap after.
        BlendIrradiance();

        // 8. Deliberate state restoration (replaces the GLStateGuard wrapper):
        // never leave an FBO bound, undo the fullscreen-stage depth/cull
        // flips, drop the pass-local texture bindings (§6.4 — soon-recreated
        // atlases must not dangle on live units), and re-establish the engine
        // camera UBO AFTER the last Shader::Bind of this pass.
        if (m_IrradianceFB[m_IrradianceCurrent])
        {
            m_IrradianceFB[m_IrradianceCurrent]->Unbind();
        }
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::EnableCulling();
        for (u32 unit = 0; unit <= 4; ++unit)
        {
            RenderCommand::BindTexture(unit, 0);
        }
        RenderCommand::SetViewport(prevViewport.x, prevViewport.y, prevViewport.width, prevViewport.height);
        CommandDispatch::InvalidateRenderStateCache();
        CommandDispatch::UploadCameraUBO();

        // 9. Publish: current (post-blend) atlases at the engine slots (an
        // intended cross-pass side effect, same shape as SetGlobalIBL), DDGI
        // UBO re-bound (uploaded Enabled=1 above). MUST come after the
        // restore block so nothing reverts the publication.
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_DDGI_IRRADIANCE, GetIrradianceAtlasID());
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_DDGI_VISIBILITY, GetVisibilityAtlasID());
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_DDGI_PROBE_DATA, m_ProbeDataTexture);
        m_DDGIUBO->Bind();

        m_Casters.clear();
        m_RanThisFrame = true;
    }

    void DDGIProbeUpdatePass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void DDGIProbeUpdatePass::ResizeFramebuffer(u32 width, u32 height)
    {
        // All DDGI targets are probe-grid-sized (screen-decoupled); only the
        // spec bookkeeping follows the viewport.
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }
} // namespace OloEngine
