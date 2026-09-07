#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/RayTracedReflectionPass.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace OloEngine
{
    RayTracedReflectionPass::RayTracedReflectionPass()
    {
        SetName("RayTracedReflectionPass");
        OLO_CORE_INFO("Creating RayTracedReflectionPass.");
    }

    void RayTracedReflectionPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};
        m_SelectedPrefilterTexture = {};

        // The tier composites OVER the colour it is handed, so the input is
        // whatever the chain has produced so far — the same versioned-name
        // fallback SSR uses, minus SSR itself, because this pass runs BEFORE it.
        // PostProcessColor is deliberately not a candidate: its alias is
        // repointed downstream and reading it here would form a cycle.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSGIColor, ResourceNames::SSGIColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        // RTReflectionColor is only declared on the deferred path with a live
        // G-Buffer, so its absence is the forward path and downstream aliases
        // straight back to the upstream colour.
        if (!m_Enabled || !blackboard.Post.RTReflectionColor.IsValid() ||
            !blackboard.Scene.SceneDepth.IsValid() ||
            !blackboard.GBuffer.GBufferNormal.IsValid() ||
            !blackboard.GBuffer.GBufferAlbedo.IsValid())
            return;

        // The by-name execution dependency RayTracingScenePass::Setup reserved
        // for ray-query consumers. The acceleration structure is not a graph
        // resource — there is no handle to Read — so this edge is the only thing
        // stopping a reorder from putting the AS build after the pass that
        // traces against it. The symptom would be a frame with no ray-traced
        // reflections and nothing in the log.
        builder.DependsOnPass("RayTracingScenePass");

        [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto normalRead = builder.Read(blackboard.GBuffer.GBufferNormal, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto albedoRead = builder.Read(blackboard.GBuffer.GBufferAlbedo, RGReadUsage::ShaderSample);
        m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
        m_SelectedGBufferNormalTexture = blackboard.GBuffer.GBufferNormal;
        m_SelectedGBufferAlbedoTexture = blackboard.GBuffer.GBufferAlbedo;

        // The environment the hit's ambient is read from. Optional: without it
        // a hit is lit by the sun alone, which is dark but not wrong, and the
        // shader is told which case it is rather than sampling a dangling cube.
        if (blackboard.IBL.PrefilterMap.IsValid())
        {
            m_SelectedPrefilterTexture = blackboard.IBL.PrefilterMap;
            [[maybe_unused]] const auto prefilterRead =
                builder.Read(m_SelectedPrefilterTexture, RGReadUsage::ShaderSample);
        }

        constexpr std::string_view versionTag = "RayTracedReflectionPass";
        const auto outputHandle =
            builder.WriteNewVersion(blackboard.Post.RTReflectionColor, RGWriteUsage::RenderTarget, versionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(builder.CreateFramebufferAttachmentView(
            std::string(ResourceNames::RTReflectionColorTexture) + "@" + std::string(versionTag), outputHandle, 0u));
    }

    void RayTracedReflectionPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Created ONLY where GL_EXT_ray_query exists. Loading it anywhere else
        // is not a graceful degradation, it is a compile error in the log on
        // every OpenGL run — and the hierarchy this tier drops out of does not
        // need it. The null shader is what IsReadyForExecution reports, which
        // ResolveAvailabilityForFrame turns into a counted ShaderUnavailable
        // rather than a warning nobody reads.
        if (!RenderCommand::SupportsRayTracing())
        {
            OLO_CORE_INFO("RayTracedReflectionPass: hardware ray tracing unavailable — the ray-query tier stays "
                          "inert and the hierarchy falls back to SSR + probe/IBL.");
            // Seed the stats with the REAL reason now. Without a shader the pass
            // is never armed, so its target is never declared, so the node is
            // culled and Execute — the only place that fills these — never runs.
            // The panel would then read the default NotRequested and tell a user
            // on a non-RT machine that they had switched the tier off. That is
            // the same lie ResolveAvailabilityForFrame was fixed to stop telling,
            // arriving by the one path that bypasses it.
            m_Stats.Fallback = ReflectionTierFallbackReason::ShaderUnavailable;
            m_Stats.RayQueryTierActive = false;
            return;
        }

        m_ReflectionShader = Shader::Create("assets/shaders/RayTracedReflection.glsl");

        OLO_CORE_INFO("RayTracedReflectionPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    bool RayTracedReflectionPass::ResolveAvailabilityForFrame(bool graphResourcesResolved)
    {
        m_Stats.Reset();

        // Ordered most-fundamental first, so the reported reason names the
        // ROOT cause rather than the first symptom: "no RT device" must not be
        // reported as "no TLAS", which is what it also implies.
        //
        // m_Settings.Enabled is the USER'S INTENT and is tested first and alone.
        // m_Enabled is not a synonym for it: the pipeline folds the deferred-path
        // check and IsReadyForExecution() into it, so testing m_Enabled here
        // would report a non-RT device — where the shader was never created — as
        // "switched off in the render settings", and ShaderUnavailable and
        // RayTracingUnavailable would be unreachable. That is precisely the
        // countable fallback this issue asks for, reported as a lie.
        ReflectionTierFallbackReason reason = ReflectionTierFallbackReason::None;
        if (!m_Settings.Enabled)
            reason = ReflectionTierFallbackReason::NotRequested;
        else if (!m_ReflectionShader || !m_ReflectionShader->IsReady() || !m_ParamsUBO)
            reason = ReflectionTierFallbackReason::ShaderUnavailable;
        else if (m_RayTracingScene == nullptr || !m_RayTracingScene->IsAvailable())
            reason = ReflectionTierFallbackReason::RayTracingUnavailable;
        // A TLAS device address of zero means no TLAS has ever been built,
        // which is a DIFFERENT state from "no RT device". Conflating them is how
        // "the first frame has no reflections" gets misread as "this GPU cannot
        // ray trace" — and it is also the shape of the trap this issue warns
        // about, where a near-empty TLAS looks exactly like a tier correctly
        // falling through to the probes.
        else if (m_RayTracingScene->GetTlasDeviceAddress() == 0u)
            reason = ReflectionTierFallbackReason::AccelerationStructureEmpty;
        else if (m_GPUScene == nullptr || m_GPUScene->GetInstanceSlotCount() == 0u)
            reason = ReflectionTierFallbackReason::GPUSceneUnavailable;
        else if (!m_Enabled || !graphResourcesResolved)
            // The tier is wanted and able, but the pipeline did not arm it (the
            // forward path) or the graph declared no target this frame.
            reason = ReflectionTierFallbackReason::TargetUnavailable;

        m_Stats.Fallback = reason;
        m_Stats.RayQueryTierActive = (reason == ReflectionTierFallbackReason::None);

        if (reason != m_LastReportedFallback)
        {
            m_LastReportedFallback = reason;
            // Once per CHANGE of reason, not once per frame: this is the line
            // that tells a user why their reflections are not ray traced, and a
            // per-frame version of it would be scrolled away unread.
            if (reason == ReflectionTierFallbackReason::None)
                OLO_CORE_INFO("RayTracedReflectionPass: the ray-query reflection tier is active.");
            else if (reason == ReflectionTierFallbackReason::GPUSceneUnavailable)
            {
                // The counts, not just the verdict. "The GPU Scene tables are
                // unavailable" is true of several very different states — no
                // GPU Scene at all, a scene that staged nothing, a scene whose
                // renderables are all ModelComponents (#1065) — and only the
                // numbers separate them.
                OLO_CORE_WARN("RayTracedReflectionPass: the ray-query reflection tier stood down — {} "
                              "(gpuScene={}, instanceSlots={}, geometrySlots={}, materialSlots={})",
                              ToString(reason), m_GPUScene != nullptr,
                              m_GPUScene != nullptr ? m_GPUScene->GetInstanceSlotCount() : 0u,
                              m_GPUScene != nullptr ? m_GPUScene->GetGeometrySlotCount() : 0u,
                              m_GPUScene != nullptr ? m_GPUScene->GetMaterialSlotCount() : 0u);
            }
            else if (reason != ReflectionTierFallbackReason::NotRequested)
                OLO_CORE_WARN("RayTracedReflectionPass: the ray-query reflection tier stood down — {}",
                              ToString(reason));
        }

        if (m_Stats.RayQueryTierActive)
        {
            // Both are STANDING limitations of this slice rather than occasional
            // ones, so they are true whenever the tier ran at all. Counted
            // instead of commented because neither is visible in a still frame.
            m_Stats.HitsShadedUntextured = true;          // #805 — untextured material factors
            m_Stats.MaskedGeometryReflectsAsSolid = true; // no any-hit alpha test without the heap
        }

        return m_Stats.RayQueryTierActive;
    }

    void RayTracedReflectionPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: the input framebuffer is intentionally not
        // resolved as an FBO here - see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
            outputFramebuffer = context.ResolveFramebuffer(outputHandle);

        RHI::ResourceHandle sceneDepthID{};
        RHI::ResourceHandle normalID{};
        RHI::ResourceHandle albedoID{};
        RHI::ResourceHandle prefilterID{};
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedGBufferNormalTexture.IsValid())
            normalID = context.ResolveTextureHandle(m_SelectedGBufferNormalTexture);
        if (m_SelectedGBufferAlbedoTexture.IsValid())
            albedoID = context.ResolveTextureHandle(m_SelectedGBufferAlbedoTexture);
        if (m_SelectedPrefilterTexture.IsValid())
            prefilterID = context.ResolveTextureHandle(m_SelectedPrefilterTexture);

        const bool graphResolved = outputFramebuffer && inputColorID.IsValid() && sceneDepthID.IsValid() &&
                                   normalID.IsValid() && albedoID.IsValid();

        // The stats / warning verdict for this frame. It does NOT gate the draw:
        // when the tier cannot answer, running the draw is still correct,
        // because every guard in the shader (a zero TLAS address, a zero slot
        // count, sky depth, the roughness gate) returns the input colour
        // unchanged. That is what "the fallback is structural, not a flag"
        // buys: there is no branch here that could forget to fill the target,
        // and no second copy path to keep in step with the shader.
        const bool tierActive = ResolveAvailabilityForFrame(graphResolved);
        if (!graphResolved)
            return; // nothing was declared to write into; downstream aliases back

        const auto& outSpec = outputFramebuffer->GetSpecification();
        const f32 width = static_cast<f32>(outSpec.Width);
        const f32 height = static_cast<f32>(outSpec.Height);
        if (width <= 0.0f || height <= 0.0f)
            return;

        UBOStructures::RayTracingReflectionUBO params{};

        // RENDER-RELATIVE, not world. The TLAS is built from the GPU Scene's
        // render-relative instance transforms, so a world-space ray origin
        // would miss every instance by the origin offset — an error that grows
        // with distance from the origin and therefore looks like "reflections
        // drift when the camera moves far from the scene centre".
        const glm::mat4 relativeView = MakeViewRelative(m_View, m_RenderOrigin);
        params.View = relativeView;
        params.InvView = glm::inverse(relativeView);
        // THE SHADER-RECONSTRUCTION SEAM, not a plain inverse. This shader does
        // the `ndc = vec3(uv*2-1, depth*2-1)` reconstruction, the family
        // RHIProjectionSeam.h says must carry the Vulkan row flip — sampled uv
        // v=0 is the TOP row there. A plain glm::inverse renders correctly on GL
        // and reconstructs every ray origin vertically mirrored on Vulkan.
        // RayTracedShadowPass and ContactShadowRenderPass use the same helper;
        // the three must not disagree.
        params.InvProjection = RHI::AdjustedInverseForShaderReconstruction(m_Projection);

        // A ZERO TLAS ADDRESS IS THE OFF SWITCH, uploaded deliberately rather
        // than skipped: it is the value the shader's first guard tests, so an
        // inactive tier reaches the GPU as "trace nothing" instead of as a
        // stale address left over from the last frame that had one.
        const u64 tlasAddress = tierActive ? m_RayTracingScene->GetTlasDeviceAddress() : 0u;
        params.TlasAddress = glm::uvec4(static_cast<u32>(tlasAddress & 0xFFFFFFFFull),
                                        static_cast<u32>(tlasAddress >> 32u),
                                        RayTracing::kInstanceMaskAll,
                                        m_FrameIndex);
        params.SlotCounts = tierActive ? glm::uvec4(m_GPUScene->GetInstanceSlotCount(),
                                                    m_GPUScene->GetGeometrySlotCount(),
                                                    m_GPUScene->GetMaterialSlotCount(),
                                                    0u)
                                       : glm::uvec4(0u);

        // A directional light's direction is translation-invariant, so unlike a
        // punctual light's position it needs NO render-origin shift.
        const glm::vec3 sunDirection = (m_HasSun && glm::dot(m_SunDirection, m_SunDirection) > 1e-8f)
                                           ? glm::normalize(m_SunDirection)
                                           : glm::vec3(0.0f, 1.0f, 0.0f);
        params.SunDirection = glm::vec4(sunDirection, m_HasSun ? 1.0f : 0.0f);
        params.SunColor = glm::vec4(m_SunRadiance, 0.0f);

        // Sanitized here, not only where the settings are loaded: these values
        // also arrive from a live edit, a script or an MCP write, and the shader
        // cannot be the backstop — a NaN gate would make smoothstep undefined
        // and a NaN confidence would spread through bloom as a black block.
        const auto finiteOr = [](f32 value, f32 fallback)
        { return std::isfinite(value) ? value : fallback; };
        const f32 maxRayDistance = std::max(finiteOr(m_Settings.MaxRayDistance, 60.0f), 0.0f);
        const f32 normalBias = std::max(finiteOr(m_Settings.RayOriginNormalBias, 0.02f), 0.0f);
        const f32 intensity = std::clamp(finiteOr(m_Settings.Intensity, 1.0f), 0.0f, 4.0f);
        params.RayParams =
            glm::vec4(maxRayDistance, normalBias, intensity, m_Settings.TraceSunShadowRay ? 1.0f : 0.0f);

        // UPPER BOUND BELOW 1.0, deliberately. gateEnd below clamps with a lower
        // bound of gateStart + 1e-4, and std::clamp has UNDEFINED BEHAVIOUR when
        // lo > hi — so a gateStart of exactly 1.0 would make that call UB rather
        // than merely degenerate. 1.0 is reachable: the MCP field advertises the
        // range [0,1] and a scene file can carry it. Capping the START one epsilon
        // below 1.0 keeps the interval well-formed for every input.
        const f32 gateStart = std::clamp(finiteOr(m_Settings.RoughnessGateStart, 0.05f), 0.0f, 1.0f - 1e-4f);
        // Strictly above the start: the shader divides the band, and a zero-wide
        // one would make smoothstep a step with an undefined edge case at equal
        // endpoints.
        const f32 gateEnd = std::clamp(finiteOr(m_Settings.RoughnessGateEnd, 0.30f), gateStart + 1e-4f, 1.0f);
        // w says whether an environment cube is BOUND this frame. The shader
        // must not sample a unit the pass skipped — an unbound sampler is
        // undefined behaviour, not a zero read — and the blackboard's
        // PrefilterMap is genuinely optional.
        const f32 skyLod = std::max(finiteOr(m_Settings.SkyAmbientLod, 4.0f), 0.0f);
        params.RoughnessGate = glm::vec4(gateStart, gateEnd, skyLod, prefilterID.IsValid() ? 1.0f : 0.0f);

        params.ScreenParams = glm::vec4(width, height, 1.0f / width, 1.0f / height);
        params.Flags = glm::vec4(m_Settings.TierDebugView ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

        // The issue's per-tier ray telemetry. Derived, not measured — the same
        // honest form the shadow tier uses. It is an UPPER bound because every
        // pixel above the roughness gate or at sky depth dispatches no ray, and
        // the shader cannot report back how many did. The optional sun ray
        // doubles it. u64 because 4K x 2 is already past what a u32 should hold
        // after one more multiplier.
        const u64 pixels = static_cast<u64>(outSpec.Width) * static_cast<u64>(outSpec.Height);
        m_Stats.ReflectionRaysDispatchedUpperBound =
            tierActive ? pixels * (m_Settings.TraceSunShadowRay ? 2ull : 1ull) : 0ull;

        // Rebind binding 65 before writing: other passes displace this indexed
        // binding, and RayTracedShadow.glsl / RayTracingProbe.comp declare their
        // own blocks at the same number (see the UBO's comment for why sharing
        // it is safe).
        m_ParamsUBO->Bind();
        m_ParamsUBO->SetData(&params, UBOStructures::RayTracingReflectionUBO::GetSize());

        // The instance / geometry / material tables at their canonical SSBO
        // bindings (15 / 16 / 17). Bound HERE rather than relied on from an
        // earlier pass: an indexed buffer binding is global state that any draw
        // between then and now may have displaced.
        if (m_GPUScene != nullptr)
            m_GPUScene->Bind();

        auto& gpuTimers = GPUPassTimerPool::GetInstance();
        gpuTimers.BeginSubPass("RayTracedReflectionTrace");

        outputFramebuffer->Bind();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        {
            constexpr u32 colorAttachment = 0;
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
            RenderCommand::SetColorMask(true, true, true, true);
            RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }

        m_ReflectionShader->Bind();
        context.BindTextureOrHeapOffset(0, inputColorID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_ALBEDO, albedoID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL, normalID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        // UNCONDITIONALLY, with a null handle when there is no environment —
        // exactly what DeferredLightingPass does at this same unit. Skipping the
        // bind does not leave the unit empty, it leaves whatever the previous
        // draw put there, and TEX_USER_1 is a general-purpose slot that other
        // passes fill with a sampler2D. A samplerCube declaration reading a
        // sampler2D binding is a type mismatch for the whole draw, not just for
        // the branch that samples it, so the guard has to be in the SHADER (the
        // hasEnvironment lane) and the bind has to always happen.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_USER_1,
                                        prefilterID.IsValid() ? prefilterID : RHI::NullResource,
                                        RHI::HeapSlotLifetime::FrameTransient);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.FlushHeapOffsets();
            RenderCommand::DrawIndexed(va);
        }

        // Restore the depth mask the fullscreen state turned off. Every
        // neighbouring post pass does this; without it, a frame where this tier
        // runs and SSR does not leaves depth writes disabled for the rest of the
        // chain.
        RenderCommand::SetDepthMask(true);
        outputFramebuffer->Unbind();
        gpuTimers.EndSubPass();
    }

    void RayTracedReflectionPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void RayTracedReflectionPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void RayTracedReflectionPass::OnReset()
    {
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
        m_SelectedGBufferAlbedoTexture = {};
        m_SelectedPrefilterTexture = {};
        m_Stats.Reset();
        m_LastReportedFallback = ReflectionTierFallbackReason::Count;
    }

} // namespace OloEngine
