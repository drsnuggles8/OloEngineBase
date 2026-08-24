#include "OloEnginePCH.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

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

        // Bounce-feedback albedo clamp (ADR 0007: albedo <= 0.9 in the bounce
        // term keeps the infinite-bounce feedback loop contractive).
        constexpr f32 kEnergyConservation = 0.9f;

        // Relocation-driven recapture. Since #707 the relocation runs on the
        // GPU, so the CPU cannot see whether a probe actually moved — it
        // schedules a FIXED number of follow-up captures instead and lets the
        // spring converge. The alternative, reading the offset back to decide,
        // is precisely the readback this issue removes.
        //
        // TWO, not the 4 the old adaptive rule used as its ceiling. As a FIXED
        // count every probe pays it, so 4 would triple the warm-up capture cost
        // for the many probes that need none at all (an unconstrained probe's
        // spring decays to zero on the first evaluation). The spring's step is
        // 0.35 of the residual, so two refinements close ~75% of the gap, and
        // the periodic refresh closes the rest without a dedicated schedule.
        constexpr u8 kMaxRelocationIterations = 2;

        // Work-group sizes, mirroring the local_size declarations in the
        // compute shaders. A mismatch is a silent under-dispatch (probes at
        // the tail never maintained), so they are named rather than inlined.
        constexpr u32 kMaintainGroupSize = 64;
        constexpr u32 kScreenRequestGroupSize = 8;
        constexpr u32 kRelocateGroupSize = 64;
        // The two dispatch strides live in DDGICommon.h so the GLSL mirrors have
        // a single C++ counterpart to be pinned against.
        using DDGI::kProbeDispatchStride;
        using DDGI::kScreenRequestStride;

        // Bit flags mirroring DDGI_PASS_FLAG_* in include/DDGIPassData.glsl.
        constexpr i32 kPassFlagCascadeShifted = 1;
        constexpr i32 kPassFlagDepthValid = 2;
        // This capture is a periodic refresh of an already-placed probe. The
        // relocation compute keeps the existing offset when it is set — see
        // DDGI_Relocate.comp for why re-placing a settled probe is a limit cycle.
        constexpr i32 kPassFlagRefreshCapture = 4;

        // The DDGI pass-local per-draw / per-dispatch block now lives in
        // UBOStructures (ShaderBindingLayout.h) rather than here, so
        // ShaderUBOSizeConsistencyTest can guard it against the GLSL
        // declaration in include/DDGIPassData.glsl — an unlisted block is
        // SKIPPED by that test, not failed, and this one is read by five
        // shaders.
        using DDGIPassDataUBO = UBOStructures::DDGIPassDataUBO;

        // One record per probe in SSBO_DDGI_PROBE_AUX. Mirrors
        // DDGIProbeAuxRecord in include/DDGIProbeBuffers.glsl, field for field.
        struct ProbeAuxRecordGPU
        {
            u32 LastRequestFrame;
            u32 ScreenRequestFrame;
            u32 State;
            u32 BounceHitCount;
            u32 Flags;
            f32 BounceWeightSum;
            f32 Pad0;
            f32 Pad1;
        };
        static_assert(sizeof(ProbeAuxRecordGPU) == 32, "ProbeAuxRecordGPU must mirror the DDGIProbeAuxBuffer std430 record");

        // The three places kMaxCascades exists — the math header, the UBO
        // struct and DDGI_MAX_CASCADES in DDGICommon.glsl — must agree. Two of
        // them are checkable here; the third is checked by the shader compiling
        // against a 512-byte block.
        static_assert(UBOStructures::DDGIVolumeUBO::MaxCascades == static_cast<u32>(DDGI::kMaxCascades),
                      "DDGIVolumeUBO's cascade arrays must be sized by DDGI::kMaxCascades");

        // Overloaded rather than converted (issue #691). The
        // atlases are framebuffer ATTACHMENTS and migrated to identities; the
        // 1x1 placeholder/white/probe-data textures this pass creates itself are
        // still native and belong to a later resource-grain slice.
        void SetAtlasTextureParams(RHI::ResourceHandle texture, RHI::Filter filter)
        {
            RenderCommand::SetTextureFilter(texture, filter, filter);
            RenderCommand::SetTextureWrap(texture, RHI::AddressMode::ClampToEdge);
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
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
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
        // passes sample them at engine slots 56/64/58), so the reachability
        // cull must never drop this pass — same reasoning as VolumetricFogPass.
        SetSideEffects(SideEffect::NeverCull);
    }

    DDGIProbeUpdatePass::~DDGIProbeUpdatePass()
    {
        DestroyResources();
        if (m_PlaceholderTexture.IsValid())
        {
            RenderCommand::DeleteTexture(m_PlaceholderTexture);
            m_PlaceholderTexture = RHI::NullResource;
        }
        if (m_WhiteTexture.IsValid())
        {
            RenderCommand::DeleteTexture(m_WhiteTexture);
            m_WhiteTexture = RHI::NullResource;
        }
        if (m_BlackCubemap.IsValid())
        {
            RenderCommand::DeleteTexture(m_BlackCubemap);
            m_BlackCubemap = RHI::NullResource;
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

        // Issue #707 compute stages.
        m_MaintainCompute = ComputeShader::Create("assets/shaders/compute/DDGI_ProbeMaintain.comp");
        m_RequestScreenCompute = ComputeShader::Create("assets/shaders/compute/DDGI_RequestScreen.comp");
        m_RequestProbeCompute = ComputeShader::Create("assets/shaders/compute/DDGI_RequestProbe.comp");
        m_RelocateCompute = ComputeShader::Create("assets/shaders/compute/DDGI_Relocate.comp");

        m_DDGIUBO = UniformBuffer::Create(UBOStructures::DDGIVolumeUBO::GetSize(),
                                          ShaderBindingLayout::UBO_DDGI);
        m_PassDataUBO = UniformBuffer::Create(sizeof(DDGIPassDataUBO),
                                              ShaderBindingLayout::UBO_USER_0);
        m_CaptureCameraUBO = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(),
                                                   ShaderBindingLayout::UBO_CAMERA);

        // Stats live for the process; the aux buffer is sized with the probe
        // grid and therefore created in EnsureResources.
        m_StatsSSBO = StorageBuffer::Create(static_cast<u32>(sizeof(ProbeStats)),
                                            ShaderBindingLayout::SSBO_DDGI_STATS,
                                            StorageBufferUsage::DynamicCopy);
        if (m_StatsSSBO)
        {
            m_StatsSSBO->ClearData();
        }

        if (!m_PlaceholderTexture.IsValid())
        {
            // 1x1 black RGBA16F — a single texture serves all three disabled
            // slots (sampler2D reads of .rg / .rgb / .w all see zero, and
            // state 0 == Uncaptured makes the sampler skip every probe).
            m_PlaceholderTexture = RenderCommand::CreateTexture2DHandle(1, 1, RHI::Format::RGBA16Float);
            RenderCommand::ClearTextureFloat(m_PlaceholderTexture, 0, glm::vec4(0.0f));
            SetAtlasTextureParams(m_PlaceholderTexture, RHI::Filter::Nearest);
        }
        if (!m_WhiteTexture.IsValid())
        {
            m_WhiteTexture = RenderCommand::CreateTexture2DHandle(1, 1, RHI::Format::RGBA8UNorm);
            constexpr u8 white[4] = { 255, 255, 255, 255 };
            RenderCommand::UploadTextureSubImage2D(m_WhiteTexture, 1, 1, RHI::Format::RGBA8UNorm, white);
            SetAtlasTextureParams(m_WhiteTexture, RHI::Filter::Nearest);
        }
        if (!m_BlackCubemap.IsValid())
        {
            // Environment fallback for the relight sky term when no global
            // IBL environment cubemap exists this frame.
            m_BlackCubemap = RenderCommand::CreateTextureCubemapHandle(1, 1, RHI::Format::RGBA16Float);
            RenderCommand::ClearTextureFloat(m_BlackCubemap, 0, glm::vec4(0.0f));
            RenderCommand::SetTextureFilter(m_BlackCubemap, RHI::Filter::Linear, RHI::Filter::Linear);
            RenderCommand::SetTextureWrap(m_BlackCubemap, RHI::AddressMode::ClampToEdge);
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

        // Scene depth for the #707 screen-request compute, remembered WITHOUT a
        // declared Read. That is deliberate and the comment is the contract:
        // this pass runs right after the shadow pass, long before anything
        // writes this frame's depth, so a declared read would reorder the whole
        // DDGI update behind the G-buffer. The compute wants the PREVIOUS
        // frame's depth anyway (paired with the previous frame's inverse
        // view-projection), and the request signal is quantized to probe cells
        // and kept alive for 16 frames — one frame of latency is invisible,
        // reordering the frame is not. Best-effort: an unresolvable handle just
        // disables the screen half of the request chain, which the camera seed
        // covers.
        m_SelectedSceneDepth = blackboard.Scene.SceneDepth;

        // Publish the pass-owned atlases into the graph so they appear in
        // RenderGraph::GetRegisteredResources() — without this, both
        // olo_render_list_targets and olo_render_capture_target were blind to
        // them and black/leaking probes could not be diagnosed from an agent
        // session without a full visual repro (issue #607). Import-only, the
        // FluidIntermediatesPass pattern: the pass renders into them through
        // its own FBOs and consumers sample them at engine slots 56/64/58, so
        // there is deliberately no Read/Write declaration to change ordering
        // or culling.
        //
        // EnsureResources() runs here as well as in Execute so the atlases
        // exist by Setup time on the very rebuild a volume submission
        // triggers (the VirtualGeometryPass "id 0 on first rebuild" lesson).
        // Both ping-pong atlases are imported under stable per-ping names —
        // "current" flips every blended frame and must not churn the
        // fingerprint (see the header comment on GetIrradianceAtlasID(ping)).
        // The raw ids are hashed into ComputeBlackboardFingerprint so a
        // Resolution/HitCacheTexels/CascadeCount recreate re-imports instead of
        // keeping a dangling id.
        if (m_VolumeSubmitted)
            EnsureResources();

        // ImportTextureHandle since issue #691, which left this
        // native because the diagnostics could not read a handle-imported
        // resource; Debug::NativeTextureIdForDiagnostics (#736) closed that.
        const auto importAtlas = [&builder](const char* name, RHI::ResourceHandle texture, RGResourceFormat format,
                                            u32 width, u32 height)
        {
            if (!texture.IsValid())
                return;
            RGResourceDesc desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, name);
            desc.Format = format;
            desc.Width = width;
            desc.Height = height;
            [[maybe_unused]] const RGTextureHandle handle = builder.ImportTextureHandle(name, texture, desc);
        };
        for (u32 ping = 0; ping < 2u; ++ping)
        {
            if (m_IrradianceFB[ping])
            {
                const auto& spec = m_IrradianceFB[ping]->GetSpecification();
                importAtlas(ping == 0 ? "DDGIIrradianceAtlas0" : "DDGIIrradianceAtlas1",
                            GetIrradianceAtlasID(ping), RGResourceFormat::RGBA16Float, spec.Width, spec.Height);
            }
            if (m_VisibilityFB[ping])
            {
                const auto& spec = m_VisibilityFB[ping]->GetSpecification();
                importAtlas(ping == 0 ? "DDGIVisibilityAtlas0" : "DDGIVisibilityAtlas1",
                            GetVisibilityAtlasID(ping), RGResourceFormat::RG16Float, spec.Width, spec.Height);
            }
        }
        if (m_ProbeDataTexture.IsValid())
        {
            const glm::ivec2 tileDims = DDGI::CascadedAtlasTileDimensions(m_ResourceResolution, m_ResourceCascadeCount);
            importAtlas("DDGIProbeData", m_ProbeDataTexture, RGResourceFormat::RGBA16Float,
                        static_cast<u32>(std::max(tileDims.x, 1)), static_cast<u32>(std::max(tileDims.y, 1)));
        }
    }

    bool DDGIProbeUpdatePass::IsReadyForExecution() const noexcept
    {
        return m_CaptureShader && m_CaptureShader->IsReady() &&
               m_ResampleShader && m_ResampleShader->IsReady() &&
               m_RelightShader && m_RelightShader->IsReady() &&
               m_BlendIrradianceShader && m_BlendIrradianceShader->IsReady() &&
               m_BlendVisibilityShader && m_BlendVisibilityShader->IsReady() &&
               m_MaintainCompute && m_MaintainCompute->IsValid() &&
               m_RequestScreenCompute && m_RequestScreenCompute->IsValid() &&
               m_RequestProbeCompute && m_RequestProbeCompute->IsValid() &&
               m_RelocateCompute && m_RelocateCompute->IsValid() &&
               m_DDGIUBO != nullptr && m_PassDataUBO != nullptr && m_CaptureCameraUBO != nullptr &&
               m_StatsSSBO != nullptr;
    }

    void DDGIProbeUpdatePass::SubmitVolume(const DDGIVolumeDesc& desc)
    {
        m_Desc = desc;
        m_Desc.Resolution = glm::max(m_Desc.Resolution, glm::ivec3(1));
        m_Desc.HitCacheTexels = DDGI::HitCacheResolutionForRayCount(m_Desc.HitCacheTexels * m_Desc.HitCacheTexels);
        m_Desc.Hysteresis = glm::clamp(m_Desc.Hysteresis, 0.0f, 0.98f);
        m_Desc.CaptureBudget = std::max(m_Desc.CaptureBudget, 1);
        m_Desc.CascadeCount = std::clamp(m_Desc.Cascaded ? m_Desc.CascadeCount : 1, 1, DDGI::kMaxCascades);
        m_Desc.BaseProbeSpacing = std::max(m_Desc.BaseProbeSpacing, 1e-3f);
        m_Desc.CascadeBlendBand = std::clamp(m_Desc.CascadeBlendBand, 0.0f, 0.9f);
        m_Desc.UpdateRateDivisor = DDGI::UpdateRateDivisor(DDGI::SnapUpdateRate(std::max(m_Desc.UpdateRateDivisor, 1)));
        m_Desc.CameraSeedRadius = std::max(m_Desc.CameraSeedRadius, 0.0f);
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

    RHI::ResourceHandle DDGIProbeUpdatePass::GetIrradianceAtlasID() const
    {
        return m_IrradianceFB[m_IrradianceCurrent]
                   ? m_IrradianceFB[m_IrradianceCurrent]->GetColorAttachmentHandle(0)
                   : RHI::NullResource;
    }

    RHI::ResourceHandle DDGIProbeUpdatePass::GetIrradianceAtlasID(const u32 pingIndex) const
    {
        if (pingIndex >= 2u || !m_IrradianceFB[pingIndex])
            return RHI::NullResource;
        return m_IrradianceFB[pingIndex]->GetColorAttachmentHandle(0);
    }

    RHI::ResourceHandle DDGIProbeUpdatePass::GetVisibilityAtlasID(const u32 pingIndex) const
    {
        if (pingIndex >= 2u || !m_VisibilityFB[pingIndex])
            return RHI::NullResource;
        return m_VisibilityFB[pingIndex]->GetColorAttachmentHandle(0);
    }

    RHI::ResourceHandle DDGIProbeUpdatePass::GetVisibilityAtlasID() const
    {
        return m_VisibilityFB[m_VisibilityCurrent]
                   ? m_VisibilityFB[m_VisibilityCurrent]->GetColorAttachmentHandle(0)
                   : RHI::NullResource;
    }

    RHI::ResourceHandle DDGIProbeUpdatePass::GetIrradianceAtlasHandle(const u32 pingIndex) const
    {
        if (pingIndex >= 2u || !m_IrradianceFB[pingIndex])
            return RHI::NullResource;
        return m_IrradianceFB[pingIndex]->GetColorAttachmentHandle(0);
    }

    RHI::ResourceHandle DDGIProbeUpdatePass::GetVisibilityAtlasHandle(const u32 pingIndex) const
    {
        if (pingIndex >= 2u || !m_VisibilityFB[pingIndex])
            return RHI::NullResource;
        return m_VisibilityFB[pingIndex]->GetColorAttachmentHandle(0);
    }

    RHI::ResourceHandle DDGIProbeUpdatePass::GetProbeDataTextureID() const
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
            if (r.Captured)
            {
                ++captured;
            }
        }
        return static_cast<f32>(captured) / static_cast<f32>(m_Records.size());
    }

    void DDGIProbeUpdatePass::ReadbackProbeDiagnostics() const
    {
        OLO_PROFILE_FUNCTION();

        // DELIBERATELY BLOCKING, and deliberately never called from Execute.
        // The whole point of issue #707's fourth upgrade is that the frame does
        // no GPU->CPU round trip; a diagnostic a human or a test asks for is a
        // different thing entirely, and hiding it behind an async ring would
        // make "read the probe table" answer with last-second-but-one data for
        // no benefit.
        if (m_Records.empty())
        {
            return;
        }

        if (m_StatsSSBO)
        {
            ProbeStats stats{};
            m_StatsSSBO->GetData(&stats, static_cast<u32>(sizeof(stats)));
            m_Stats = stats;
        }

        if (!m_ProbeAuxSSBO)
        {
            return;
        }

        std::vector<ProbeAuxRecordGPU> aux(m_Records.size());
        m_ProbeAuxSSBO->GetData(aux.data(), static_cast<u32>(aux.size() * sizeof(ProbeAuxRecordGPU)));

        // Probe data (relocation offsets + state) lives in a texture rather
        // than the aux buffer because the GATHER samples it; reading it back
        // needs the same shape the relocation compute writes.
        const glm::ivec2 tileDims = DDGI::CascadedAtlasTileDimensions(m_ResourceResolution, m_ResourceCascadeCount);
        std::vector<glm::vec4> probeData(static_cast<sizet>(std::max(tileDims.x, 1)) *
                                         static_cast<sizet>(std::max(tileDims.y, 1)));
        const bool probeDataRead =
            m_ProbeDataTexture.IsValid() &&
            RenderCommand::ReadTextureSubImage(m_ProbeDataTexture, 0, 0, 0, 0,
                                               static_cast<u32>(std::max(tileDims.x, 1)),
                                               static_cast<u32>(std::max(tileDims.y, 1)), 1,
                                               RHI::Format::RGBA32Float,
                                               probeData.size() * sizeof(glm::vec4), probeData.data());

        for (sizet i = 0; i < m_Records.size(); ++i)
        {
            ProbeRecord& rec = m_Records[i];
            const ProbeAuxRecordGPU& a = aux[i];
            rec.State = static_cast<DDGI::ProbeState>(std::clamp<i32>(static_cast<i32>(a.State), 0, 2));
            rec.BounceWeightSum = a.BounceWeightSum;
            rec.BounceHitCount = static_cast<i32>(a.BounceHitCount);
            // `Captured` is CPU-OWNED and deliberately NOT overwritten from
            // a.Flags here. A diagnostic that edited the capture scheduler's
            // state would make "look at the probe table" change what the next
            // frame does — and the two sides cannot drift anyway: both derive
            // the cascade-shift invalidation from the same two lattice origins
            // (BuildCascades / DDGI_ProbeMaintain.comp).

            if (probeDataRead)
            {
                const glm::ivec2 tile =
                    DDGI::CascadedProbeTileCoord(static_cast<i32>(i), m_ResourceResolution);
                if (tile.x >= 0 && tile.x < tileDims.x && tile.y >= 0 && tile.y < tileDims.y)
                {
                    const glm::vec4 texel =
                        probeData[static_cast<sizet>(tile.y) * static_cast<sizet>(tileDims.x) +
                                  static_cast<sizet>(tile.x)];
                    rec.OffsetN = glm::vec3(texel);
                }
            }
        }
    }

    DDGIProbeUpdatePass::ProbeStats DDGIProbeUpdatePass::GetProbeStats() const
    {
        ReadbackProbeDiagnostics();
        return m_Stats;
    }

    f32 DDGIProbeUpdatePass::GetBounceCoverage() const
    {
        ReadbackProbeDiagnostics();

        f64 sum = 0.0;
        i64 hits = 0;
        for (const ProbeRecord& r : m_Records)
        {
            // Uncaptured probes have no hit cache yet, and Inactive ones are
            // inside geometry and never relight — neither says anything about
            // the field's authoring.
            if (r.State != DDGI::ProbeState::Active)
            {
                continue;
            }
            sum += static_cast<f64>(r.BounceWeightSum);
            hits += static_cast<i64>(r.BounceHitCount);
        }
        // -1 = nothing to measure. See the header: a diagnostic that reports
        // "fine" when it means "unknown" is the failure mode it exists to fix.
        return (hits > 0) ? static_cast<f32>(sum / static_cast<f64>(hits)) : -1.0f;
    }

    void DDGIProbeUpdatePass::UploadDisabledUBO()
    {
        if (m_DDGIUBO)
        {
            UBOStructures::DDGIVolumeUBO ubo{};
            ubo.Enabled = 0;
            ubo.CascadeCount = 1;
            // A zero spacing would divide by ~0 in any cascade helper that ran
            // before the Enabled check. Every shader tests Enabled first, so
            // this is defensive only — but it is the same defensiveness
            // BuildCascades applies to unused cascade levels, and a disabled
            // UBO carrying values that are illegal to use is a trap for whoever
            // adds the next consumer.
            for (u32 level = 0; level < UBOStructures::DDGIVolumeUBO::MaxCascades; ++level)
            {
                ubo.CascadeSpacing[level] = glm::vec4(1.0f);
                ubo.CascadeOrigin[level] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            }
            ubo.ProbeSpacing = glm::vec4(1.0f);
            m_DDGIUBO->SetData(&ubo, sizeof(ubo));
            m_DDGIUBO->Bind();
        }
        if (m_PlaceholderTexture.IsValid())
        {
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_DDGI_IRRADIANCE, m_PlaceholderTexture, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_DDGI_VISIBILITY, m_PlaceholderTexture, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_DDGI_PROBE_DATA, m_PlaceholderTexture, RHI::HeapSlotLifetime::Persistent);
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
        m_ProbeAuxSSBO = nullptr;
        if (m_ProbeDataTexture.IsValid())
        {
            RenderCommand::DeleteTexture(m_ProbeDataTexture);
            m_ProbeDataTexture = RHI::NullResource;
        }
        m_ResourceResolution = glm::ivec3(0);
        m_ResourceHitTexels = 0;
        m_ResourceCascadeCount = 0;
        m_HaveCascadeHistory = false;
    }

    void DDGIProbeUpdatePass::EnsureResources()
    {
        OLO_PROFILE_FUNCTION();

        if (m_ResourceResolution == m_Desc.Resolution && m_ResourceHitTexels == m_Desc.HitCacheTexels &&
            m_ResourceCascadeCount == m_Desc.CascadeCount)
        {
            return;
        }

        DestroyResources();

        const glm::ivec2 tileDims = DDGI::CascadedAtlasTileDimensions(m_Desc.Resolution, m_Desc.CascadeCount);
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
            SetAtlasTextureParams(m_IrradianceFB[i]->GetColorAttachmentHandle(0), RHI::Filter::Linear);
            m_IrradianceFB[i]->Bind();
            m_IrradianceFB[i]->ClearAllAttachments(glm::vec4(0.0f), -1);
        }

        // Visibility ping-pong (RG16F, 16x16 tiles) — LINEAR.
        for (u32 i = 0; i < 2; ++i)
        {
            m_VisibilityFB[i] = makeAtlasFB(FramebufferTextureFormat::RG16F, DDGI::kVisibilityTileTexels);
            SetAtlasTextureParams(m_VisibilityFB[i]->GetColorAttachmentHandle(0), RHI::Filter::Linear);
            m_VisibilityFB[i]->Bind();
            m_VisibilityFB[i]->ClearAllAttachments(glm::vec4(0.0f), -1);
        }

        // Radiance cache (RGBA16F, HitCacheTexels tiles, no border) — NEAREST
        // (texelFetch-only consumer).
        m_RadianceFB = makeAtlasFB(FramebufferTextureFormat::RGBA16F, t);
        SetAtlasTextureParams(m_RadianceFB->GetColorAttachmentHandle(0), RHI::Filter::Nearest);
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
            SetAtlasTextureParams(m_HitFB->GetColorAttachmentHandle(0), RHI::Filter::Nearest);
            SetAtlasTextureParams(m_HitFB->GetColorAttachmentHandle(1), RHI::Filter::Nearest);
            m_HitFB->Bind();
            m_HitFB->ClearAttachment(0, glm::vec4(0.0f));
            // Geo cleared to "sky" so never-resampled tiles read as misses.
            m_HitFB->ClearAttachment(1, glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
        }

        // Capture target: 3x2 grid of cube faces, faceRes = 2 * HitCacheTexels.
        // RT0 RGBA8 albedo + gl_FrontFacing flag, RT1 RGBA16F octNormal +
        // linear distance, D32F depth. Probe-count independent — one probe is
        // captured at a time.
        {
            const i32 faceRes = 2 * t;
            FramebufferSpecification spec;
            spec.Width = static_cast<u32>(3 * faceRes);
            spec.Height = static_cast<u32>(2 * faceRes);
            spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RGBA16F,
                                 FramebufferTextureFormat::ShadowDepth };
            m_CaptureFB = Framebuffer::Create(spec);
            SetAtlasTextureParams(m_CaptureFB->GetColorAttachmentHandle(0), RHI::Filter::Nearest);
            SetAtlasTextureParams(m_CaptureFB->GetColorAttachmentHandle(1), RHI::Filter::Nearest);
        }

        // Probe data: one texel per probe (xyz = relocation offset normalized
        // by spacing, w = state). Written on the GPU since #707 (imageStore
        // from DDGI_Relocate.comp / DDGI_ProbeMaintain.comp); cleared to zero
        // == Uncaptured.
        m_ProbeDataTexture = RenderCommand::CreateTexture2DHandle(static_cast<u32>(tileDims.x),
                                                                  static_cast<u32>(tileDims.y), RHI::Format::RGBA16Float);
        RenderCommand::ClearTextureFloat(m_ProbeDataTexture, 0, glm::vec4(0.0f));
        SetAtlasTextureParams(m_ProbeDataTexture, RHI::Filter::Nearest);

        // Reset the CPU scheduling mirror — a new grid invalidates every record.
        const sizet total = static_cast<sizet>(m_Desc.Resolution.x) *
                            static_cast<sizet>(m_Desc.Resolution.y) *
                            static_cast<sizet>(m_Desc.Resolution.z) *
                            static_cast<sizet>(std::max(m_Desc.CascadeCount, 1));
        m_Records.assign(total, ProbeRecord{});
        m_CaptureCursor = 0;
        m_IrradianceCurrent = 0;
        m_VisibilityCurrent = 0;

        // Per-probe aux buffer (issue #707), zero-initialised: request frame 0
        // means "never requested", state 0 means Uncaptured, flags 0 means
        // "not captured" — the three defaults the shaders expect.
        m_ProbeAuxSSBO = StorageBuffer::Create(static_cast<u32>(total * sizeof(ProbeAuxRecordGPU)),
                                               ShaderBindingLayout::SSBO_DDGI_PROBE_AUX,
                                               StorageBufferUsage::DynamicCopy);
        if (m_ProbeAuxSSBO)
        {
            m_ProbeAuxSSBO->ClearData();
        }

        m_ResourceResolution = m_Desc.Resolution;
        m_ResourceHitTexels = m_Desc.HitCacheTexels;
        m_ResourceCascadeCount = m_Desc.CascadeCount;

        OLO_CORE_INFO("DDGIProbeUpdatePass: (re)created atlases for {} cascade(s) x {}x{}x{} probes ({} total), {} hit texels/probe",
                      m_Desc.CascadeCount, m_Desc.Resolution.x, m_Desc.Resolution.y, m_Desc.Resolution.z,
                      static_cast<u64>(total), t);
    }

    void DDGIProbeUpdatePass::BuildCascades()
    {
        OLO_PROFILE_FUNCTION();

        m_CascadeCount = std::clamp(m_Desc.CascadeCount, 1, DDGI::kMaxCascades);

        std::array<DDGI::CascadeGrid, DDGI::kMaxCascades> previous = m_Cascades;

        for (i32 level = 0; level < DDGI::kMaxCascades; ++level)
        {
            if (level >= m_CascadeCount)
            {
                // Unused levels still get a valid grid: the UBO array is fixed
                // size and a zero spacing there would divide by ~0 in any
                // shader that indexed past the count by mistake.
                m_Cascades[static_cast<sizet>(level)] = m_Cascades[0];
                continue;
            }
            if (m_Desc.Cascaded)
            {
                m_Cascades[static_cast<sizet>(level)] =
                    DDGI::MakeCameraCascade(level, m_CameraWorldPos, glm::vec3(m_Desc.BaseProbeSpacing),
                                            m_Desc.Resolution);
            }
            else
            {
                m_Cascades[static_cast<sizet>(level)] =
                    DDGI::MakeAuthoredCascade(m_Desc.BoundsMin, m_Desc.BoundsMax, m_Desc.Resolution);
            }
        }

        // Did any cascade's window move? A shift reassigns a slab of storage
        // coordinates to different world lattice points, and everything cached
        // for them is now for the wrong place — see DDGI_ProbeMaintain.comp.
        m_CascadeShifted = false;
        for (i32 level = 0; level < m_CascadeCount; ++level)
        {
            const glm::ivec3 prevMin = m_HaveCascadeHistory ? previous[static_cast<sizet>(level)].LatticeMin
                                                            : m_Cascades[static_cast<sizet>(level)].LatticeMin;
            m_PrevLattice[static_cast<sizet>(level)] = prevMin;
            if (prevMin != m_Cascades[static_cast<sizet>(level)].LatticeMin)
            {
                m_CascadeShifted = true;
            }
        }
        for (i32 level = m_CascadeCount; level < DDGI::kMaxCascades; ++level)
        {
            m_PrevLattice[static_cast<sizet>(level)] = m_Cascades[static_cast<sizet>(level)].LatticeMin;
        }

        // Mirror the invalidation on the CPU side so the capture scheduler does
        // not keep believing a moved probe is captured. The GPU does the same
        // thing from the same two lattice origins in DDGI_ProbeMaintain.comp;
        // the two must agree, which is why both derive it rather than one
        // telling the other (that would be a readback).
        if (m_CascadeShifted && !m_Records.empty())
        {
            const i32 perCascade = DDGI::ProbesPerCascade(m_Desc.Resolution);
            for (i32 level = 0; level < m_CascadeCount; ++level)
            {
                const DDGI::CascadeGrid& curr = m_Cascades[static_cast<sizet>(level)];
                DDGI::CascadeGrid prev = curr;
                prev.LatticeMin = m_PrevLattice[static_cast<sizet>(level)];
                if (prev.LatticeMin == curr.LatticeMin)
                {
                    continue;
                }
                for (i32 local = 0; local < perCascade; ++local)
                {
                    const glm::ivec3 storage = DDGI::ProbeGridCoord(local, m_Desc.Resolution);
                    if (DDGI::LatticeForStorageCoord(storage, prev) == DDGI::LatticeForStorageCoord(storage, curr))
                    {
                        continue;
                    }
                    const sizet idx = static_cast<sizet>(level) * static_cast<sizet>(perCascade) +
                                      static_cast<sizet>(local);
                    if (idx < m_Records.size())
                    {
                        m_Records[idx] = ProbeRecord{};
                    }
                }
            }
        }

        m_HaveCascadeHistory = true;
    }

    void DDGIProbeUpdatePass::UploadVolumeUBO(bool enabled)
    {
        UBOStructures::DDGIVolumeUBO ubo{};
        const DDGI::CascadeGrid& c0 = m_Cascades[0];
        ubo.BoundsMin = glm::vec4(DDGI::CascadeBoundsMin(c0) - m_RenderOrigin, 0.0f);
        ubo.BoundsMax = glm::vec4(DDGI::CascadeBoundsMax(c0) - m_RenderOrigin, 0.0f);
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
        // Issue #751: the infinite-bounce gather reaches this far past the
        // bounds. Not authored — it is a property of the probe field's own
        // sample density, see DDGI::kBounceMarginSpacingScale.
        ubo.BounceMarginScale = DDGI::kBounceMarginSpacingScale;
        // Issue #707.
        ubo.CascadeCount = m_CascadeCount;
        ubo.CascadeBlendBand = m_Desc.CascadeBlendBand;
        ubo.UpdateRateDivisor = std::max(m_Desc.UpdateRateDivisor, 1);
        ubo.RequestLifetime = static_cast<i32>(DDGI::kProbeRequestLifetimeFrames);
        ubo.SparsityEnabled = m_Desc.SparsityEnabled ? 1 : 0;

        for (i32 level = 0; level < DDGI::kMaxCascades; ++level)
        {
            const DDGI::CascadeGrid& g = m_Cascades[static_cast<sizet>(level)];
            const f32 minAxial = glm::min(glm::min(g.Spacing.x, g.Spacing.y), g.Spacing.z);
            const f32 maxRay = DDGI::kMaxRayDistanceSpacingScale * glm::length(g.Spacing);
            // The lattice ORIGIN is shifted by the render origin, not the
            // bounds: every probe position is origin + lattice * spacing, so
            // shifting the origin shifts the whole lattice consistently and the
            // integer lattice coordinates stay absolute (they must — they are
            // the toroidal storage key and a rebase must not renumber them).
            ubo.CascadeOrigin[level] = glm::vec4(g.Origin - m_RenderOrigin, maxRay);
            ubo.CascadeSpacing[level] = glm::vec4(g.Spacing, minAxial);
            ubo.CascadeLattice[level] = glm::ivec4(g.LatticeMin, 0);
        }

        m_DDGIUBO->SetData(&ubo, sizeof(ubo));
        m_DDGIUBO->Bind();
    }

    void DDGIProbeUpdatePass::UploadComputeParams(i32 probeIndexOrTotal, i32 flags)
    {
        DDGIPassDataUBO data{};
        data.Model = glm::mat4(1.0f);
        data.NormalMatrix = glm::mat4(1.0f);
        data.BaseColor = glm::vec4(1.0f);
        data.ProbePosition = glm::vec4(0.0f, 0.0f, 0.0f, static_cast<f32>(probeIndexOrTotal));
        data.InvViewProjection = m_PrevWorldInvViewProjection;
        data.RenderOrigin = glm::vec4(m_RenderOrigin, m_Desc.CameraSeedRadius);
        data.CameraPosRel = glm::vec4(m_CameraWorldPos - m_RenderOrigin, 0.0f);
        data.ComputeParams = glm::ivec4(m_TotalProbes,
                                        static_cast<i32>(m_FramebufferSpec.Width),
                                        static_cast<i32>(m_FramebufferSpec.Height),
                                        flags);
        for (i32 level = 0; level < DDGI::kMaxCascades; ++level)
        {
            data.PrevLattice[level] = glm::ivec4(m_PrevLattice[static_cast<sizet>(level)], 0);
        }
        m_PassDataUBO->SetData(&data, sizeof(data));
        m_PassDataUBO->Bind();
    }

    void DDGIProbeUpdatePass::SetPassDataProbe(i32 probeIdx)
    {
        UploadComputeParams(probeIdx, 0);
    }

    void DDGIProbeUpdatePass::BindProbeBuffers() const
    {
        if (m_ProbeAuxSSBO)
        {
            m_ProbeAuxSSBO->Bind();
        }
        if (m_StatsSSBO)
        {
            m_StatsSSBO->Bind();
        }
    }

    glm::vec3 DDGIProbeUpdatePass::ProbeGridWorldPosition(i32 probeIdx) const
    {
        const i32 level = std::clamp(DDGI::CascadeOfProbeIndex(probeIdx, m_Desc.Resolution), 0, m_CascadeCount - 1);
        const glm::ivec3 storage = DDGI::StorageCoordOfProbeIndex(probeIdx, m_Desc.Resolution);
        return DDGI::CascadeProbeGridPosition(storage, m_Cascades[static_cast<sizet>(level)]);
    }

    bool DDGIProbeUpdatePass::IsProbeCpuLive(i32 probeIndex) const
    {
        // The CPU's own liveness proxy, and it is a PROXY on purpose.
        //
        // The GPU knows exactly which probes are requested — but capture is
        // rasterization, issued by the CPU as six draws per probe, so the CPU
        // has to choose the capture set. Asking the GPU would mean reading the
        // request buffer back, which is the readback issue #707 exists to
        // delete. So the CPU derives its own answer from the camera and the
        // cascade layout: a probe is worth capturing if it is inside (or just
        // outside) the view frustum, or close enough to the camera that a turn
        // would bring it in.
        //
        // Being conservative is the right failure direction here: capturing a
        // probe nothing ends up shading wastes a slice of a small budget, while
        // MISSING one leaves a live probe permanently uncaptured and therefore
        // permanently absent from the gather.
        if (!m_Desc.SparsityEnabled)
        {
            return true;
        }

        const i32 level = std::clamp(DDGI::CascadeOfProbeIndex(probeIndex, m_Desc.Resolution), 0, m_CascadeCount - 1);
        const glm::vec3 worldPos = ProbeGridWorldPosition(probeIndex);
        const glm::vec3 spacing = m_Cascades[static_cast<sizet>(level)].Spacing;

        const glm::vec3 toCamera = worldPos - m_CameraWorldPos;
        const f32 nearReach = m_Desc.CameraSeedRadius + glm::length(spacing);
        if (glm::dot(toCamera, toCamera) <= nearReach * nearReach)
        {
            return true;
        }

        // THE PROXY MUST BE A SUPERSET OF WHAT THE GPU CAN REQUEST, and the
        // dilation below is what makes it one.
        //
        // The GPU has three request sources. Screen pixels are inside the
        // frustum and the camera seed is the sphere above — but the ONE
        // INDIRECTION hop (DDGI_RequestProbe.comp) marks the probes around a
        // live probe's cached HIT POINTS, and a hit point can sit up to
        // kMaxRayDistanceSpacingScale spacings behind the probe, i.e. outside
        // the frustum entirely. A plain frustum test therefore leaves those
        // probes live on the GPU and invisible to the CPU capture scheduler
        // forever: never captured, never Active, never in the gather.
        //
        // That is not hypothetical — it shipped in the first version of this
        // function and showed up as 30 of 509 live probes stuck uncaptured in a
        // steady state 910 frames long, which no test then in the tree could
        // see. They are also the WORST 30 to lose: the off-screen surfaces the
        // hop exists to bring in are exactly the ones carrying the bounce that
        // the screen-visible probes cannot supply.
        //
        // The hop's reach is bounded — one max-ray-distance to the hit point,
        // plus one cell to the probes gathered around it — so dilating the
        // frustum by that radius closes the gap exactly, with no unbounded
        // over-capture.
        const f32 hopReach = DDGI::kMaxRayDistanceSpacingScale * glm::length(spacing) + glm::length(spacing);
        return m_ViewFrustum.IsSphereVisible(worldPos, hopReach);
    }

    std::vector<i32> DDGIProbeUpdatePass::PickCaptureSet(i32 budget)
    {
        // The authored single-volume path keeps the EXACT #632 scheduler: its
        // reference-path-tracer parity and its goldens were measured against
        // that capture order, and "equivalent coverage" is not the same claim
        // as "same order" when the thing being compared is a temporally
        // converged field.
        if (!m_Desc.Cascaded && !m_Desc.SparsityEnabled)
        {
            return PickCaptureSetLegacy(budget);
        }
        return PickCaptureSetPrioritized(budget);
    }

    std::vector<i32> DDGIProbeUpdatePass::PickCaptureSetLegacy(i32 budget)
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

        // 1) Never-captured probes, linear cursor. Scan indices derive from a
        //    FROZEN copy of the cursor: advancing m_CaptureCursor on each
        //    selection while also using it in the index expression made the
        //    scan jump ahead by the accumulated selections, skipping probes
        //    and underfilling the budget within a single frame's pass.
        //
        //    COVERAGE BEFORE REFINEMENT, and the order matters more since #707:
        //    an uncaptured probe contributes NOTHING to the gather, while a
        //    probe awaiting a relocation refinement is already contributing —
        //    slightly off, but contributing. Putting refinement first let the
        //    first captured batch hold the entire budget for its follow-up
        //    passes, which multiplies the time to full coverage by
        //    kMaxRelocationIterations.
        const i32 scanStart = m_CaptureCursor;
        for (i32 n = 0; n < total && static_cast<i32>(result.size()) < budget; ++n)
        {
            const i32 idx = (scanStart + n) % total;
            if (picked[idx] == 0u && !m_Records[idx].Captured)
            {
                result.push_back(idx);
                picked[idx] = 1u;
                m_CaptureCursor = (idx + 1) % total;
            }
        }

        // 2) Probes whose relocation may still be settling — recapture from the
        //    new spot, with whatever budget step 1 left.
        for (i32 i = 0; i < total && static_cast<i32>(result.size()) < budget; ++i)
        {
            if (picked[i] == 0u && m_Records[i].PendingRelocationRecapture)
            {
                result.push_back(i);
                picked[i] = 1u;
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
                if (picked[i] == 0u && m_Records[i].Captured && m_Records[i].LastCaptureFrame < oldestFrame)
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

    std::vector<i32> DDGIProbeUpdatePass::PickCaptureSetPrioritized(i32 budget)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<i32> result;
        const i32 total = m_TotalProbes;
        if (total <= 0 || budget <= 0)
        {
            return result;
        }
        budget = std::min(budget, total);

        // Scored scan over the whole field. O(total) per frame, which for the
        // default 4 x 16^3 is 16k cheap iterations — far below the cost of the
        // six draws each selected probe then triggers, so a cleverer structure
        // would optimise the wrong end.
        struct Candidate
        {
            i32 Index;
            f32 Score; // lower is more urgent
        };
        std::vector<Candidate> candidates;
        candidates.reserve(static_cast<sizet>(budget) * 8u);

        for (i32 i = 0; i < total; ++i)
        {
            const ProbeRecord& rec = m_Records[static_cast<sizet>(i)];
            if (!IsProbeCpuLive(i))
            {
                continue;
            }

            const glm::vec3 worldPos = ProbeGridWorldPosition(i);
            const f32 distance = glm::length(worldPos - m_CameraWorldPos);
            const i32 level = std::clamp(DDGI::CascadeOfProbeIndex(i, m_Desc.Resolution), 0, m_CascadeCount - 1);

            // The tiering itself lives in DDGI::CaptureScore, with its own
            // contract test — the ORDER between tiers is the thing that fails
            // silently here (everything still gets captured, just far later),
            // so it is a pinned pure function rather than a rule written in a
            // comment next to the code that implements it.
            const DDGI::CaptureTier tier = !rec.Captured                    ? DDGI::CaptureTier::NeverCaptured
                                           : rec.PendingRelocationRecapture ? DDGI::CaptureTier::RelocationRefinement
                                                                            : DDGI::CaptureTier::PeriodicRefresh;
            const u32 age = (m_FrameIndex >= rec.LastCaptureFrame) ? (m_FrameIndex - rec.LastCaptureFrame) : 0u;
            candidates.push_back({ i, DDGI::CaptureScore(tier, distance, level, age) });
        }

        if (candidates.empty())
        {
            return result;
        }

        const sizet keep = std::min(static_cast<sizet>(budget), candidates.size());
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(keep), candidates.end(),
                          [](const Candidate& a, const Candidate& b)
                          {
                              // Index as the tiebreak: an unstable capture order
                              // across frames would make convergence
                              // non-reproducible, which is the one property a
                              // temporal algorithm's tests depend on.
                              // Written as two ordered comparisons rather than
                              // `a.Score != b.Score ? ... : ...`: exact
                              // equality on a float is banned here
                              // (docs/agent-rules/cpp-coding-quality.md §2) and
                              // this form is also the one std::sort's strict
                              // weak ordering wants.
                              if (a.Score < b.Score)
                              {
                                  return true;
                              }
                              if (b.Score < a.Score)
                              {
                                  return false;
                              }
                              return a.Index < b.Index;
                          });

        // Refresh throttle, same 1/8 rule as the legacy path: once every live
        // probe is captured AND settled, only a slice of the budget
        // re-rasterizes. The threshold is DERIVED from the tier bias rather
        // than written as a literal, so raising kCaptureTierBias cannot leave
        // this comparison quietly classifying every probe as urgent.
        constexpr f32 kRefreshTierFloor =
            static_cast<f32>(std::to_underlying(DDGI::CaptureTier::PeriodicRefresh)) * DDGI::kCaptureTierBias;
        const bool anyUrgent = candidates.front().Score < kRefreshTierFloor;
        const i32 effectiveBudget = anyUrgent ? budget : std::max(1, budget / 8);

        result.reserve(static_cast<sizet>(effectiveBudget));
        for (sizet i = 0; i < keep && static_cast<i32>(result.size()) < effectiveBudget; ++i)
        {
            result.push_back(candidates[i].Index);
        }
        return result;
    }

    void DDGIProbeUpdatePass::DispatchProbeMaintain()
    {
        OLO_PROFILE_FUNCTION();

        if (m_TotalProbes <= 0 || !m_MaintainCompute)
        {
            return;
        }

        m_MaintainCompute->Bind();
        BindProbeBuffers();
        UploadComputeParams(m_TotalProbes, m_CascadeShifted ? kPassFlagCascadeShifted : 0);
        HeapBinding::BindImageOrOffset(0, m_ProbeDataTexture, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::RGBA16Float, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        const u32 groups = (static_cast<u32>(m_TotalProbes) + kMaintainGroupSize - 1u) / kMaintainGroupSize;
        RenderCommand::DispatchCompute(groups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::ShaderImageAccess |
                                     MemoryBarrierFlags::TextureFetch);
    }

    void DDGIProbeUpdatePass::DispatchScreenRequests()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Desc.SparsityEnabled || !m_RequestScreenCompute || !m_SceneDepthID.IsValid())
        {
            return;
        }
        if (!m_HavePrevViewProjection || m_FramebufferSpec.Width == 0 || m_FramebufferSpec.Height == 0)
        {
            return;
        }

        m_RequestScreenCompute->Bind();
        BindProbeBuffers();
        UploadComputeParams(m_TotalProbes, kPassFlagDepthValid);
        HeapBinding::BindTextureOrOffset(0, m_SceneDepthID, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        const u32 samplesX = (m_FramebufferSpec.Width + kScreenRequestStride - 1u) / kScreenRequestStride;
        const u32 samplesY = (m_FramebufferSpec.Height + kScreenRequestStride - 1u) / kScreenRequestStride;
        const u32 groupsX = (samplesX + kScreenRequestGroupSize - 1u) / kScreenRequestGroupSize;
        const u32 groupsY = (samplesY + kScreenRequestGroupSize - 1u) / kScreenRequestGroupSize;
        RenderCommand::DispatchCompute(std::max(groupsX, 1u), std::max(groupsY, 1u), 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    void DDGIProbeUpdatePass::DispatchProbeRequests()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Desc.SparsityEnabled || !m_RequestProbeCompute || m_TotalProbes <= 0 || !m_HitFB)
        {
            return;
        }

        m_RequestProbeCompute->Bind();
        BindProbeBuffers();
        UploadComputeParams(m_TotalProbes, 0);
        HeapBinding::BindTextureOrOffset(0, m_HitFB->GetColorAttachmentHandle(1), RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        // One work group per probe — the group early-outs uniformly for probes
        // that are not screen-requested, which is the vast majority.
        const u32 probeCount = static_cast<u32>(m_TotalProbes);
        const u32 groupsX = std::min(probeCount, kProbeDispatchStride);
        const u32 groupsY = (probeCount + kProbeDispatchStride - 1u) / kProbeDispatchStride;
        RenderCommand::DispatchCompute(std::max(groupsX, 1u), std::max(groupsY, 1u), 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    void DDGIProbeUpdatePass::CaptureProbe(i32 probeIdx)
    {
        OLO_PROFILE_FUNCTION();

        // The CPU knows only the LATTICE position; the relocation offset lives
        // on the GPU since #707. That is fine for both things the CPU needs it
        // for — the caster cull radius (widened by the maximum offset below)
        // and the far plane — while the capture's own eye position is derived
        // in DDGI_Capture.glsl's vertex stage from the probe-data texture.
        const glm::vec3 probeGridWorld = ProbeGridWorldPosition(probeIdx);
        const i32 level = std::clamp(DDGI::CascadeOfProbeIndex(probeIdx, m_Desc.Resolution), 0, m_CascadeCount - 1);
        const glm::vec3 spacing = m_Cascades[static_cast<sizet>(level)].Spacing;

        const i32 faceRes = 2 * m_Desc.HitCacheTexels;
        const f32 maxRayDistance = DDGI::kMaxRayDistanceSpacingScale * glm::length(spacing);
        const f32 farPlane = 2.0f * maxRayDistance;
        // Relocation can move the probe by up to kMaxProbeOffsetFraction of a
        // cell, and the CPU cannot see how far it actually did — so the cull
        // radius is widened by the worst case rather than by the actual offset.
        const f32 cullRadius = farPlane + DDGI::kMaxProbeOffsetFraction * glm::length(spacing);

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
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);

        m_CaptureShader->Bind();
        m_CaptureCameraUBO->Bind();
        m_PassDataUBO->Bind();
        // The vertex stage reads the probe's relocated position from here.
        HeapBinding::BindTextureOrOffset(1, m_ProbeDataTexture, RHI::HeapSlotLifetime::Persistent);

        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, farPlane);

        for (u32 face = 0; face < 6; ++face)
        {
            // EYE AT THE ORIGIN (issue #707). The view is rotation-only and the
            // vertex stage subtracts the probe position itself, because that
            // position now lives on the GPU. Analytically this is the same
            // matrix the pre-#707 lookAt(probeRel, ...) produced, with the
            // translation moved from the matrix into the shader.
            const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), kFaceTargets[face], kFaceUps[face]);
            const glm::mat4 vp = proj * view;

            // A8 seam, CAPTURE flavour (#691): the z remap
            // without the y flip. The atlas is DIRECTION-addressed by the
            // relight, so the screen-space y flip would store every face
            // row-mirrored relative to the GL bake while direction->texel
            // addressing stayed API-identical — see
            // RHIProjectionSeam.h's AdjustCaptureProjectionForBackend.
            UBOStructures::CameraUBO camera{};
            camera.ViewProjection = RHI::AdjustCaptureProjectionForBackend(vp);
            camera.View = view;
            camera.Projection = RHI::AdjustCaptureProjectionForBackend(proj);
            camera.Position = glm::vec3(0.0f);
            camera.Pad0 = 0.0f;
            camera.PrevViewProjection = RHI::AdjustCaptureProjectionForBackend(vp);
            camera.RenderOrigin = m_RenderOrigin;
            // Capture flavour's reconstruction sibling = the raw matrix (#691).
            camera.ProjectionForReconstruction = proj;
            m_CaptureCameraUBO->SetData(&camera, UBOStructures::CameraUBO::GetSize());

            RenderCommand::SetViewport(static_cast<u32>((face % 3u) * static_cast<u32>(faceRes)),
                                       static_cast<u32>((face / 3u) * static_cast<u32>(faceRes)),
                                       static_cast<u32>(faceRes), static_cast<u32>(faceRes));

            for (const auto& caster : m_Casters)
            {
                if (!caster.vaoID.IsValid() || caster.indexCount == 0)
                {
                    continue;
                }
                if (!CasterInRange(caster, probeGridWorld, cullRadius))
                {
                    continue;
                }

                if (caster.albedoTextureID.IsValid())
                    HeapBinding::BindTextureOrOffset(0, caster.albedoTextureID, RHI::HeapSlotLifetime::Persistent);
                else
                    HeapBinding::BindTextureOrOffset(0, m_WhiteTexture, RHI::HeapSlotLifetime::Persistent);

                DDGIPassDataUBO data{};
                data.Model = MakeModelRelative(caster.transform, m_RenderOrigin);
                data.NormalMatrix = glm::transpose(glm::inverse(data.Model));
                data.BaseColor = caster.baseColor;
                data.ProbePosition = glm::vec4(probeGridWorld - m_RenderOrigin, static_cast<f32>(probeIdx));
                m_PassDataUBO->SetData(&data, sizeof(data));

                HeapBinding::FlushOffsets();
                RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount, caster.baseIndex);
            }
        }
    }

    void DDGIProbeUpdatePass::ResampleProbe(i32 probeIdx)
    {
        OLO_PROFILE_FUNCTION();

        const i32 t = m_Desc.HitCacheTexels;
        const glm::ivec2 tile = DDGI::CascadedProbeTileCoord(probeIdx, m_Desc.Resolution);

        m_HitFB->Bind();
        SetFullscreenPassState();
        RenderCommand::SetViewport(static_cast<u32>(tile.x * t), static_cast<u32>(tile.y * t),
                                   static_cast<u32>(t), static_cast<u32>(t));

        m_ResampleShader->Bind();
        HeapBinding::BindTextureOrOffset(0, m_CaptureFB->GetColorAttachmentHandle(0), RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindTextureOrOffset(1, m_CaptureFB->GetColorAttachmentHandle(1), RHI::HeapSlotLifetime::Persistent);
        SetPassDataProbe(probeIdx);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        HeapBinding::FlushOffsets();
        RenderCommand::DrawIndexed(va);
    }

    void DDGIProbeUpdatePass::RelocateProbeGPU(i32 probeIdx, bool refreshCapture)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_RelocateCompute || !m_HitFB)
        {
            return;
        }

        m_RelocateCompute->Bind();
        BindProbeBuffers();
        UploadComputeParams(probeIdx, refreshCapture ? kPassFlagRefreshCapture : 0);
        HeapBinding::BindImageOrOffset(0, m_ProbeDataTexture, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::RGBA16Float, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindTextureOrOffset(1, m_HitFB->GetColorAttachmentHandle(1), RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        // One work group, cooperatively reducing the whole hit tile.
        static_assert(kRelocateGroupSize == 64u, "kRelocateGroupSize must match DDGI_RELOCATE_GROUP");
        RenderCommand::DispatchCompute(1, 1, 1);
    }

    void DDGIProbeUpdatePass::BlendVisibility(const std::vector<i32>& capturedProbes)
    {
        OLO_PROFILE_FUNCTION();

        const u32 prevIdx = m_VisibilityCurrent;
        const u32 currIdx = 1u - m_VisibilityCurrent;
        const RHI::ResourceHandle prevTex = m_VisibilityFB[prevIdx]->GetColorAttachmentHandle(0);
        const RHI::ResourceHandle currTex = m_VisibilityFB[currIdx]->GetColorAttachmentHandle(0);
        const glm::ivec2 visSize = m_TileDims * DDGI::kVisibilityTileTexels;

        // Carry every un-recaptured tile forward, then overwrite only the
        // captured probes' tiles below.
        RenderCommand::CopyImageSubData(prevTex, RendererAPI::TextureTargetType::Texture2D,
                                        currTex, RendererAPI::TextureTargetType::Texture2D,
                                        static_cast<u32>(visSize.x), static_cast<u32>(visSize.y));

        m_VisibilityFB[currIdx]->Bind();
        SetFullscreenPassState();

        m_BlendVisibilityShader->Bind();
        HeapBinding::BindTextureOrOffset(0, m_HitFB->GetColorAttachmentHandle(1), RHI::HeapSlotLifetime::Persistent); // hit geo (dist + flag)
        HeapBinding::BindTextureOrOffset(1, prevTex, RHI::HeapSlotLifetime::Persistent);                              // EMA history

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();

        for (const i32 probeIdx : capturedProbes)
        {
            const glm::ivec2 tile = DDGI::CascadedProbeTileCoord(probeIdx, m_Desc.Resolution);
            RenderCommand::SetViewport(static_cast<u32>(tile.x * DDGI::kVisibilityTileTexels),
                                       static_cast<u32>(tile.y * DDGI::kVisibilityTileTexels),
                                       static_cast<u32>(DDGI::kVisibilityTileTexels),
                                       static_cast<u32>(DDGI::kVisibilityTileTexels));
            SetPassDataProbe(probeIdx);
            HeapBinding::FlushOffsets();
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
        BindProbeBuffers();
        HeapBinding::BindTextureOrOffset(0, m_HitFB->GetColorAttachmentHandle(0), RHI::HeapSlotLifetime::Persistent);                             // hit albedo
        HeapBinding::BindTextureOrOffset(1, m_HitFB->GetColorAttachmentHandle(1), RHI::HeapSlotLifetime::Persistent);                             // hit geo
        HeapBinding::BindTextureOrOffset(2, m_IrradianceFB[m_IrradianceCurrent]->GetColorAttachmentHandle(0), RHI::HeapSlotLifetime::Persistent); // prev irradiance (bounce)
        HeapBinding::BindTextureOrOffset(3, m_VisibilityFB[m_VisibilityCurrent]->GetColorAttachmentHandle(0), RHI::HeapSlotLifetime::Persistent); // current visibility
        HeapBinding::BindTextureOrOffset(4, m_ProbeDataTexture, RHI::HeapSlotLifetime::Persistent);

        // Global environment cubemap for sky texels, at the engine's canonical
        // samplerCube slot (TEX_ENVIRONMENT) — the black fallback keeps the
        // declared samplerCube valid when no scene environment exists, and the
        // slot normally carries this exact texture for the lit passes anyway.
        if (const RHI::ResourceHandle envMap = Renderer3D::GetGlobalEnvironmentMapHandle(); envMap.IsValid())
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_ENVIRONMENT, envMap,
                                                     RHI::HeapSlotLifetime::Persistent, {},
                                                     RHI::NullSamplerKind::Cube);
        else
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_ENVIRONMENT, m_BlackCubemap,
                                                     RHI::HeapSlotLifetime::Persistent, {},
                                                     RHI::NullSamplerKind::Cube);

        // CSM + shadow atlas at the binding units include/PBRCommon.glsl's
        // evaluators expect (8 / 13 comparison, 33 / 34 raw for PCSS) — same
        // placeholder discipline as DeferredLightingPass / VolumetricFogPass.
        auto& shadowMap = Renderer3D::GetShadowMap();
        const RHI::ResourceHandle csmID = shadowMap.GetCSMHandle().IsValid()
                                              ? shadowMap.GetCSMHandle()
                                              : ShadowMap::GetCSMPlaceholderHandle();
        const RHI::ResourceHandle atlasID = shadowMap.GetAtlasHandle().IsValid()
                                                ? shadowMap.GetAtlasHandle()
                                                : ShadowMap::GetAtlasPlaceholderHandle();
        const RHI::ResourceHandle csmRawID = shadowMap.GetCSMRawHandle().IsValid()
                                                 ? shadowMap.GetCSMRawHandle()
                                                 : ShadowMap::GetCSMRawPlaceholderHandle();
        const RHI::ResourceHandle atlasRawID = shadowMap.GetAtlasRawHandle().IsValid()
                                                   ? shadowMap.GetAtlasRawHandle()
                                                   : ShadowMap::GetAtlasRawPlaceholderHandle();
        // THE COMPARISON SAMPLER IS MANDATORY HERE, not a refinement. This
        // PUBLISHES into the shared offset table, so the descriptor staged here is
        // what every bindless reader of TEX_SHADOW sees for the rest of the frame
        // — including shaders that never go near DDGI. Defaulting the sampler
        // stages a compare-DISABLED handle, and a `sampler2DArrayShadow` built
        // from one is undefined and reads as unshadowed. See
        // HeapBinding::ShadowDepthSampler.
        const RHI::SamplerDesc shadowSampler = HeapBinding::ShadowDepthSampler(true);
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_SHADOW, csmID,
                                                 RHI::HeapSlotLifetime::Persistent, shadowSampler,
                                                 RHI::NullSamplerKind::Texture2DArrayShadow);
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_SHADOW_ATLAS, atlasID,
                                                 RHI::HeapSlotLifetime::Persistent, shadowSampler,
                                                 RHI::NullSamplerKind::Texture2DArrayShadow);
        // Comparison OFF but everything else as the texture object carries it — see
        // the note in DeferredLightingPass. `{}` here would publish a ClampToEdge,
        // mip-filtered descriptor to a slot every bindless reader shares.
        const RHI::SamplerDesc rawShadowSampler = HeapBinding::ShadowDepthSampler(false);
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_SHADOW_CSM_RAW, csmRawID,
                                                 RHI::HeapSlotLifetime::Persistent, rawShadowSampler,
                                                 RHI::NullSamplerKind::Texture2DArray);
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, atlasRawID,
                                                 RHI::HeapSlotLifetime::Persistent, rawShadowSampler,
                                                 RHI::NullSamplerKind::Texture2DArray);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();

        // ONE draw over the whole radiance atlas. Which probes actually pay for
        // the light loop is decided IN THE SHADER by ddgiProbeUpdatesNow —
        // sparsity plus the variable update rate (issue #707), replacing the
        // #632 RelightBudget row scissor. The scissor throttled by ATLAS ROW,
        // which is a storage-order window with no relationship to what the
        // camera can see; two mechanisms deciding the same thing, one of them
        // view-blind, is strictly worse than one that knows.
        HeapBinding::FlushOffsets();
        RenderCommand::DrawIndexed(va);
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
        BindProbeBuffers();
        HeapBinding::BindTextureOrOffset(0, m_RadianceFB->GetColorAttachmentHandle(0), RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindTextureOrOffset(1, m_HitFB->GetColorAttachmentHandle(1), RHI::HeapSlotLifetime::Persistent); // hit geo (backface flags)
        HeapBinding::BindTextureOrOffset(2, m_IrradianceFB[prevIdx]->GetColorAttachmentHandle(0), RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindTextureOrOffset(3, m_ProbeDataTexture, RHI::HeapSlotLifetime::Persistent);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        HeapBinding::FlushOffsets();
        RenderCommand::DrawIndexed(va);

        // Swap AFTER the draw.
        m_IrradianceCurrent = currIdx;
    }

    void DDGIProbeUpdatePass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

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
        // reverted the atlas publication at slots 56/64/58 (an INTENDED global
        // side effect, same shape as SetGlobalIBL) to the previous frame's
        // textures, and its per-frame "N mutations escaped" trace flooded the
        // log at ~90 lines/s. See the restore block before the publish step.
        const auto prevViewport = RenderCommand::GetViewport();

        // 1. Resource fingerprint / (re)create.
        EnsureResources();

        // Frame-derived values. The camera comes from CommandDispatch rather
        // than the submission: it is the same value every other pass uses, it
        // is correct on the editor, simulate and runtime paths alike, and
        // threading it through DDGIVolumeDesc would have added a second place
        // for the cascade centre to be wrong.
        m_RenderOrigin = Renderer3D::GetRenderOrigin();
        m_CameraWorldPos = CommandDispatch::GetViewPosition();
        m_WorldViewProjection = CommandDispatch::GetViewProjectionMatrix();
        m_ViewFrustum.Update(m_WorldViewProjection);

        BuildCascades();

        m_Spacing = m_Cascades[0].Spacing;
        m_MinAxialSpacing = glm::min(glm::min(m_Spacing.x, m_Spacing.y), m_Spacing.z);
        m_MaxRayDistance = DDGI::kMaxRayDistanceSpacingScale * glm::length(m_Spacing);
        m_TileDims = DDGI::CascadedAtlasTileDimensions(m_Desc.Resolution, m_CascadeCount);
        m_TotalProbes = static_cast<i32>(m_Records.size());
        // Frame counters START AT 1: 0 is the "never requested" sentinel in the
        // probe aux buffer, and a frame 0 would make every probe read as
        // requested on the very first frame and as stale forever after.
        ++m_FrameIndex;

        m_SceneDepthID = m_SelectedSceneDepth.IsValid() ? context.ResolveTextureHandle(m_SelectedSceneDepth)
                                                        : RHI::NullResource;

        // Upload the DDGI UBO up front (Enabled = 1): every stage below reads
        // the volume block via include/DDGICommon.glsl.
        UploadVolumeUBO(true);

        // Stats are per frame; the aux buffer's request timestamps are NOT
        // (they are the sparsity history and must survive).
        if (m_StatsSSBO)
        {
            m_StatsSSBO->ClearData();
        }

        // 2. Per-probe maintenance: cascade-shift invalidation, camera seed,
        //    live/active counters.
        DispatchProbeMaintain();

        // 3. Requests. Screen pixels first, then the ONE probe->probe
        //    indirection over the probes those pixels just made live.
        DispatchScreenRequests();
        DispatchProbeRequests();

        // 4-5. Amortized capture: mini-G-buffer rasterization -> octahedral
        // resample, per scheduled probe, then GPU relocation/classification.
        const std::vector<i32> captureSet = PickCaptureSet(m_Desc.CaptureBudget);
        if (!captureSet.empty())
        {
            for (const i32 probeIdx : captureSet)
            {
                CaptureProbe(probeIdx);
                ResampleProbe(probeIdx);
            }

            // The resample wrote the hit atlas through the raster pipeline; the
            // relocation compute samples it. Framebuffer writes are coherent
            // for later texture fetches, but the probe-data IMAGE the compute
            // then writes is not, so the barrier after the dispatches below is
            // the one that matters.
            for (const i32 probeIdx : captureSet)
            {
                ProbeRecord& rec = m_Records[static_cast<sizet>(probeIdx)];
                // A probe that has exhausted its placement refinements is
                // SETTLED; this capture is a geometry refresh, so the spring
                // must leave its position alone.
                const bool refreshCapture = rec.Captured && rec.RelocationIteration >= kMaxRelocationIterations;
                RelocateProbeGPU(probeIdx, refreshCapture);
                rec.Captured = true;
                rec.LastCaptureFrame = m_FrameIndex;
                // The spring needs a few iterations, and the CPU cannot see
                // whether it converged (that would be a readback), so it
                // schedules a bounded number of follow-up captures and stops.
                if (rec.RelocationIteration < kMaxRelocationIterations)
                {
                    ++rec.RelocationIteration;
                    rec.PendingRelocationRecapture = true;
                }
                else
                {
                    rec.PendingRelocationRecapture = false;
                }
            }
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch |
                                         MemoryBarrierFlags::ShaderStorage);

            // The capture overwrote UBO binding 0 with this pass's own camera
            // buffer. Invalidate the dispatch bind-cache FIRST so the re-upload
            // actually rebinds the engine camera UBO (BindUBOIfNeeded would
            // otherwise think it never left), then restore the real camera for
            // the relight stage's CSM cascade selection — it is re-established
            // AGAIN at the end of Execute, after the last Shader::Bind (whose
            // resource-registry side effects can re-bind UBO point 0).
            CommandDispatch::InvalidateRenderStateCache();
            CommandDispatch::UploadCameraUBO();

            // 6. Visibility blend — only captured probes carry new distances
            // (ADR: visibility updates at capture time, not per frame).
            BlendVisibility(captureSet);
        }

        // Once-in-a-while scheduling diagnostics (issue #632 bring-up), now
        // reporting the CPU-side view only: the probe classification histogram
        // lives on the GPU and printing it every 300 frames would mean a
        // readback every 300 frames, which is exactly the habit #707 removes.
        // Ask GetProbeStats() when you want the GPU-side numbers.
        if ((m_FrameIndex % 300u) == 1u)
        {
            OLO_CORE_INFO("DDGI: frame {} cascades={} probes={} casters={} captured {}/frame, capture coverage {:.1f}%",
                          m_FrameIndex, m_CascadeCount, m_TotalProbes, m_Casters.size(), captureSet.size(),
                          static_cast<f64>(ComputeCapturedFraction()) * 100.0);
        }

        // 7. Relight every LIVE, SCHEDULED probe's cached hit points with
        // current direct lighting + previous-frame probe irradiance.
        RelightProbes();

        // 8. Cosine-convolve the relit radiance into the irradiance atlas
        // under adjusted-hysteresis EMA, ping-pong swap after.
        BlendIrradiance();

        // 9. Deliberate state restoration (replaces the GLStateGuard wrapper):
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
            HeapBinding::BindTextureOrOffset(unit, RHI::NullResource, RHI::HeapSlotLifetime::Persistent);
        }
        RenderCommand::SetViewport(prevViewport.x, prevViewport.y, prevViewport.width, prevViewport.height);
        CommandDispatch::InvalidateRenderStateCache();
        CommandDispatch::UploadCameraUBO();

        // 10. Publish: current (post-blend) atlases at the engine slots (an
        // intended cross-pass side effect, same shape as SetGlobalIBL), DDGI
        // UBO re-bound (uploaded Enabled=1 above). MUST come after the
        // restore block so nothing reverts the publication.
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_DDGI_IRRADIANCE, GetIrradianceAtlasID(), RHI::HeapSlotLifetime::Persistent);
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_DDGI_VISIBILITY, GetVisibilityAtlasID(), RHI::HeapSlotLifetime::Persistent);
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_DDGI_PROBE_DATA, m_ProbeDataTexture, RHI::HeapSlotLifetime::Persistent);
        m_DDGIUBO->Bind();

        // Remember this frame's world view-projection for the NEXT frame's
        // screen-request reconstruction: it must be the matrix the depth buffer
        // that pass reads was rendered with, not this frame's.
        // RECOMPUTED FROM THE ADJUSTED FORWARD MATRIX, never adjusted after
        // inverting — RHIProjectionSeam.h states that contract for every
        // uploaded inverse, and DDGI_RequestScreen.comp reconstructs world
        // positions from this one. Identity on GL, so this is a Vulkan-route
        // correctness fix rather than a behaviour change today.
        m_PrevWorldInvViewProjection = RHI::AdjustedInverseForShaderReconstruction(m_WorldViewProjection);
        m_HavePrevViewProjection = true;

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
        // spec bookkeeping follows the viewport — which the #707 screen-request
        // compute reads to size its dispatch.
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }
} // namespace OloEngine
