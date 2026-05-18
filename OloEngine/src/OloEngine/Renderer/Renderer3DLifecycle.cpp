#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"

#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Asset/AssetManager.h"

#include <chrono>
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/ShaderWarmup.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <atomic>
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

    Renderer3D::Renderer3DData Renderer3D::s_Data;
    ShaderLibrary Renderer3D::m_ShaderLibrary;

    void Renderer3D::Init(Window* loadingWindow)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing Renderer3D.");

        RendererProfiler::GetInstance().Initialize();

        // Query driver MSAA caps once so the settings panel and the
        // ApplyRendererSettings clamp logic have the true max the GPU
        // supports. We take the min of colour-attachment and depth-texture
        // caps because the G-Buffer needs matching sample counts on both.
        {
            GLint colorSamples = 0;
            GLint depthSamples = 0;
            glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &colorSamples);
            glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &depthSamples);
            s_Data.MaxMSAASamplesColor = static_cast<u32>(std::max(colorSamples, 1));
            s_Data.MaxMSAASamplesDepth = static_cast<u32>(std::max(depthSamples, 1));
            OLO_CORE_INFO("Renderer3D: Driver MSAA caps — GL_MAX_COLOR_TEXTURE_SAMPLES={}, "
                          "GL_MAX_DEPTH_TEXTURE_SAMPLES={} (usable max = {})",
                          s_Data.MaxMSAASamplesColor,
                          s_Data.MaxMSAASamplesDepth,
                          std::min(s_Data.MaxMSAASamplesColor, s_Data.MaxMSAASamplesDepth));
        }

        FrameDataBufferManager::Init();
        FrameResourceManager::Get().Init();

        CommandDispatch::Initialize();
        OLO_CORE_INFO("CommandDispatch system initialized.");

        s_Data.CubeMesh = MeshPrimitives::CreateCube();
        s_Data.QuadMesh = MeshPrimitives::CreatePlane(1.0f, 1.0f);
        s_Data.SkyboxMesh = MeshPrimitives::CreateSkyboxCube();
        // Cached unit line quad (length 1 along +X, centered on X with half-thickness of 0.5 on Y)
        // We'll scale/rotate/translate this via a transform in DrawLine.
        {
            std::vector<Vertex> verts;
            verts.reserve(4);
            // Define a unit line along +X from 0 to 1, quad thickness 1 in Y (will be scaled by desired thickness)
            verts.emplace_back(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f));
            verts.emplace_back(glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(1.0f, 0.0f));
            verts.emplace_back(glm::vec3(1.0f, 0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(1.0f));
            verts.emplace_back(glm::vec3(1.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f, 1.0f));

            std::vector<u32> inds = { 0, 1, 2, 2, 3, 0 };

            auto src = Ref<MeshSource>::Create(verts, inds);
            Submesh sm;
            sm.m_BaseVertex = 0;
            sm.m_BaseIndex = 0;
            sm.m_IndexCount = static_cast<u32>(inds.size());
            sm.m_VertexCount = static_cast<u32>(verts.size());
            sm.m_MaterialIndex = 0;
            sm.m_IsRigged = false;
            sm.m_NodeName = "LineQuad";
            src->AddSubmesh(sm);
            src->Build();
            s_Data.LineQuadMesh = Ref<Mesh>::Create(src, 0);
        }

        // Create fullscreen quad VAO for grid and post-processing
        {
            // NDC fullscreen quad vertices (position only)
            float quadVertices[] = {
                // positions
                -1.0f, -1.0f, 0.0f,
                1.0f, -1.0f, 0.0f,
                1.0f, 1.0f, 0.0f,
                -1.0f, -1.0f, 0.0f,
                1.0f, 1.0f, 0.0f,
                -1.0f, 1.0f, 0.0f
            };

            s_Data.FullscreenQuadVAO = VertexArray::Create();
            Ref<VertexBuffer> quadVBO = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
            quadVBO->SetLayout({ { ShaderDataType::Float3, "a_Position" } });
            s_Data.FullscreenQuadVAO->AddVertexBuffer(quadVBO);
        }

        u32 shaderIdx = 0;
        // NOTE: Keep totalShaders3D in sync with the number of Load() calls below.
        constexpr u32 totalShaders3D = 41;

        // Boot + fallback are idempotent — no-ops when already initialized by
        // Renderer::Init().  Needed here for the lazy-init path (EditorLayer
        // calls Renderer3D::Init() directly without going through Renderer::Init).
        ShaderWarmup::Init();
        ShaderLibrary::InitFallbackShader();

        Window* window = loadingWindow;

        // All Load() calls issue glLinkProgram() back-to-back WITHOUT checking
        // GL_LINK_STATUS. When GL_ARB_parallel_shader_compile is available the
        // driver links them all in parallel. Status is checked later via
        // PollPendingShaders() each frame from `RenderPipeline::PrepareFrame()`.
        static constexpr std::array s_ShaderPaths = {
            "assets/shaders/LightCube.glsl",
            "assets/shaders/Lighting3D.glsl",
            "assets/shaders/SkinnedLighting3D_Simple.glsl",
            "assets/shaders/Renderer3D_Quad.glsl",
            "assets/shaders/PBR.glsl",
            "assets/shaders/PBR_Skinned.glsl",
            "assets/shaders/PBR_MultiLight.glsl",
            "assets/shaders/PBR_MultiLight_Skinned.glsl",
            "assets/shaders/PBR_GBuffer.glsl",
            "assets/shaders/PBR_GBuffer_Skinned.glsl",
            "assets/shaders/EquirectangularToCubemap.glsl",
            "assets/shaders/IrradianceConvolution.glsl",
            "assets/shaders/IBLPrefilter.glsl",
            "assets/shaders/BRDFLutGeneration.glsl",
            "assets/shaders/Skybox.glsl",
            "assets/shaders/Skybox_GBuffer.glsl",
            "assets/shaders/LightCube_GBuffer.glsl",
            "assets/shaders/InfiniteGrid.glsl",
            "assets/shaders/InfiniteGrid_GBuffer.glsl",
            "assets/shaders/ShadowDepth.glsl",
            "assets/shaders/ShadowDepthSkinned.glsl",
            "assets/shaders/ShadowDepthPoint.glsl",
            "assets/shaders/ShadowDepthPointSkinned.glsl",
            "assets/shaders/Terrain_PBR.glsl",
            "assets/shaders/Terrain_GBuffer.glsl",
            "assets/shaders/Terrain_Depth.glsl",
            "assets/shaders/Terrain_Voxel.glsl",
            "assets/shaders/Terrain_Voxel_GBuffer.glsl",
            "assets/shaders/Terrain_VoxelDepth.glsl",
            "assets/shaders/Foliage_Instance.glsl",
            "assets/shaders/Foliage_Instance_GBuffer.glsl",
            "assets/shaders/Foliage_Depth.glsl",
            "assets/shaders/Water.glsl",
            "assets/shaders/Decal.glsl",
            "assets/shaders/Decal_OIT.glsl",
            "assets/shaders/Decal_GBuffer.glsl",
            "assets/shaders/Decal_GBuffer_Normal.glsl",
            "assets/shaders/Decal_GBuffer_RMA.glsl",
            "assets/shaders/Decal_GBuffer_Emissive.glsl",
            "assets/shaders/OcclusionProxy.glsl",
            "assets/shaders/ForwardPlusDebug.glsl",
        };
        static_assert(s_ShaderPaths.size() == totalShaders3D);

        for (const auto* path : s_ShaderPaths)
        {
            m_ShaderLibrary.Load(path);
            ShaderWarmup::RenderProgressFrame(static_cast<f32>(++shaderIdx) / totalShaders3D, window, "3D shaders", static_cast<i32>(shaderIdx), static_cast<i32>(totalShaders3D), 1);
        }

        // Log how many shaders are still compiling asynchronously
        if (const u32 pending = m_ShaderLibrary.GetPendingCount(); pending > 0)
        {
            OLO_CORE_INFO("{} of {} shaders issued for async linking", pending, totalShaders3D);
        }

        // Display a loading screen with progress bar while shaders finish linking.
        // This blocks here until ALL shaders are Ready, keeping the window responsive.
        ShaderWarmup::RunWarmupScreen(m_ShaderLibrary, window);

        s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
        s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");
        s_Data.SkinnedLightingShader = m_ShaderLibrary.Get("SkinnedLighting3D_Simple");
        s_Data.QuadShader = m_ShaderLibrary.Get("Renderer3D_Quad");
        s_Data.PBRShader = m_ShaderLibrary.Get("PBR_MultiLight");
        s_Data.PBRSkinnedShader = m_ShaderLibrary.Get("PBR_MultiLight_Skinned");
        s_Data.PBRMultiLightShader = m_ShaderLibrary.Get("PBR_MultiLight");
        s_Data.PBRMultiLightSkinnedShader = m_ShaderLibrary.Get("PBR_MultiLight_Skinned");
        s_Data.PBRGBufferShader = m_ShaderLibrary.Get("PBR_GBuffer");
        s_Data.PBRGBufferSkinnedShader = m_ShaderLibrary.Get("PBR_GBuffer_Skinned");
        s_Data.SkyboxShader = m_ShaderLibrary.Get("Skybox");
        s_Data.SkyboxGBufferShader = m_ShaderLibrary.Get("Skybox_GBuffer");
        s_Data.LightCubeGBufferShader = m_ShaderLibrary.Get("LightCube_GBuffer");
        s_Data.InfiniteGridShader = m_ShaderLibrary.Get("InfiniteGrid");
        s_Data.InfiniteGridGBufferShader = m_ShaderLibrary.Get("InfiniteGrid_GBuffer");
        s_Data.ForwardPlusDebugShader = m_ShaderLibrary.Get("ForwardPlusDebug");
        s_Data.ShadowDepthShader = m_ShaderLibrary.Get("ShadowDepth");
        s_Data.ShadowDepthSkinnedShader = m_ShaderLibrary.Get("ShadowDepthSkinned");
        s_Data.ShadowDepthPointSkinnedShader = m_ShaderLibrary.Get("ShadowDepthPointSkinned");
        s_Data.TerrainPBRShader = m_ShaderLibrary.Get("Terrain_PBR");
        s_Data.TerrainGBufferShader = m_ShaderLibrary.Get("Terrain_GBuffer");
        s_Data.TerrainDepthShader = m_ShaderLibrary.Get("Terrain_Depth");
        s_Data.VoxelPBRShader = m_ShaderLibrary.Get("Terrain_Voxel");
        s_Data.VoxelGBufferShader = m_ShaderLibrary.Get("Terrain_Voxel_GBuffer");
        s_Data.VoxelDepthShader = m_ShaderLibrary.Get("Terrain_VoxelDepth");
        s_Data.FoliageShader = m_ShaderLibrary.Get("Foliage_Instance");
        s_Data.FoliageGBufferShader = m_ShaderLibrary.Get("Foliage_Instance_GBuffer");
        s_Data.FoliageDepthShader = m_ShaderLibrary.Get("Foliage_Depth");
        s_Data.WaterShader = m_ShaderLibrary.Get("Water");
        s_Data.DecalShader = m_ShaderLibrary.Get("Decal");
        s_Data.DecalGBufferShader = m_ShaderLibrary.Get("Decal_GBuffer");
        s_Data.DecalGBufferNormalShader = m_ShaderLibrary.Get("Decal_GBuffer_Normal");
        s_Data.DecalGBufferRMAShader = m_ShaderLibrary.Get("Decal_GBuffer_RMA");
        s_Data.DecalGBufferEmissiveShader = m_ShaderLibrary.Get("Decal_GBuffer_Emissive");
        s_Data.DecalCubeMesh = MeshPrimitives::CreateCube();
        s_Data.WhiteTexture = Texture2D::Create(TextureSpecification());
        u32 whiteTextureData = 0xffffffffU;
        s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(u32));

        s_Data.SharedSceneUBOs.Camera = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        s_Data.SharedSceneUBOs.LightProperties = UniformBuffer::Create(ShaderBindingLayout::LightUBO::GetSize(), ShaderBindingLayout::UBO_LIGHTS);
        // Allocate enough for the larger PBR layout (PBRMaterialUBO > MaterialUBO)
        constexpr u32 materialBufferSize = std::max(ShaderBindingLayout::MaterialUBO::GetSize(), ShaderBindingLayout::PBRMaterialUBO::GetSize());
        s_Data.SharedSceneUBOs.Material = UniformBuffer::Create(materialBufferSize, ShaderBindingLayout::UBO_MATERIAL);
        // Validate the MultiLightUBO fits within the GPU's uniform block size limit.
        // MAX_LIGHTS=256 produces ~20 KB which exceeds the GL spec minimum of 16 KB
        // but is within typical desktop GPU limits (64 KB+).
        {
            const u32 maxUBOSize = RenderCommand::GetMaxUniformBlockSize();
            constexpr u32 multiLightSize = ShaderBindingLayout::MultiLightUBO::GetSize();
            if (multiLightSize > maxUBOSize)
            {
                OLO_CORE_ERROR("MultiLightUBO size ({} bytes) exceeds GL_MAX_UNIFORM_BLOCK_SIZE ({} bytes). "
                               "Reduce MAX_LIGHTS or migrate to an SSBO. Multi-light path disabled.",
                               multiLightSize, maxUBOSize);
            }
            else
            {
                s_Data.MultiLightBuffer = UniformBuffer::Create(multiLightSize, ShaderBindingLayout::UBO_MULTI_LIGHTS);
            }
        }

        // Per-draw instance SSBO at SSBO_INSTANCE_DATA = 15. The legacy
        // ModelMatrixUBO at binding 3 has been retired — every mesh shader
        // now reads transforms from this SSBO via InstanceBlock.glsl. Initial
        // capacity 1 covers the non-batched path; CommandBucket auto-batching
        // grows it on demand via InstanceBuffer::EnsureCapacity().
        s_Data.ModelInstanceBuffer = Ref<InstanceBuffer>::Create(1);
        s_Data.BoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION);
        s_Data.PrevBoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION_PREV);
        s_Data.TerrainUBO = UniformBuffer::Create(ShaderBindingLayout::TerrainUBO::GetSize(), ShaderBindingLayout::UBO_TERRAIN);
        s_Data.FoliageUBO = UniformBuffer::Create(ShaderBindingLayout::FoliageUBO::GetSize(), ShaderBindingLayout::UBO_FOLIAGE);
        s_Data.WaterUBO = UniformBuffer::Create(ShaderBindingLayout::WaterUBO::GetSize(), ShaderBindingLayout::UBO_WATER);
        s_Data.PostProcessGPU.PostProcess = UniformBuffer::Create(PostProcessUBOData::GetSize(), ShaderBindingLayout::UBO_USER_0);
        s_Data.PostProcessGPU.MotionBlur = UniformBuffer::Create(MotionBlurUBOData::GetSize(), ShaderBindingLayout::UBO_USER_1);
        s_Data.PostProcessGPU.SSAO = UniformBuffer::Create(SSAOUBOData::GetSize(), ShaderBindingLayout::UBO_SSAO);
        s_Data.PostProcessGPU.GTAO = UniformBuffer::Create(UBOStructures::GTAOUBO::GetSize(), ShaderBindingLayout::UBO_GTAO);
        s_Data.SceneEffectsGPU.Snow = UniformBuffer::Create(SnowUBOData::GetSize(), ShaderBindingLayout::UBO_SNOW);
        s_Data.SceneEffectsGPU.SSS = UniformBuffer::Create(SSSUBOData::GetSize(), ShaderBindingLayout::UBO_SSS);
        s_Data.SceneEffectsGPU.Fog = UniformBuffer::Create(FogUBOData::GetSize(), ShaderBindingLayout::UBO_FOG);
        s_Data.SceneEffectsGPU.FogVolumes = UniformBuffer::Create(FogVolumesUBOData::GetSize(), ShaderBindingLayout::UBO_FOG_VOLUMES);
        s_Data.SceneEffectsGPU.FogVolumesData = FogVolumesUBOData{};
        s_Data.DecalUBO = UniformBuffer::Create(ShaderBindingLayout::DecalUBO::GetSize(), ShaderBindingLayout::UBO_DECAL);
        s_Data.LightProbeVolumeUBO = UniformBuffer::Create(ShaderBindingLayout::LightProbeVolumeUBO::GetSize(), ShaderBindingLayout::UBO_LIGHT_PROBES);
        s_Data.SceneEffectsGPU.DRS = UniformBuffer::Create(DRSUBOData::GetSize(), ShaderBindingLayout::UBO_DRS);

        // Initialize light probe UBO with disabled state and create a small zeroed SSBO
        // so shaders always have valid bindings at SSBO_LIGHT_PROBES
        {
            ShaderBindingLayout::LightProbeVolumeUBO disabledProbeUBO{};
            s_Data.LightProbeVolumeUBO->SetData(&disabledProbeUBO, ShaderBindingLayout::LightProbeVolumeUBO::GetSize());
            constexpr u32 dummySSBOSize = 16; // Minimum valid SSBO (one vec4)
            s_Data.LightProbeSHBuffer = StorageBuffer::Create(dummySSBOSize, ShaderBindingLayout::SSBO_LIGHT_PROBES);
            std::array<u8, dummySSBOSize> zeros{};
            s_Data.LightProbeSHBuffer->SetData(zeros.data(), dummySSBOSize);
        }

        CommandDispatch::SetUBOReferences(
            s_Data.SharedSceneUBOs.Camera,
            s_Data.SharedSceneUBOs.Material,
            s_Data.SharedSceneUBOs.LightProperties,
            s_Data.BoneMatricesUBO,
            s_Data.ModelInstanceBuffer,
            s_Data.PrevBoneMatricesUBO,
            &s_Data.ForwardPlus);

        EnvironmentMap::InitializeIBLSystem(m_ShaderLibrary);
        OLO_CORE_INFO("IBL system initialized.");

        s_Data.SceneLight.Type = LightType::Directional;
        s_Data.SceneLight.Position = glm::vec3(1.2f, 1.0f, 2.0f);
        s_Data.SceneLight.Direction = glm::vec3(-0.2f, -1.0f, -0.3f);
        s_Data.SceneLight.Ambient = glm::vec3(0.2f, 0.2f, 0.2f);
        s_Data.SceneLight.Diffuse = glm::vec3(0.5f, 0.5f, 0.5f);
        s_Data.SceneLight.Specular = glm::vec3(1.0f, 1.0f, 1.0f);
        s_Data.SceneLight.Constant = 1.0f;
        s_Data.SceneLight.Linear = 0.09f;
        s_Data.SceneLight.Quadratic = 0.032f;

        s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);

        s_Data.Stats.Reset();

        s_Data.RGraph = Ref<RenderGraph>::Create();
        RenderGraphDebugRuntime::SetActiveGraph(s_Data.RGraph);
        // Headless init (window == nullptr) uses a placeholder framebuffer size;
        // the real size is applied later via Renderer3D::OnWindowResize.
        const u32 fbWidth = window ? window->GetFramebufferWidth() : 1280u;
        const u32 fbHeight = window ? window->GetFramebufferHeight() : 720u;
        SetupRenderGraph(fbWidth, fbHeight);

        // Initialize shadow mapping
        s_Data.Shadow.Init();

        ParticleBatchRenderer::Init();

        // Initialize wind system (3D wind-field volume)
        WindSystem::Init();

        // Initialize snow accumulation & ejecta systems
        SnowAccumulationSystem::Init();
        SnowEjectaSystem::Init(s_Data.SnowEjecta.MaxParticles);

        // Initialize precipitation system (use default max particle counts from settings)
        PrecipitationSystem::Init(s_Data.Precipitation.MaxParticlesNearField,
                                  s_Data.Precipitation.MaxParticlesFarField);
        ScreenSpacePrecipitation::Init();

        // Initialize fog temporal state
        s_Data.FogFrameIndex = 0;
        s_Data.FogLastTime = std::chrono::steady_clock::now();
        s_Data.FogTime = 0.0f;

        // Initialize Forward+ light culling system
        s_Data.ForwardPlus.Initialize(fbWidth, fbHeight);

        OLO_CORE_INFO("Renderer3D initialization complete.");
    }

    bool Renderer3D::IsInitialized()
    {
        return s_Data.RGraph != nullptr && s_Data.Pipeline->FrameCorePasses.Scene != nullptr;
    }

    void Renderer3D::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Shutting down Renderer3D.");

        // Flush any shaders still compiling asynchronously
        m_ShaderLibrary.FlushPendingShaders();

        ParticleBatchRenderer::Shutdown();

        // Shutdown occlusion culling systems
        OcclusionCuller::GetInstance().Shutdown();
        OcclusionQueryPool::GetInstance().Shutdown();
        OcclusionStateManager::GetInstance().Clear();

        // Shutdown wind system
        WindSystem::Shutdown();

        // Shutdown precipitation system
        ScreenSpacePrecipitation::Shutdown();
        PrecipitationSystem::Shutdown();

        // Shutdown snow systems
        SnowEjectaSystem::Shutdown();
        SnowAccumulationSystem::Shutdown();

        // Shutdown Forward+ system
        s_Data.ForwardPlus.Shutdown();

        // Shutdown shadow mapping
        s_Data.Shadow.Shutdown();
        // Release the static placeholder shadow textures lazy-initialised on
        // first bind-when-real-shadow-absent. Idempotent.
        ShadowMap::ShutdownPlaceholders();

        // Clear any pending GPU resource commands
        GPUResourceQueue::Clear();

        // Reset fog temporal state
        s_Data.FogFrameIndex = 0;
        s_Data.FogLastTime = {};
        s_Data.FogTime = 0.0f;

        if (s_Data.RGraph)
        {
            s_Data.RGraph->Shutdown();
        }

        RenderGraphDebugRuntime::SetActiveGraph(nullptr);

        // Release all render passes now while the GL context and RendererAPI are still alive.
        // Their destructors call RenderCommand::DeleteTexture() which needs s_RendererAPI.
        s_Data.Pipeline->Reset();
        s_Data.RGraph.Reset();

        // Release UBOs explicitly while the GL context is still alive
        s_Data.SharedSceneUBOs.Reset();
        s_Data.MultiLightBuffer.Reset();
        s_Data.ModelInstanceBuffer.Reset();
        s_Data.BoneMatricesUBO.Reset();
        s_Data.TerrainUBO.Reset();
        s_Data.FoliageUBO.Reset();
        s_Data.WaterUBO.Reset();
        s_Data.PostProcessGPU.Reset();
        s_Data.SceneEffectsGPU.Reset();
        s_Data.DecalUBO.Reset();
        s_Data.LightProbeVolumeUBO.Reset();
        s_Data.LightProbeSHBuffer.Reset();
        s_Data.PrevBoneMatricesUBO.Reset();

        MeshPrimitives::Shutdown();

        FrameResourceManager::Get().Shutdown();
        FrameDataBufferManager::Shutdown();

        // Boot/fallback shader shutdown is handled by Renderer::Shutdown()
        // (both are idempotent, but no need to call them twice).

        RendererProfiler::GetInstance().Shutdown();

        OLO_CORE_INFO("Renderer3D shutdown complete.");
    }

    void Renderer3D::OnAssetReloaded(const AssetReloadedEvent& e)
    {
        OLO_PROFILE_FUNCTION();

        AssetHandle handle = e.GetHandle();
        AssetType type = e.GetAssetType();
        u32 generation = AssetManager::GetAssetGeneration(handle);

        OLO_CORE_INFO("Renderer3D::OnAssetReloaded: handle={}, type={}, generation={}, path={}",
                      static_cast<u64>(handle), static_cast<int>(type), generation, e.GetPath().string());

        // Commands are rebuilt each frame from Material/Mesh objects.
        // When AssetManager reloads an asset in-place, the Ref<T> smart
        // pointers still point to the same object whose internal state has
        // been refreshed, so the NEXT frame's DrawMesh() / DrawAnimatedMesh()
        // will naturally pick up new RendererIDs.
        //
        // No per-command patching is needed because:
        //  1. Command buckets are cleared every frame — stale IDs survive
        //     at most one frame.
        //  2. Material/Render-state tables in FrameDataBuffer are rebuilt
        //     each frame via Reset().
        //
        // If a cached Ref<Shader> in s_Data is the reloaded asset, the
        // Ref still points to the same (now updated) Shader object.
    }

    void Renderer3D::OnWindowResize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Renderer3D::OnWindowResize: Resizing to {}x{}", width, height);

        if (width == 0 || height == 0)
        {
            return;
        }

        // If render graph exists but passes were never created (Init ran with 0x0 window),
        // create them now that we have valid dimensions.
        if (s_Data.RGraph && !s_Data.Pipeline->FrameCorePasses.Scene)
        {
            if (IsRenderGraphDiagnosticsEnabled())
            {
                OLO_CORE_TRACE("Renderer3D::OnWindowResize: ScenePass missing - running deferred SetupRenderGraph");
            }

            SetupRenderGraph(width, height);
            s_Data.ForwardPlus.Initialize(width, height);
            return; // Initialize already configured for width x height
        }

        if (s_Data.RGraph)
        {
            s_Data.RGraph->Resize(width, height);
        }
        else
        {
            OLO_CORE_WARN("Renderer3D::OnWindowResize: No render graph available!");
        }

        // Resize Forward+ light grid
        s_Data.ForwardPlus.Resize(width, height);
    }
} // namespace OloEngine
