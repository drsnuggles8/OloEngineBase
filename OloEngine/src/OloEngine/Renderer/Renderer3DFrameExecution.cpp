#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace OloEngine
{
    namespace
    {
        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            return value && value[0] != '\0' && value[0] != '0' && value[0] != 'f' && value[0] != 'F';
        }

        bool IsRenderGraphDiagnosticsEnabled()
        {
            static const bool enabled = IsTruthyEnvironmentVariable("OLO_RENDERGRAPH_DIAGNOSTICS");
            return enabled;
        }
    } // namespace

    void Renderer3D::ConfigureFrameGraphPassesForFrame()
    {
        // OITResolvePass and SSSPass are self-resolving:
        // their Execute(RGCommandContext&) calls context.GetBlackboard() to look
        // up SceneColor directly, eliminating the per-frame side-channel. The
        // legacy raw SetInputFramebuffer() setters remain as headless / test
        // fallbacks.
        // (A previous typed-handle side-channel block was removed;
        // removed since those two passes resolve via the blackboard.)

        // ForwardOverlayPass, FoliagePass, WaterPass,
        // DecalPass and ParticlePass now self-resolve SceneColor (and
        // SceneDepth for Decal) directly from the render-graph blackboard
        // inside their Execute() implementations. No per-frame side-channel
        // setter calls are needed here.
        {
            // DeferredLightingPass now self-resolves all
            // G-Buffer and scene depth handles from the render-graph
            // blackboard. No SetXxx calls needed.
        }

        if (s_Data.SceneCompositePasses.SSAO)
        {
            s_Data.SceneCompositePasses.SSAO->SetSettings(s_Data.PostProcess);
            // SSAOPass now self-resolves SceneDepth and
            // SceneNormals from the blackboard; no per-frame handle setters needed.

            // Upload projection matrices for SSAO position reconstruction
            s_Data.PostProcessGPU.SSAOData.Projection = s_Data.ProjectionMatrix;
            s_Data.PostProcessGPU.SSAOData.InverseProjection = glm::inverse(s_Data.ProjectionMatrix);
            s_Data.PostProcessGPU.SSAOData.DebugView = s_Data.PostProcess.SSAODebugView ? 1 : 0;
        }
        if (s_Data.SceneCompositePasses.GTAO)
        {
            s_Data.SceneCompositePasses.GTAO->SetSettings(s_Data.PostProcess);
            s_Data.SceneCompositePasses.GTAO->SetProjectionMatrix(s_Data.ProjectionMatrix);
            s_Data.SceneCompositePasses.GTAO->SetViewMatrix(s_Data.ViewMatrix);
            // GTAOPass now self-resolves SceneDepth and
            // SceneNormals from the blackboard; no per-frame handle setters needed.

            // When GTAO is active, override the SSAO UBO debug/intensity fields
            // so the PostProcess_SSAOApply shader reads correct values for GTAO,
            // and upload the UBO since SSAORenderPass::Execute() is skipped.
            if (s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled)
            {
                s_Data.PostProcessGPU.SSAOData.DebugView = s_Data.PostProcess.GTAODebugView ? 1 : 0;
                // GTAO power is baked in compute; intensity=1 for the apply pass
                s_Data.PostProcessGPU.SSAOData.Intensity = 1.0f;
                s_Data.PostProcessGPU.SSAO->SetData(&s_Data.PostProcessGPU.SSAOData, SSAOUBOData::GetSize());
                s_Data.PostProcessGPU.SSAO->Bind();
            }
        }
        if (s_Data.PostProcessPasses.SSS)
        {
            s_Data.PostProcessPasses.SSS->SetSettings(s_Data.Snow);
        }
        // Wire AOApplyPass before the dynamic post chain.
        if (s_Data.PostProcessPasses.AOApply)
        {
            const bool ssaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO && s_Data.PostProcess.SSAOEnabled;
            const bool gtaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled;
            s_Data.PostProcessPasses.AOApply->SetEnabled(ssaoEnabled || gtaoEnabled);
            // SetAOTextureID was removed; AOApplyPass::Execute()
            // self-resolves AO texture via the render-graph blackboard.
            s_Data.PostProcessPasses.AOApply->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
            s_Data.PostProcessPasses.AOApply->SetSSAOUBO(s_Data.PostProcessGPU.SSAO);
        }
        // Standalone dynamic post-chain configuration.
        if (s_Data.PostProcessPasses.Bloom)
        {
            s_Data.PostProcessPasses.Bloom->SetEnabled(s_Data.PostProcess.BloomEnabled);
            s_Data.PostProcessPasses.Bloom->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
            s_Data.PostProcessPasses.Bloom->SetPostProcessGPUData(&s_Data.PostProcessGPU.PostProcessData);
        }

        if (s_Data.PostProcessPasses.DOF)
        {
            s_Data.PostProcessPasses.DOF->SetEnabled(s_Data.PostProcess.DOFEnabled);
            s_Data.PostProcessPasses.DOF->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
        }

        if (s_Data.PostProcessPasses.MotionBlur)
        {
            s_Data.PostProcessPasses.MotionBlur->SetEnabled(s_Data.PostProcess.MotionBlurEnabled);
            s_Data.PostProcessPasses.MotionBlur->SetMotionBlurUBO(s_Data.PostProcessGPU.MotionBlur);
        }

        if (s_Data.PostProcessPasses.TAA)
        {
            s_Data.PostProcessPasses.TAA->SetEnabled(s_Data.PostProcess.TAAEnabled);
            s_Data.PostProcessPasses.TAA->SetSettings(s_Data.PostProcess);
        }

        if (s_Data.PostProcessPasses.Precipitation)
        {
            const bool precipEnabled = s_Data.Precipitation.Enabled &&
                                       (s_Data.Precipitation.ScreenStreaksEnabled ||
                                        s_Data.Precipitation.LensImpactsEnabled);
            s_Data.PostProcessPasses.Precipitation->SetEnabled(precipEnabled);
        }

        if (s_Data.PostProcessPasses.Fog)
        {
            s_Data.PostProcessPasses.Fog->SetEnabled(s_Data.Fog.Enabled);
            s_Data.PostProcessPasses.Fog->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
        }

        if (s_Data.PostProcessPasses.ChromAberration)
        {
            s_Data.PostProcessPasses.ChromAberration->SetEnabled(s_Data.PostProcess.ChromaticAberrationEnabled);
            s_Data.PostProcessPasses.ChromAberration->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
        }
        if (s_Data.PostProcessPasses.ColorGrading)
        {
            s_Data.PostProcessPasses.ColorGrading->SetEnabled(s_Data.PostProcess.ColorGradingEnabled);
            s_Data.PostProcessPasses.ColorGrading->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
        }
        if (s_Data.PostProcessPasses.ToneMap)
        {
            // ToneMap always runs; m_Enabled stays true.
            s_Data.PostProcessPasses.ToneMap->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
        }
        if (s_Data.PostProcessPasses.Vignette)
        {
            s_Data.PostProcessPasses.Vignette->SetEnabled(s_Data.PostProcess.VignetteEnabled);
            s_Data.PostProcessPasses.Vignette->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
        }

        if (s_Data.PostProcessPasses.FXAA)
        {
            s_Data.PostProcessPasses.FXAA->SetEnabled(s_Data.PostProcess.FXAAEnabled);
            s_Data.PostProcessPasses.FXAA->SetPostProcessUBO(s_Data.PostProcessGPU.PostProcess);
        }
    }

    void Renderer3D::UploadFrameGraphExecutionState()
    {
        auto& profiler = RendererProfiler::GetInstance();
        if (s_Data.ScenePass)
        {
            const auto& commandBucket = s_Data.ScenePass->GetCommandBucket();
            profiler.IncrementCounter(RendererProfiler::MetricType::CommandPackets, static_cast<u32>(commandBucket.GetCommandCount()));
        }

        ApplyGlobalResources();

        // Upload post-process settings to GPU
        {
            auto& pp = s_Data.PostProcess;
            auto& gpu = s_Data.PostProcessGPU.PostProcessData;
            gpu.TonemapOperator = static_cast<i32>(pp.Tonemap);
            gpu.Exposure = pp.Exposure;
            gpu.Gamma = pp.Gamma;
            gpu.BloomThreshold = pp.BloomThreshold;
            gpu.BloomIntensity = pp.BloomIntensity;
            gpu.VignetteIntensity = pp.VignetteIntensity;
            gpu.VignetteSmoothness = pp.VignetteSmoothness;
            gpu.ChromaticAberrationIntensity = pp.ChromaticAberrationIntensity;
            gpu.DOFFocusDistance = pp.DOFFocusDistance;
            gpu.DOFFocusRange = pp.DOFFocusRange;
            gpu.DOFBokehRadius = pp.DOFBokehRadius;
            gpu.MotionBlurStrength = pp.MotionBlurStrength;
            gpu.MotionBlurSamples = pp.MotionBlurSamples;
            gpu.CameraNear = s_Data.CameraNearClip;
            gpu.CameraFar = s_Data.CameraFarClip;
            if (s_Data.ScenePass && s_Data.ScenePass->GetTarget())
            {
                const auto& spec = s_Data.ScenePass->GetTarget()->GetSpecification();
                gpu.InverseScreenWidth = 1.0f / static_cast<f32>(spec.Width);
                gpu.InverseScreenHeight = 1.0f / static_cast<f32>(spec.Height);
            }
            s_Data.PostProcessGPU.PostProcess->SetData(&gpu, PostProcessUBOData::GetSize());
        }

        // Upload DRS bounds so screen-space shaders can clamp UVs to the rendered region.
        {
            const glm::vec2 bounds = s_Data.RGraph ? s_Data.RGraph->GetRenderScaleBounds() : glm::vec2(1.0f);
            s_Data.SceneEffectsGPU.DRSData.RenderScaleBounds = bounds;
            s_Data.SceneEffectsGPU.DRS->SetData(&s_Data.SceneEffectsGPU.DRSData, DRSUBOData::GetSize());
        }

        // Upload snow settings to GPU
        if (s_Data.Snow.Enabled)
        {
            auto& snow = s_Data.Snow;
            auto& gpu = s_Data.SceneEffectsGPU.SnowData;
            gpu.CoverageParams = glm::vec4(snow.HeightStart, snow.HeightFull, snow.SlopeStart, snow.SlopeFull);
            gpu.AlbedoAndRoughness = glm::vec4(snow.Albedo, snow.Roughness);
            gpu.SSSColorAndIntensity = glm::vec4(snow.SSSColor, snow.SSSIntensity);
            gpu.SparkleParams = glm::vec4(snow.SparkleIntensity, snow.SparkleDensity, snow.SparkleScale, snow.NormalPerturbStrength);
            gpu.Flags = glm::vec4(1.0f, snow.WindDriftFactor, 0.0f, 0.0f);
            s_Data.SceneEffectsGPU.Snow->SetData(&gpu, SnowUBOData::GetSize());

            // SSS blur parameters
            auto& sssGpu = s_Data.SceneEffectsGPU.SSSData;
            sssGpu.BlurParams = glm::vec4(snow.SSSBlurRadius, snow.SSSBlurFalloff, 0.0f, 0.0f);
            sssGpu.Flags = glm::vec4(snow.SSSBlurEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            if (s_Data.ScenePass && s_Data.ScenePass->GetTarget())
            {
                const auto& spec = s_Data.ScenePass->GetTarget()->GetSpecification();
                sssGpu.BlurParams.z = static_cast<f32>(spec.Width);
                sssGpu.BlurParams.w = static_cast<f32>(spec.Height);
            }
            s_Data.SceneEffectsGPU.SSS->SetData(&sssGpu, SSSUBOData::GetSize());
        }
        else
        {
            // Upload disabled state so shaders skip snow
            auto& gpu = s_Data.SceneEffectsGPU.SnowData;
            gpu.Flags = glm::vec4(0.0f);
            s_Data.SceneEffectsGPU.Snow->SetData(&gpu, SnowUBOData::GetSize());
        }

        // Upload fog & atmospheric scattering settings to GPU
        if (s_Data.Fog.Enabled)
        {
            auto& fog = s_Data.Fog;
            auto& gpu = s_Data.SceneEffectsGPU.FogData;
            gpu.ColorAndDensity = glm::vec4(fog.Color, fog.Density);
            gpu.DistanceParams = glm::vec4(fog.Start, fog.End, fog.HeightFalloff, fog.HeightOffset);
            gpu.ScatterParams = glm::vec4(fog.RayleighStrength, fog.MieStrength, fog.MieDirectionality, fog.SunIntensity);
            gpu.RayleighColorAndMaxOpacity = glm::vec4(fog.RayleighColor, fog.MaxOpacity);
            // Derive sun direction from the scene's primary directional light
            // Guard against zero-length direction to prevent NaN from normalize
            glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
            const f32 dirLen2 = glm::dot(s_Data.SceneLight.Direction, s_Data.SceneLight.Direction);
            if (std::isfinite(dirLen2) && dirLen2 > 1e-8f)
            {
                sunDir = glm::normalize(s_Data.SceneLight.Direction);
            }
            // Pack fog frame index into SunDirection.w (bare uniforms fail SPIR-V)
            // Wrap at 1024 to stay well within float32 integer-exact range
            gpu.SunDirection = glm::vec4(sunDir, static_cast<f32>(s_Data.FogFrameIndex));
            s_Data.FogFrameIndex = (s_Data.FogFrameIndex + 1u) & 0x3FFu;
            gpu.Flags = glm::vec4(1.0f, static_cast<f32>(static_cast<i32>(fog.Mode)),
                                  fog.EnableScattering ? 1.0f : 0.0f,
                                  fog.EnableVolumetric ? 1.0f : 0.0f);

            // Accumulate time for noise animation
            const auto fogNow = std::chrono::steady_clock::now();
            const f32 fogDt = std::clamp(std::chrono::duration<f32>(fogNow - s_Data.FogLastTime).count(), 0.0f, 0.1f);
            s_Data.FogLastTime = fogNow;
            s_Data.FogTime += fogDt;

            const f32 effectiveNoiseIntensity = fog.EnableNoise ? fog.NoiseIntensity : 0.0f;
            gpu.NoiseParams = glm::vec4(fog.NoiseScale, fog.NoiseSpeed, effectiveNoiseIntensity, s_Data.FogTime);
            gpu.VolumetricParams = glm::vec4(static_cast<f32>(fog.VolumetricSamples), fog.AbsorptionCoefficient,
                                             fog.LightShaftIntensity, fog.EnableLightShafts ? 1.0f : 0.0f);
            s_Data.SceneEffectsGPU.Fog->SetData(&gpu, FogUBOData::GetSize());
        }
        else
        {
            auto& gpu = s_Data.SceneEffectsGPU.FogData;
            gpu.Flags = glm::vec4(0.0f);
            s_Data.SceneEffectsGPU.Fog->SetData(&gpu, FogUBOData::GetSize());
        }

        // Upload fog volumes (collected by the scene)
        s_Data.SceneEffectsGPU.FogVolumes->SetData(&s_Data.SceneEffectsGPU.FogVolumesData, FogVolumesUBOData::GetSize());

        // Update wind system (regenerate 3D wind field, upload wind UBO)
        {
            // TODO: Pass actual frame dt once Timestep is threaded through BeginScene
            static auto lastTime = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            f32 dt = std::chrono::duration<f32>(now - lastTime).count();
            dt = std::clamp(dt, 0.0f, 0.1f);
            lastTime = now;

            WindSystem::Update(s_Data.Wind, s_Data.ViewPos, Timestep(dt));
            WindSystem::BindWindTexture();

            // Update snow accumulation system
            if (s_Data.SnowAccumulation.Enabled)
            {
                SnowAccumulationSystem::Update(s_Data.SnowAccumulation, s_Data.ViewPos, Timestep(dt));
                SnowAccumulationSystem::BindSnowDepthTexture();
                CommandDispatch::SetSnowDepthTextureID(SnowAccumulationSystem::GetSnowDepthTextureID());
            }

            // Update snow ejecta particle simulation
            if (s_Data.SnowEjecta.Enabled)
            {
                SnowEjectaSystem::Update(s_Data.SnowEjecta, Timestep(dt));
            }

            // Update precipitation system (always run so disabled particles can drain)
            {
                glm::vec3 windXZ = glm::vec3(s_Data.Wind.Direction.x, 0.0f, s_Data.Wind.Direction.z);
                f32 windXZLen = glm::length(windXZ);
                glm::vec3 windDir = (windXZLen > 1e-6f) ? (windXZ / windXZLen) : glm::vec3(1.0f, 0.0f, 0.0f);
                f32 windSpeed = s_Data.Wind.Speed;
                PrecipitationSystem::Update(s_Data.Precipitation, s_Data.ViewPos, windDir, windSpeed, Timestep(dt));

                if (s_Data.Precipitation.Enabled)
                {
                    glm::vec2 windDirScreen = glm::vec2(windDir.x, -windDir.z);
                    ScreenSpacePrecipitation::Update(s_Data.Precipitation, PrecipitationSystem::GetCurrentIntensity(), windDirScreen, windSpeed, dt);
                    PrecipitationSystem::UpdateScreenEffectsUBO(s_Data.Precipitation, windDirScreen, s_Data.FogTime);
                }
            }
        }

        // Upload motion blur / inverse VP matrices (needed by motion blur AND fog depth reconstruction
        // AND the deferred lighting pass, which reconstructs world-space position from G-Buffer depth
        // AND TAA for camera-only velocity reprojection in Forward / Forward+).
        if (s_Data.PostProcess.MotionBlurEnabled || s_Data.PostProcess.TAAEnabled || s_Data.Fog.Enabled || s_Data.Settings.Path == RenderingPath::Deferred)
        {
            auto& mb = s_Data.PostProcessGPU.MotionBlurData;
            mb.InverseViewProjection = s_Data.InverseViewProjectionMatrix;
            mb.PrevViewProjection = s_Data.PrevViewProjectionMatrix;
            s_Data.PostProcessGPU.MotionBlur->SetData(&mb, MotionBlurUBOData::GetSize());
        }

        // Wire deferred lighting pass inputs each frame so it reflects the
        // current G-Buffer / debug-channel selection. The pass no-ops when
        // the path is Forward / Forward+ (GBuffer is never created).
        if (s_Data.SceneCompositePasses.DeferredLighting && s_Data.ScenePass)
        {
            const bool deferred = (s_Data.Settings.Path == RenderingPath::Deferred);
            s_Data.SceneCompositePasses.DeferredLighting->SetGBuffer(deferred ? s_Data.ScenePass->GetGBuffer() : nullptr);
            s_Data.SceneCompositePasses.DeferredLighting->SetDebugChannel(deferred ? s_Data.Settings.Deferred.DebugChannel : 0);
            s_Data.SceneCompositePasses.DeferredLighting->SetPerSampleLighting(deferred && s_Data.Settings.Deferred.PerSampleLighting);
        }

        // Wire the opaque-decal graph shim: in Deferred mode it drains the
        // DecalRenderPass bucket into the G-Buffer between ScenePass and
        // DeferredLightingPass. Safe to update unconditionally — the pass
        // no-ops when it isn't registered in the graph (Forward paths).
        if (s_Data.SceneCompositePasses.DeferredOpaqueDecal && s_Data.DecalPass && s_Data.ScenePass)
        {
            const bool deferred = (s_Data.Settings.Path == RenderingPath::Deferred);
            s_Data.SceneCompositePasses.DeferredOpaqueDecal->SetDecalPass(s_Data.DecalPass);
            s_Data.SceneCompositePasses.DeferredOpaqueDecal->SetGBuffer(deferred ? s_Data.ScenePass->GetGBuffer() : nullptr);
            s_Data.SceneCompositePasses.DeferredOpaqueDecal->SetPerSampleLighting(deferred && s_Data.Settings.Deferred.PerSampleLighting);
        }

        // Phase 6: propagate OIT toggle to transparent passes + resolve
        // every frame so UI changes take effect immediately.
        {
            const bool oitEnabled = (s_Data.Settings.Path == RenderingPath::Deferred) && s_Data.Settings.Deferred.OITEnabled;
            if (s_Data.SceneCompositePasses.Particle)
                s_Data.SceneCompositePasses.Particle->SetOITEnabled(oitEnabled);
            if (s_Data.WaterPass)
                s_Data.WaterPass->SetOITEnabled(oitEnabled);
            if (s_Data.DecalPass)
                s_Data.DecalPass->SetOITEnabled(oitEnabled);
            if (s_Data.SceneCompositePasses.OITResolve)
                s_Data.SceneCompositePasses.OITResolve->SetEnabled(oitEnabled);
        }
    }

    void Renderer3D::RefreshCompiledFrameGraphBlackboard()
    {
        auto& board = s_Data.RGraph->GetBlackboard();
        board.SSAORaw = s_Data.RGraph->GetFramebufferHandle("SSAORaw");
        // Phase D Slice 2: JFA ping-pong scratch framebuffers for SelectionOutlinePass.
        board.JFAPing = s_Data.RGraph->GetFramebufferHandle("JFAPing");
        board.JFAPong = s_Data.RGraph->GetFramebufferHandle("JFAPong");
        // Refresh imported core handles as well. Handle generations can advance
        // when the resource registry is rebuilt; using stale generations causes
        // ResolveFramebuffer/ResolveTexture to fail and leaves downstream passes
        // rendering into/reading from uninitialized outputs.
        board.SceneColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::SceneColor);
        board.SceneDepth = s_Data.RGraph->GetTextureHandle(ResourceNames::SceneDepth);
        board.SceneNormals = s_Data.RGraph->GetTextureHandle(ResourceNames::SceneNormals);
        board.Velocity = s_Data.RGraph->GetTextureHandle(ResourceNames::Velocity);
        board.AOBuffer = s_Data.RGraph->GetTextureHandle(ResourceNames::AOBuffer);
        // Refresh post-process transient framebuffer handles after BuildFrameGraph.
        // BuildFrameGraph finalizes per-frame resource generations; using handles
        // captured before build can resolve stale framebuffer slots (e.g. ping-pong
        // IDs), which manifests as black/transparent post chain outputs.
        board.SSSColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::SSSColor);
        board.AOApplyColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::AOApplyColor);
        board.BloomColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::BloomColor);
        board.DOFColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::DOFColor);
        board.MotionBlurColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::MotionBlurColor);
        board.TAAColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::TAAColor);
        board.PrecipitationColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::PrecipitationColor);
        board.FogColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::FogColor);
        board.ChromAbColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::ChromAbColor);
        board.ColorGradingColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::ColorGradingColor);
        board.ToneMapColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::ToneMapColor);
        board.VignetteColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::VignetteColor);
        board.FXAAColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::FXAAColor);
        board.SelectionOutlineColor = s_Data.RGraph->GetFramebufferHandle(ResourceNames::SelectionOutlineColor);
        board.UIComposite = s_Data.RGraph->GetFramebufferHandle(ResourceNames::UIComposite);

        // Rebuild dynamic post-chain alias from refreshed handles.
        if (board.AOApplyColor.IsValid())
            board.PostProcessColor = board.AOApplyColor;
        else if (board.SSSColor.IsValid())
            board.PostProcessColor = board.SSSColor;
        else
            board.PostProcessColor = board.SceneColor;

        // Phase D Slice 3: Bloom mip-chain scratch framebuffers.
        for (u32 i = 0; i < 5u; ++i)
        {
            const std::string mipName = "BloomMip" + std::to_string(i);
            board.BloomMips[i] = s_Data.RGraph->GetFramebufferHandle(mipName);
        }
        // Phase D Slice 4: GTAO edge scratch texture.
        board.GTAOEdge = s_Data.RGraph->GetTextureHandle("GTAOEdge");
        // Phase D Slice 6: HZB depth pyramid scratch texture.
        board.HZBDepth = s_Data.RGraph->GetTextureHandle(ResourceNames::HZBDepth);
        // Phase D Slice 5: Water refraction scratch texture.
        board.WaterRefraction = s_Data.RGraph->GetTextureHandle("WaterRefraction");
    }

    void Renderer3D::EndScene()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("Renderer3D::EndScene: Render graph is null!");
            return;
        }

        ConfigureFrameGraphPassesForFrame();

        // Populate the graph blackboard AFTER per-frame pass configuration so
        // AOBuffer / PostProcessColor imports resolve the current frame's
        // active technique and enabled outputs rather than last frame's state.
        SetupFrameBlackboard();

        UploadFrameGraphExecutionState();

        // Phase C: compile graph-native pass declarations before execution.
        s_Data.RGraph->BuildFrameGraph();

        {
            const auto& buildStats = s_Data.RGraph->GetLastBuildStats();
            static RenderGraph::FrameBuildStats s_LastBuildStats{};
            static bool s_HasLastBuildStats = false;

            const bool changed = !s_HasLastBuildStats ||
                                 buildStats.PassesVisited != s_LastBuildStats.PassesVisited ||
                                 buildStats.DeclaredReads != s_LastBuildStats.DeclaredReads ||
                                 buildStats.DeclaredWrites != s_LastBuildStats.DeclaredWrites ||
                                 buildStats.DerivedEdges != s_LastBuildStats.DerivedEdges;

            if (changed)
            {
                if (IsRenderGraphDiagnosticsEnabled())
                {
                    OLO_CORE_TRACE("RenderGraph BuildFrameGraph stats: passes={}, reads={}, writes={}, derivedEdges={}",
                                   buildStats.PassesVisited,
                                   buildStats.DeclaredReads,
                                   buildStats.DeclaredWrites,
                                   buildStats.DerivedEdges);
                }
                s_LastBuildStats = buildStats;
                s_HasLastBuildStats = true;
            }
        }

        // Phase D Slice 1: after BuildFrameGraph, stable handles for transient resources
        // are assigned. Populate the blackboard so Execute callbacks can resolve them.
        RefreshCompiledFrameGraphBlackboard();
        s_Data.RGraph->Execute();

        // End occlusion query frame after render graph execution
        if (s_Data.OcclusionCullingEnabled)
        {
            OcclusionQueryPool::GetInstance().EndFrame();
        }

        // Store current VP as previous for next frame's motion blur
        s_Data.PrevViewProjectionMatrix = s_Data.ViewProjectionMatrix;

        // Don't return the allocator to the pool - it's managed by FrameResourceManager
        // The allocator will be reset at the start of the next frame when this buffer is reused
        const auto clearRenderStreamAllocator = [](PassGraphNode* node)
        {
            if (node)
                node->SetCommandAllocator(nullptr);
        };
        s_Data.StreamNodes.ForEach(clearRenderStreamAllocator);

        RendererProfiler::GetInstance().EndFrame();

        // End frame for double-buffered resources (inserts GPU fence)
        FrameResourceManager::Get().EndFrame();
    }
} // namespace OloEngine
