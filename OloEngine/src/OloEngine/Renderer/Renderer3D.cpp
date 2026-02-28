#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
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
#include "OloEngine/Renderer/Commands/CommandMemoryManager.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Passes/PostProcessRenderPass.h"

#include <chrono>
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/Containers/Array.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>

namespace OloEngine
{
    static bool s_ForceDisableCulling = false;

    Renderer3D::Renderer3DData Renderer3D::s_Data;
    ShaderLibrary Renderer3D::m_ShaderLibrary;

    // Helper function to compute depth from camera space for sort key
    // Returns a quantized depth value in range [0, 0xFFFFFF] for 24-bit depth
    // @param modelMatrix The model transformation matrix
    // @param boundingSphereCenter Optional world-space bounding sphere center. If provided,
    //        uses this for more accurate depth sorting for off-center meshes. If nullptr,
    //        falls back to using modelMatrix[3] (the origin of the transformed object).
    static u32 ComputeDepthForSortKey(const glm::mat4& modelMatrix, const glm::vec3* boundingSphereCenter = nullptr)
    {
        // Transform object center to view space using CommandDispatch's cached view matrix
        const glm::mat4& viewMatrix = CommandDispatch::GetViewMatrix();

        // Use bounding sphere center if provided, otherwise use model origin
        glm::vec4 worldPos = boundingSphereCenter
                                 ? glm::vec4(*boundingSphereCenter, 1.0f)
                                 : modelMatrix[3];
        glm::vec4 viewPos = viewMatrix * worldPos;

        // Use negative Z since camera looks down -Z axis
        f32 depth = -viewPos.z;

        // Clamp depth to reasonable range [0, 1000] and quantize to 24 bits
        constexpr f32 MIN_DEPTH = 0.1f;
        constexpr f32 MAX_DEPTH = 1000.0f;
        depth = glm::clamp(depth, MIN_DEPTH, MAX_DEPTH);
        f32 normalizedDepth = (depth - MIN_DEPTH) / (MAX_DEPTH - MIN_DEPTH);
        return static_cast<u32>(normalizedDepth * 0xFFFFFF);
    }

    // Helper to generate material ID hash for sort key
    static u32 ComputeMaterialID(const Material& material)
    {
        u64 hash = 0;

        if (material.GetType() == MaterialType::PBR)
        {
            // Use PBR textures for hashing
            u64 albedoID = material.GetAlbedoMap() ? static_cast<u64>(material.GetAlbedoMap()->GetRendererID()) : 0ULL;
            u64 metallicID = material.GetMetallicRoughnessMap() ? static_cast<u64>(material.GetMetallicRoughnessMap()->GetRendererID()) : 0ULL;
            u64 normalID = material.GetNormalMap() ? static_cast<u64>(material.GetNormalMap()->GetRendererID()) : 0ULL;

            hash = albedoID;
            hash ^= metallicID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
            hash ^= normalID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
        }
        else
        {
            // Use legacy textures
            u64 diffuseID = material.GetDiffuseMap() ? static_cast<u64>(material.GetDiffuseMap()->GetRendererID()) : 0ULL;
            u64 specularID = material.GetSpecularMap() ? static_cast<u64>(material.GetSpecularMap()->GetRendererID()) : 0ULL;

            hash = diffuseID;
            hash ^= specularID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
        }

        // Fold 64-bit hash to 16-bit material ID (as defined in DrawKey)
        return static_cast<u32>((hash ^ (hash >> 32)) & 0xFFFF);
    }

    // Helper to create default POD render state
    static PODRenderState CreateDefaultPODRenderState()
    {
        PODRenderState state{};
        // All fields are initialized to sensible defaults by the struct itself
        return state;
    }

    // Helper to populate POD render state from material properties
    // Maps MaterialFlag to PODRenderState for proper render state setup
    static PODRenderState CreatePODRenderStateForMaterial(const Material& material)
    {
        PODRenderState state{};

        // Depth test - most materials want this enabled
        state.depthTestEnabled = material.GetFlag(MaterialFlag::DepthTest);
        state.depthWriteMask = true; // Write to depth buffer for opaque materials
        state.depthFunction = GL_LESS;

        // Blend state - for transparent materials
        if (material.GetFlag(MaterialFlag::Blend))
        {
            state.blendEnabled = true;
            state.blendSrcFactor = GL_SRC_ALPHA;
            state.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
            state.blendEquation = GL_FUNC_ADD;
            // Transparent objects typically don't write to depth buffer
            state.depthWriteMask = false;
        }
        else
        {
            state.blendEnabled = false;
        }

        // Culling - controlled by TwoSided flag
        if (material.GetFlag(MaterialFlag::TwoSided))
        {
            // Double-sided materials don't cull any faces
            state.cullingEnabled = false;
        }
        else
        {
            state.cullingEnabled = true;
            state.cullFace = GL_BACK;
        }

        return state;
    }

    void Renderer3D::Init()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing Renderer3D.");

        RendererProfiler::GetInstance().Initialize();

        CommandMemoryManager::Init();
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
            sm.m_IndexCount = (u32)inds.size();
            sm.m_VertexCount = (u32)verts.size();
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

        m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
        m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");
        m_ShaderLibrary.Load("assets/shaders/SkinnedLighting3D_Simple.glsl");
        m_ShaderLibrary.Load("assets/shaders/Renderer3D_Quad.glsl");
        m_ShaderLibrary.Load("assets/shaders/PBR.glsl");
        m_ShaderLibrary.Load("assets/shaders/PBR_Skinned.glsl");
        m_ShaderLibrary.Load("assets/shaders/PBR_MultiLight.glsl");
        m_ShaderLibrary.Load("assets/shaders/PBR_MultiLight_Skinned.glsl");
        m_ShaderLibrary.Load("assets/shaders/EquirectangularToCubemap.glsl");
        m_ShaderLibrary.Load("assets/shaders/IrradianceConvolution.glsl");
        m_ShaderLibrary.Load("assets/shaders/IBLPrefilter.glsl");
        m_ShaderLibrary.Load("assets/shaders/BRDFLutGeneration.glsl");
        m_ShaderLibrary.Load("assets/shaders/Skybox.glsl");
        m_ShaderLibrary.Load("assets/shaders/InfiniteGrid.glsl");
        m_ShaderLibrary.Load("assets/shaders/ShadowDepth.glsl");
        m_ShaderLibrary.Load("assets/shaders/ShadowDepthSkinned.glsl");
        m_ShaderLibrary.Load("assets/shaders/ShadowDepthPoint.glsl");

        m_ShaderLibrary.Load("assets/shaders/Terrain_PBR.glsl");
        m_ShaderLibrary.Load("assets/shaders/Terrain_Depth.glsl");
        m_ShaderLibrary.Load("assets/shaders/Terrain_Voxel.glsl");
        m_ShaderLibrary.Load("assets/shaders/Terrain_VoxelDepth.glsl");
        m_ShaderLibrary.Load("assets/shaders/Foliage_Instance.glsl");
        m_ShaderLibrary.Load("assets/shaders/Foliage_Depth.glsl");

        s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
        s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");
        s_Data.SkinnedLightingShader = m_ShaderLibrary.Get("SkinnedLighting3D_Simple");
        s_Data.QuadShader = m_ShaderLibrary.Get("Renderer3D_Quad");
        s_Data.PBRShader = m_ShaderLibrary.Get("PBR_MultiLight");
        s_Data.PBRSkinnedShader = m_ShaderLibrary.Get("PBR_MultiLight_Skinned");
        s_Data.PBRMultiLightShader = m_ShaderLibrary.Get("PBR_MultiLight");
        s_Data.PBRMultiLightSkinnedShader = m_ShaderLibrary.Get("PBR_MultiLight_Skinned");
        s_Data.SkyboxShader = m_ShaderLibrary.Get("Skybox");
        s_Data.InfiniteGridShader = m_ShaderLibrary.Get("InfiniteGrid");
        s_Data.ShadowDepthShader = m_ShaderLibrary.Get("ShadowDepth");
        s_Data.ShadowDepthSkinnedShader = m_ShaderLibrary.Get("ShadowDepthSkinned");
        s_Data.TerrainPBRShader = m_ShaderLibrary.Get("Terrain_PBR");
        s_Data.TerrainDepthShader = m_ShaderLibrary.Get("Terrain_Depth");
        s_Data.VoxelPBRShader = m_ShaderLibrary.Get("Terrain_Voxel");
        s_Data.VoxelDepthShader = m_ShaderLibrary.Get("Terrain_VoxelDepth");
        s_Data.FoliageShader = m_ShaderLibrary.Get("Foliage_Instance");
        s_Data.FoliageDepthShader = m_ShaderLibrary.Get("Foliage_Depth");

        s_Data.CameraUBO = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        s_Data.LightPropertiesUBO = UniformBuffer::Create(ShaderBindingLayout::LightUBO::GetSize(), ShaderBindingLayout::UBO_LIGHTS);
        s_Data.MaterialUBO = UniformBuffer::Create(ShaderBindingLayout::MaterialUBO::GetSize(), ShaderBindingLayout::UBO_MATERIAL);
        s_Data.MultiLightBuffer = UniformBuffer::Create(ShaderBindingLayout::MultiLightUBO::GetSize(), ShaderBindingLayout::UBO_MULTI_LIGHTS);
        s_Data.ModelMatrixUBO = UniformBuffer::Create(ShaderBindingLayout::ModelUBO::GetSize(), ShaderBindingLayout::UBO_MODEL);
        s_Data.BoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION);
        s_Data.TerrainUBO = UniformBuffer::Create(ShaderBindingLayout::TerrainUBO::GetSize(), ShaderBindingLayout::UBO_TERRAIN);
        s_Data.FoliageUBO = UniformBuffer::Create(ShaderBindingLayout::FoliageUBO::GetSize(), ShaderBindingLayout::UBO_FOLIAGE);
        s_Data.PostProcessUBO = UniformBuffer::Create(PostProcessUBOData::GetSize(), ShaderBindingLayout::UBO_USER_0);
        s_Data.MotionBlurUBO = UniformBuffer::Create(MotionBlurUBOData::GetSize(), ShaderBindingLayout::UBO_USER_1);
        s_Data.SSAOUBO = UniformBuffer::Create(SSAOUBOData::GetSize(), ShaderBindingLayout::UBO_SSAO);
        s_Data.SnowUBO = UniformBuffer::Create(SnowUBOData::GetSize(), ShaderBindingLayout::UBO_SNOW);
        s_Data.SSSUBO = UniformBuffer::Create(SSSUBOData::GetSize(), ShaderBindingLayout::UBO_SSS);

        CommandDispatch::SetUBOReferences(
            s_Data.CameraUBO,
            s_Data.MaterialUBO,
            s_Data.LightPropertiesUBO,
            s_Data.BoneMatricesUBO,
            s_Data.ModelMatrixUBO);

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

        Window& window = Application::Get().GetWindow();
        s_Data.RGraph = Ref<RenderGraph>::Create();
        SetupRenderGraph(window.GetFramebufferWidth(), window.GetFramebufferHeight());

        // Initialize shadow mapping
        s_Data.Shadow.Init();

        ParticleBatchRenderer::Init();

        // Initialize wind system (3D wind-field volume)
        WindSystem::Init();

        OLO_CORE_INFO("Renderer3D initialization complete.");
    }

    bool Renderer3D::IsInitialized()
    {
        return s_Data.RGraph != nullptr && s_Data.ScenePass != nullptr;
    }

    void Renderer3D::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Shutting down Renderer3D.");

        ParticleBatchRenderer::Shutdown();

        // Shutdown wind system
        WindSystem::Shutdown();

        // Shutdown shadow mapping
        s_Data.Shadow.Shutdown();

        // Clear any pending GPU resource commands
        GPUResourceQueue::Clear();

        // Clear shader registries
        s_Data.ShaderRegistries.clear();

        if (s_Data.RGraph)
            s_Data.RGraph->Shutdown();

        // Release all render passes now while the GL context and RendererAPI are still alive.
        // Their destructors call RenderCommand::DeleteTexture() which needs s_RendererAPI.
        s_Data.ShadowPass.Reset();
        s_Data.ScenePass.Reset();
        s_Data.SSAOPass.Reset();
        s_Data.ParticlePass.Reset();
        s_Data.SSSPass.Reset();
        s_Data.PostProcessPass.Reset();
        s_Data.FinalPass.Reset();
        s_Data.RGraph.Reset();

        FrameResourceManager::Get().Shutdown();
        FrameDataBufferManager::Shutdown();

        RendererProfiler::GetInstance().Shutdown();

        OLO_CORE_INFO("Renderer3D shutdown complete.");
    }

    void Renderer3D::BeginSceneCommon()
    {
        OLO_PROFILE_FUNCTION();

        // Process any pending GPU resource creation commands from async loaders
        GPUResourceQueue::ProcessAll();

        // Begin new frame for double-buffered resources
        FrameResourceManager::Get().BeginFrame();

        RendererProfiler::GetInstance().BeginFrame();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::BeginScene: ScenePass is null!");
            return;
        }

        // Reset frame data buffer for new frame
        FrameDataBufferManager::Get().Reset();

        // Use frame resource manager's allocator for double-buffering
        CommandAllocator* frameAllocator = FrameResourceManager::Get().GetFrameAllocator();
        if (!frameAllocator)
        {
            // Fallback to legacy allocator if double-buffering is disabled
            frameAllocator = CommandMemoryManager::GetFrameAllocator();
        }
        s_Data.ScenePass->GetCommandBucket().SetAllocator(frameAllocator);

        CommandDispatch::SetViewProjectionMatrix(s_Data.ViewProjectionMatrix);
        CommandDispatch::SetViewMatrix(s_Data.ViewMatrix);
        CommandDispatch::SetProjectionMatrix(s_Data.ProjectionMatrix);

        s_Data.ViewFrustum.Update(s_Data.ViewProjectionMatrix);

        s_Data.Stats.Reset();
        s_Data.CommandCounter = 0;

        UpdateCameraMatricesUBO(s_Data.ViewMatrix, s_Data.ProjectionMatrix);
        UpdateLightPropertiesUBO();

        CommandDispatch::SetSceneLight(s_Data.SceneLight);
        CommandDispatch::SetViewPosition(s_Data.ViewPos);

        s_Data.ScenePass->ResetCommandBucket();

        CommandDispatch::ResetState();

        // Set shadow texture IDs AFTER ResetState() so they aren't zeroed out
        CommandDispatch::SetShadowTextureIDs(
            s_Data.Shadow.GetCSMRendererID(),
            s_Data.Shadow.GetSpotRendererID());

        // Set point shadow cubemap texture IDs
        {
            std::array<u32, ShadowMap::MAX_POINT_SHADOWS> pointIDs{};
            for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            {
                pointIDs[i] = s_Data.Shadow.GetPointRendererID(i);
            }
            CommandDispatch::SetPointShadowTextureIDs(pointIDs);
        }

        // Initialize parallel scene context with immutable frame data
        s_Data.ParallelContext.ViewMatrix = s_Data.ViewMatrix;
        s_Data.ParallelContext.ProjectionMatrix = s_Data.ProjectionMatrix;
        s_Data.ParallelContext.ViewProjectionMatrix = s_Data.ViewProjectionMatrix;
        s_Data.ParallelContext.ViewPosition = s_Data.ViewPos;
        s_Data.ParallelContext.ViewFrustum = s_Data.ViewFrustum;
        s_Data.ParallelContext.FrustumCullingEnabled = s_Data.FrustumCullingEnabled;
        s_Data.ParallelContext.DynamicCullingEnabled = s_Data.DynamicCullingEnabled;

        // Cache shader references for parallel access
        s_Data.ParallelContext.LightingShader = s_Data.LightingShader;
        s_Data.ParallelContext.SkinnedLightingShader = s_Data.SkinnedLightingShader;
        s_Data.ParallelContext.PBRShader = s_Data.PBRShader;
        s_Data.ParallelContext.PBRSkinnedShader = s_Data.PBRSkinnedShader;
        s_Data.ParallelContext.LightCubeShader = s_Data.LightCubeShader;
        s_Data.ParallelContext.SkyboxShader = s_Data.SkyboxShader;
        s_Data.ParallelContext.QuadShader = s_Data.QuadShader;

        s_Data.ParallelSubmissionActive = false;
    }

    void Renderer3D::BeginScene(const PerspectiveCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = camera.GetView();
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = camera.GetViewProjection();
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        BeginSceneCommon();
    }

    void Renderer3D::BeginScene(const EditorCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = camera.GetViewMatrix();
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        BeginSceneCommon();
    }

    void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = glm::inverse(transform);
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = glm::vec3(transform[3]);
        // Camera base class has no near/far — keep previous values

        BeginSceneCommon();
    }

    void Renderer3D::EndScene()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("Renderer3D::EndScene: Render graph is null!");
            return;
        }

        // Framebuffer piping is handled once in SetupRenderGraph() and on resize.
        // Only per-frame settings updates are needed here.

        if (s_Data.SSAOPass)
        {
            s_Data.SSAOPass->SetSettings(s_Data.PostProcess);

            // Upload projection matrices for SSAO position reconstruction
            s_Data.SSAOGPUData.Projection = s_Data.ProjectionMatrix;
            s_Data.SSAOGPUData.InverseProjection = glm::inverse(s_Data.ProjectionMatrix);
            s_Data.SSAOGPUData.DebugView = s_Data.PostProcess.SSAODebugView ? 1 : 0;
        }
        if (s_Data.SSSPass)
        {
            s_Data.SSSPass->SetSettings(s_Data.Snow);
        }
        if (s_Data.PostProcessPass)
        {
            s_Data.PostProcessPass->SetSettings(s_Data.PostProcess);
            s_Data.PostProcessPass->SetPostProcessUBO(s_Data.PostProcessUBO, &s_Data.PostProcessGPUData);

            // Pass SSAO texture to PostProcessPass for application
            if (s_Data.SSAOPass && s_Data.PostProcess.SSAOEnabled)
            {
                s_Data.PostProcessPass->SetSSAOTexture(s_Data.SSAOPass->GetSSAOTextureID());
            }
            else
            {
                s_Data.PostProcessPass->SetSSAOTexture(0);
            }
        }
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
            auto& gpu = s_Data.PostProcessGPUData;
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
            s_Data.PostProcessUBO->SetData(&gpu, PostProcessUBOData::GetSize());
        }

        // Upload snow settings to GPU
        if (s_Data.Snow.Enabled)
        {
            auto& snow = s_Data.Snow;
            auto& gpu = s_Data.SnowGPUData;
            gpu.CoverageParams = glm::vec4(snow.HeightStart, snow.HeightFull, snow.SlopeStart, snow.SlopeFull);
            gpu.AlbedoAndRoughness = glm::vec4(snow.Albedo, snow.Roughness);
            gpu.SSSColorAndIntensity = glm::vec4(snow.SSSColor, snow.SSSIntensity);
            gpu.SparkleParams = glm::vec4(snow.SparkleIntensity, snow.SparkleDensity, snow.SparkleScale, snow.NormalPerturbStrength);
            gpu.Flags = glm::vec4(1.0f, snow.WindDriftFactor, 0.0f, 0.0f);
            s_Data.SnowUBO->SetData(&gpu, SnowUBOData::GetSize());

            // SSS blur parameters
            auto& sssGpu = s_Data.SSSGPUData;
            sssGpu.BlurParams = glm::vec4(snow.SSSBlurRadius, snow.SSSBlurFalloff, 0.0f, 0.0f);
            sssGpu.Flags = glm::vec4(snow.SSSBlurEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            if (s_Data.ScenePass && s_Data.ScenePass->GetTarget())
            {
                const auto& spec = s_Data.ScenePass->GetTarget()->GetSpecification();
                sssGpu.BlurParams.z = static_cast<f32>(spec.Width);
                sssGpu.BlurParams.w = static_cast<f32>(spec.Height);
            }
            s_Data.SSSUBO->SetData(&sssGpu, SSSUBOData::GetSize());
        }
        else
        {
            // Upload disabled state so shaders skip snow
            auto& gpu = s_Data.SnowGPUData;
            gpu.Flags = glm::vec4(0.0f);
            s_Data.SnowUBO->SetData(&gpu, SnowUBOData::GetSize());
        }

        // Update wind system (regenerate 3D wind field, upload wind UBO)
        {
            // TODO: Pass actual frame dt once Timestep is threaded through BeginScene
            static auto lastTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            f32 dt = std::chrono::duration<f32>(now - lastTime).count();
            dt = std::clamp(dt, 0.0f, 0.1f);
            lastTime = now;

            WindSystem::Update(s_Data.Wind, s_Data.ViewPos, Timestep(dt));
            WindSystem::BindWindTexture();
        }

        // Upload motion blur matrices
        if (s_Data.PostProcess.MotionBlurEnabled)
        {
            auto& mb = s_Data.MotionBlurGPUData;
            mb.InverseViewProjection = glm::inverse(s_Data.ViewProjectionMatrix);
            mb.PrevViewProjection = s_Data.PrevViewProjectionMatrix;
            s_Data.MotionBlurUBO->SetData(&mb, MotionBlurUBOData::GetSize());
        }

        s_Data.RGraph->Execute();

        // Store current VP as previous for next frame's motion blur
        s_Data.PrevViewProjectionMatrix = s_Data.ViewProjectionMatrix;

        // Don't return the allocator to the pool - it's managed by FrameResourceManager
        // The allocator will be reset at the start of the next frame when this buffer is reused
        s_Data.ScenePass->GetCommandBucket().SetAllocator(nullptr);

        profiler.EndFrame();

        // End frame for double-buffered resources (inserts GPU fence)
        FrameResourceManager::Get().EndFrame();
    }

    // ========================================================================
    // Parallel Command Generation Implementation
    // ========================================================================

    void Renderer3D::BeginParallelSubmission()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::BeginParallelSubmission: ScenePass is null!");
            return;
        }

        // Prepare command bucket for parallel submission
        s_Data.ScenePass->GetCommandBucket().PrepareForParallelSubmission();

        // Prepare worker allocators
        CommandMemoryManager::PrepareWorkerAllocatorsForFrame();

        // Prepare frame data buffer for parallel submission
        FrameDataBufferManager::Get().PrepareForParallelSubmission();

        s_Data.ParallelSubmissionActive = true;

        OLO_CORE_TRACE("Renderer3D: Parallel submission mode enabled");
    }

    void Renderer3D::EndParallelSubmission()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ParallelSubmissionActive)
        {
            OLO_CORE_WARN("Renderer3D::EndParallelSubmission: Not in parallel submission mode!");
            return;
        }

        // Merge frame data scratch buffers
        FrameDataBufferManager::Get().MergeScratchBuffers();

        // Merge command bucket thread-local commands
        s_Data.ScenePass->GetCommandBucket().MergeThreadLocalCommands();

        // Remap bone offsets from worker-local to global
        // Must be done after both MergeScratchBuffers() and MergeThreadLocalCommands()
        s_Data.ScenePass->GetCommandBucket().RemapBoneOffsets(FrameDataBufferManager::Get());

        // Release worker allocators
        CommandMemoryManager::ReleaseWorkerAllocators();

        s_Data.ParallelSubmissionActive = false;

        OLO_CORE_TRACE("Renderer3D: Parallel submission mode disabled");
    }

    WorkerSubmitContext Renderer3D::GetWorkerContext(u32 workerIndex)
    {
        OLO_PROFILE_FUNCTION();

        WorkerSubmitContext ctx;

        // Get worker allocator by explicit index - no thread ID lookup needed
        auto [index, allocator] = CommandMemoryManager::GetWorkerAllocatorByIndex(workerIndex);
        ctx.WorkerIndex = index;
        ctx.Allocator = allocator;

        // Get command bucket
        if (s_Data.ScenePass)
        {
            ctx.Bucket = &s_Data.ScenePass->GetCommandBucket();
            // Use the explicit worker index - no thread ID lookup needed
            ctx.Bucket->UseWorkerIndex(workerIndex);
        }

        // Set scene context
        ctx.SceneContext = &s_Data.ParallelContext;

        ctx.CommandsSubmitted = 0;
        ctx.MeshesCulled = 0;

        return ctx;
    }

    const ParallelSceneContext* Renderer3D::GetParallelSceneContext()
    {
        return &s_Data.ParallelContext;
    }

    bool Renderer3D::IsParallelSubmissionActive()
    {
        return s_Data.ParallelSubmissionActive;
    }

    void Renderer3D::SubmitPacketParallel(WorkerSubmitContext& ctx, CommandPacket* packet)
    {
        OLO_PROFILE_FUNCTION();

        if (!packet)
        {
            OLO_CORE_WARN("Renderer3D::SubmitPacketParallel: Null packet!");
            return;
        }

        if (!ctx.Bucket)
        {
            OLO_CORE_ERROR("Renderer3D::SubmitPacketParallel: No bucket in context!");
            return;
        }

        ctx.Bucket->SubmitPacketParallel(packet, ctx.WorkerIndex);
        ctx.CommandsSubmitted++;
    }

    CommandPacket* Renderer3D::DrawMeshParallel(WorkerSubmitContext& ctx,
                                                const Ref<Mesh>& mesh,
                                                const glm::mat4& modelMatrix,
                                                const Material& material,
                                                bool isStatic,
                                                i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!ctx.Allocator || !ctx.SceneContext)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: Invalid worker context!");
            return nullptr;
        }

        // Frustum culling using parallel scene context
        if (ctx.SceneContext->FrustumCullingEnabled &&
            (isStatic || ctx.SceneContext->DynamicCullingEnabled))
        {
            if (mesh)
            {
                BoundingSphere sphere = mesh->GetTransformedBoundingSphere(modelMatrix);
                sphere.Radius *= 1.3f;

                if (!ctx.SceneContext->ViewFrustum.IsBoundingSphereVisible(sphere))
                {
                    ctx.MeshesCulled++;
                    return nullptr;
                }
            }
        }

        if (!mesh || !mesh->GetVertexArray())
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: Invalid mesh or vertex array!");
            return nullptr;
        }

        // Select shader from parallel context
        Ref<Shader> shaderToUse;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            shaderToUse = ctx.SceneContext->PBRShader;
        }
        else
        {
            shaderToUse = ctx.SceneContext->LightingShader;
        }

        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: No shader available!");
            return nullptr;
        }

        // Create POD command using worker's allocator
        PacketMetadata initialMetadata;
        CommandPacket* packet = ctx.Allocator->AllocatePacketWithCommand<DrawMeshCommand>(initialMetadata);
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: Failed to allocate command packet!");
            return nullptr;
        }

        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = mesh->GetIndexCount();
        cmd->transform = glm::mat4(modelMatrix);
        cmd->shaderHandle = shaderToUse->GetHandle();
        cmd->shaderRendererID = shaderToUse->GetRendererID();

        // Legacy material properties (POD)
        cmd->ambient = material.GetAmbient();
        cmd->diffuse = material.GetDiffuse();
        cmd->specular = material.GetSpecular();
        cmd->shininess = material.GetShininess();
        cmd->useTextureMaps = material.IsUsingTextureMaps();
        cmd->diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRendererID() : 0;
        cmd->specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRendererID() : 0;

        // PBR material properties (POD)
        cmd->enablePBR = (material.GetType() == MaterialType::PBR);
        cmd->baseColorFactor = material.GetBaseColorFactor();
        cmd->emissiveFactor = material.GetEmissiveFactor();
        cmd->metallicFactor = material.GetMetallicFactor();
        cmd->roughnessFactor = material.GetRoughnessFactor();
        cmd->normalScale = material.GetNormalScale();
        cmd->occlusionStrength = material.GetOcclusionStrength();
        cmd->enableIBL = material.IsIBLEnabled();

        // PBR texture renderer IDs (POD)
        cmd->albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRendererID() : 0;
        cmd->metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRendererID() : 0;
        cmd->normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRendererID() : 0;
        cmd->aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRendererID() : 0;
        cmd->emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRendererID() : 0;
        cmd->environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRendererID() : 0;
        cmd->irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRendererID() : 0;
        cmd->prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRendererID() : 0;
        cmd->brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRendererID() : 0;

        // Inlined POD render state
        cmd->renderState = CreatePODRenderStateForMaterial(material);

        // Entity ID for picking
        cmd->entityID = entityID;

        // No bone matrices for non-animated mesh
        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key using parallel context view matrix for depth
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderToUse->GetRendererID() & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);

        // Compute depth using parallel context's view matrix
        glm::vec4 viewPos = ctx.SceneContext->ViewMatrix * modelMatrix[3];
        f32 depth = -viewPos.z;
        constexpr f32 MIN_DEPTH = 0.1f;
        constexpr f32 MAX_DEPTH = 1000.0f;
        depth = glm::clamp(depth, MIN_DEPTH, MAX_DEPTH);
        f32 normalizedDepth = (depth - MIN_DEPTH) / (MAX_DEPTH - MIN_DEPTH);
        u32 depthKey = static_cast<u32>(normalizedDepth * 0xFFFFFF);

        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                        const Ref<Mesh>& mesh,
                                                        const glm::mat4& modelMatrix,
                                                        const Material& material,
                                                        const std::vector<glm::mat4>& boneMatrices,
                                                        bool isStatic)
    {
        OLO_PROFILE_FUNCTION();

        if (!ctx.Allocator || !ctx.SceneContext)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Invalid worker context!");
            return nullptr;
        }

        // For animated meshes, be more conservative with frustum culling
        if (ctx.SceneContext->FrustumCullingEnabled &&
            (isStatic || ctx.SceneContext->DynamicCullingEnabled))
        {
            if (mesh && mesh->GetMeshSource())
            {
                BoundingSphere animatedSphere = mesh->GetTransformedBoundingSphere(modelMatrix);
                animatedSphere.Radius *= 2.0f;

                if (!ctx.SceneContext->ViewFrustum.IsBoundingSphereVisible(animatedSphere))
                {
                    ctx.MeshesCulled++;
                    return nullptr;
                }
            }
        }

        if (!mesh || !mesh->GetMeshSource())
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Invalid mesh or mesh source!");
            return nullptr;
        }

        // Select skinned shader from parallel context
        Ref<Shader> shaderToUse;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            shaderToUse = ctx.SceneContext->PBRSkinnedShader;
        }
        else
        {
            shaderToUse = ctx.SceneContext->SkinnedLightingShader;
        }

        if (!shaderToUse)
        {
            shaderToUse = ctx.SceneContext->LightingShader;
        }

        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: No shader available!");
            return nullptr;
        }

        // Allocate bone matrices in worker's scratch buffer
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 boneCount = static_cast<u32>(boneMatrices.size());

        // Use parallel allocation API
        u32 localBoneOffset = frameBuffer.AllocateBoneMatricesParallel(ctx.WorkerIndex, boneCount);
        if (localBoneOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Failed to allocate bone buffer space");
            return nullptr;
        }
        frameBuffer.WriteBoneMatricesParallel(ctx.WorkerIndex, localBoneOffset, boneMatrices.data(), boneCount);

        // Create POD command using worker's allocator
        PacketMetadata initialMetadata;
        CommandPacket* packet = ctx.Allocator->AllocatePacketWithCommand<DrawMeshCommand>(initialMetadata);
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Failed to allocate command packet!");
            return nullptr;
        }

        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = mesh->GetIndexCount();
        cmd->transform = modelMatrix;
        cmd->shaderHandle = shaderToUse->GetHandle();
        cmd->shaderRendererID = shaderToUse->GetRendererID();

        // Material properties
        cmd->ambient = material.GetAmbient();
        cmd->diffuse = material.GetDiffuse();
        cmd->specular = material.GetSpecular();
        cmd->shininess = material.GetShininess();
        cmd->useTextureMaps = material.IsUsingTextureMaps();
        cmd->diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRendererID() : 0;
        cmd->specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRendererID() : 0;

        cmd->enablePBR = (material.GetType() == MaterialType::PBR);
        cmd->baseColorFactor = material.GetBaseColorFactor();
        cmd->emissiveFactor = material.GetEmissiveFactor();
        cmd->metallicFactor = material.GetMetallicFactor();
        cmd->roughnessFactor = material.GetRoughnessFactor();
        cmd->normalScale = material.GetNormalScale();
        cmd->occlusionStrength = material.GetOcclusionStrength();
        cmd->enableIBL = material.IsIBLEnabled();

        cmd->albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRendererID() : 0;
        cmd->metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRendererID() : 0;
        cmd->normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRendererID() : 0;
        cmd->aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRendererID() : 0;
        cmd->emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRendererID() : 0;
        cmd->environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRendererID() : 0;
        cmd->irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRendererID() : 0;
        cmd->prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRendererID() : 0;
        cmd->brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRendererID() : 0;

        cmd->renderState = CreatePODRenderStateForMaterial(material);

        // Animation support - store worker-local offset with remapping info
        // The offset will be remapped to global in EndParallelSubmission()
        cmd->isAnimatedMesh = true;
        cmd->boneBufferOffset = localBoneOffset;
        cmd->boneCount = boneCount;
        cmd->workerIndex = static_cast<u8>(ctx.WorkerIndex);
        cmd->needsBoneOffsetRemap = true;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderToUse->GetRendererID() & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);

        glm::vec4 viewPos = ctx.SceneContext->ViewMatrix * modelMatrix[3];
        f32 depth = -viewPos.z;
        constexpr f32 MIN_DEPTH = 0.1f;
        constexpr f32 MAX_DEPTH = 1000.0f;
        depth = glm::clamp(depth, MIN_DEPTH, MAX_DEPTH);
        f32 normalizedDepth = (depth - MIN_DEPTH) / (MAX_DEPTH - MIN_DEPTH);
        u32 depthKey = static_cast<u32>(normalizedDepth * 0xFFFFFF);

        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        return packet;
    }

    // ========================================================================
    // High-Level Parallel Submission Helper
    // ========================================================================

    u32 Renderer3D::SubmitMeshesParallel(const std::vector<MeshSubmitDesc>& meshes,
                                         i32 minBatchSize)
    {
        OLO_PROFILE_FUNCTION();

        if (meshes.empty())
        {
            return 0;
        }

        const i32 numMeshes = static_cast<i32>(meshes.size());

        // For small batches, use single-threaded path
        if (numMeshes < minBatchSize * 2)
        {
            u32 totalSubmitted = 0;
            for (const auto& desc : meshes)
            {
                CommandPacket* packet = nullptr;
                if (desc.IsAnimated && desc.BoneMatrices)
                {
                    packet = DrawAnimatedMesh(desc.Mesh, desc.Transform, desc.MaterialData, *desc.BoneMatrices, desc.IsStatic);
                }
                else
                {
                    packet = DrawMesh(desc.Mesh, desc.Transform, desc.MaterialData, desc.IsStatic, desc.EntityID);
                }
                if (packet)
                {
                    SubmitPacket(packet);
                    totalSubmitted++;
                }
            }
            return totalSubmitted;
        }

        // Parallel path using ParallelForWithTaskContext
        BeginParallelSubmission();

        // Per-worker accumulator to track statistics
        struct WorkerStats
        {
            WorkerSubmitContext Context;
            u32 Submitted = 0;
            u32 Culled = 0;
        };

        TArray<WorkerStats> workerStats;

        ParallelForWithTaskContext(
            "SubmitMeshesParallel",
            workerStats,
            numMeshes,
            minBatchSize,
            // Context constructor - initialize worker context for each task slot
            // Use explicit contextIndex to avoid std::thread::id lookup
            [](i32 contextIndex, i32 /*numContexts*/) -> WorkerStats
            {
                WorkerStats stats;
                // Use the optimized path with explicit worker index
                stats.Context = Renderer3D::GetWorkerContext(static_cast<u32>(contextIndex));
                return stats;
            },
            // Body - process one mesh descriptor
            [&meshes](WorkerStats& stats, i32 index)
            {
                const MeshSubmitDesc& desc = meshes[index];

                CommandPacket* packet = nullptr;
                if (desc.IsAnimated && desc.BoneMatrices)
                {
                    packet = Renderer3D::DrawAnimatedMeshParallel(
                        stats.Context,
                        desc.Mesh,
                        desc.Transform,
                        desc.MaterialData,
                        *desc.BoneMatrices,
                        desc.IsStatic);
                }
                else
                {
                    packet = Renderer3D::DrawMeshParallel(
                        stats.Context,
                        desc.Mesh,
                        desc.Transform,
                        desc.MaterialData,
                        desc.IsStatic,
                        desc.EntityID);
                }

                if (packet)
                {
                    Renderer3D::SubmitPacketParallel(stats.Context, packet);
                    stats.Submitted++;
                }
                else
                {
                    stats.Culled++;
                }
            },
            EParallelForFlags::None);

        EndParallelSubmission();

        // Aggregate statistics
        u32 totalSubmitted = 0;
        for (i32 i = 0; i < workerStats.Num(); ++i)
        {
            totalSubmitted += workerStats[i].Submitted;
        }

        return totalSubmitted;
    }

    void Renderer3D::SetLight(const Light& light)
    {
        s_Data.SceneLight = light;
    }

    void Renderer3D::SetViewPosition(const glm::vec3& position)
    {
        s_Data.ViewPos = position;
    }

    void Renderer3D::SetCameraClipPlanes(f32 nearClip, f32 farClip)
    {
        s_Data.CameraNearClip = nearClip;
        s_Data.CameraFarClip = farClip;
    }

    void Renderer3D::UploadMultiLightUBO(const UBOStructures::MultiLightUBO& data)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.MultiLightBuffer)
        {
            s_Data.MultiLightBuffer->SetData(&data, UBOStructures::MultiLightUBO::GetSize());
        }
    }

    void Renderer3D::SetSceneLights(const Ref<Scene>& scene)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !s_Data.MultiLightBuffer)
        {
            return;
        }

        // Collect lights from the scene
        constexpr u32 MAX_POINT_LIGHTS = 16;
        constexpr u32 MAX_SPOT_LIGHTS = 8;

        u32 pointLightCount = 0;
        u32 spotLightCount = 0;

        // TODO: Create proper multi-light UBO structure and populate it
        // For now, we'll just gather the count and warn if limits exceeded

        // Count directional lights
        auto dirLightView = scene->GetAllEntitiesWith<DirectionalLightComponent>();
        u32 dirLightCount = 0;
        for ([[maybe_unused]] auto entity : dirLightView)
        {
            dirLightCount++;
        }

        // Count and collect point lights
        auto pointLightView = scene->GetAllEntitiesWith<PointLightComponent>();
        for ([[maybe_unused]] auto entity : pointLightView)
        {
            pointLightCount++;
        }

        // Count and collect spot lights
        auto spotLightView = scene->GetAllEntitiesWith<SpotLightComponent>();
        for ([[maybe_unused]] auto entity : spotLightView)
        {
            spotLightCount++;
        }

        // Warn if limits exceeded
        if (pointLightCount > MAX_POINT_LIGHTS)
        {
            OLO_CORE_WARN("Scene contains {} point lights, but max is {}. Only first {} will be rendered.",
                          pointLightCount, MAX_POINT_LIGHTS, MAX_POINT_LIGHTS);
        }

        if (spotLightCount > MAX_SPOT_LIGHTS)
        {
            OLO_CORE_WARN("Scene contains {} spot lights, but max is {}. Only first {} will be rendered.",
                          spotLightCount, MAX_SPOT_LIGHTS, MAX_SPOT_LIGHTS);
        }

        // TODO: Populate multi-light UBO with actual light data
        // This requires matching the shader's light structure
    }

    void Renderer3D::EnableFrustumCulling(bool enable)
    {
        s_Data.FrustumCullingEnabled = enable;
    }

    bool Renderer3D::IsFrustumCullingEnabled()
    {
        if (s_ForceDisableCulling)
            return false;
        return s_Data.FrustumCullingEnabled;
    }

    void Renderer3D::EnableDynamicCulling(bool enable)
    {
        s_Data.DynamicCullingEnabled = enable;
    }

    bool Renderer3D::IsDynamicCullingEnabled()
    {
        if (s_ForceDisableCulling)
            return false;
        return s_Data.DynamicCullingEnabled;
    }

    const Frustum& Renderer3D::GetViewFrustum()
    {
        return s_Data.ViewFrustum;
    }

    void Renderer3D::SetForceDisableCulling(bool disable)
    {
        s_ForceDisableCulling = disable;
        if (disable)
        {
            EnableFrustumCulling(false);
            EnableDynamicCulling(false);
            OLO_CORE_WARN("Renderer3D: All culling forcibly disabled for debugging!");
        }
    }

    bool Renderer3D::IsForceDisableCulling()
    {
        return s_ForceDisableCulling;
    }

    Renderer3D::Statistics Renderer3D::GetStats()
    {
        return s_Data.Stats;
    }

    void Renderer3D::ResetStats()
    {
        s_Data.Stats.Reset();
    }

    bool Renderer3D::IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform)
    {
        if (!s_Data.FrustumCullingEnabled)
            return true;

        BoundingSphere sphere = mesh->GetTransformedBoundingSphere(transform);
        sphere.Radius *= 1.3f;

        return s_Data.ViewFrustum.IsBoundingSphereVisible(sphere);
    }

    bool Renderer3D::IsVisibleInFrustum(const BoundingSphere& sphere)
    {
        if (!s_Data.FrustumCullingEnabled)
            return true;

        BoundingSphere expandedSphere = sphere;
        expandedSphere.Radius *= 1.3f;

        return s_Data.ViewFrustum.IsBoundingSphereVisible(expandedSphere);
    }

    bool Renderer3D::IsVisibleInFrustum(const BoundingBox& box)
    {
        if (!s_Data.FrustumCullingEnabled)
            return true;

        return s_Data.ViewFrustum.IsBoundingBoxVisible(box);
    }

    CommandPacket* Renderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic, i32 entityID)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: ScenePass is null!");
            return nullptr;
        }
        s_Data.Stats.TotalMeshes++;

        if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
        {
            if (!IsVisibleInFrustum(mesh, modelMatrix))
            {
                s_Data.Stats.CulledMeshes++;
                return nullptr;
            }
        }
        if (!mesh || !mesh->GetVertexArray())
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: Invalid mesh or vertex array!");
            return nullptr;
        }

        Ref<Shader> shaderToUse;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            shaderToUse = s_Data.PBRShader;
        }
        else
        {
            shaderToUse = s_Data.LightingShader;
        }

        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: No shader available!");
            return nullptr;
        }

        // Create POD command using asset handles and renderer IDs
        CommandPacket* packet = CreateDrawCall<DrawMeshCommand>();
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = mesh->GetIndexCount();
        cmd->transform = glm::mat4(modelMatrix);
        cmd->entityID = entityID;
        cmd->shaderHandle = shaderToUse->GetHandle();
        cmd->shaderRendererID = shaderToUse->GetRendererID();

        // Legacy material properties (POD)
        cmd->ambient = material.GetAmbient();
        cmd->diffuse = material.GetDiffuse();
        cmd->specular = material.GetSpecular();
        cmd->shininess = material.GetShininess();
        cmd->useTextureMaps = material.IsUsingTextureMaps();
        cmd->diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRendererID() : 0;
        cmd->specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRendererID() : 0;

        // PBR material properties (POD)
        cmd->enablePBR = (material.GetType() == MaterialType::PBR);
        cmd->baseColorFactor = material.GetBaseColorFactor();
        cmd->emissiveFactor = material.GetEmissiveFactor();
        cmd->metallicFactor = material.GetMetallicFactor();
        cmd->roughnessFactor = material.GetRoughnessFactor();
        cmd->normalScale = material.GetNormalScale();
        cmd->occlusionStrength = material.GetOcclusionStrength();
        cmd->enableIBL = material.IsIBLEnabled();

        // PBR texture renderer IDs (POD)
        cmd->albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRendererID() : 0;
        cmd->metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRendererID() : 0;
        cmd->normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRendererID() : 0;
        cmd->aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRendererID() : 0;
        cmd->emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRendererID() : 0;
        cmd->environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRendererID() : 0;
        cmd->irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRendererID() : 0;
        cmd->prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRendererID() : 0;
        cmd->brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRendererID() : 0;

        // Inlined POD render state
        cmd->renderState = CreatePODRenderStateForMaterial(material);

        // No bone matrices for non-animated mesh
        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for optimal command sorting
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderToUse->GetRendererID() & 0xFFFF; // 16-bit shader ID
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: ScenePass is null!");
            return nullptr;
        }
        if (!texture)
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: No texture provided!");
            return nullptr;
        }
        if (!s_Data.QuadShader)
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: Quad shader is not loaded!");
            return nullptr;
        }
        if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: Quad mesh or its vertex array is invalid!");
            s_Data.QuadMesh = MeshPrimitives::CreatePlane(1.0f, 1.0f);
            if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
                return nullptr;
        }

        // Create POD command
        CommandPacket* packet = CreateDrawCall<DrawQuadCommand>();
        auto* cmd = packet->GetCommandData<DrawQuadCommand>();
        cmd->header.type = CommandType::DrawQuad;
        cmd->transform = glm::mat4(modelMatrix);
        cmd->textureID = texture->GetRendererID();
        cmd->shaderHandle = s_Data.QuadShader->GetHandle();
        cmd->shaderRendererID = s_Data.QuadShader->GetRendererID();
        cmd->quadVAID = s_Data.QuadMesh->GetVertexArray()->GetRendererID();
        cmd->renderState = CreateDefaultPODRenderState();

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for quad commands
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = s_Data.QuadShader->GetRendererID() & 0xFFFF;
        u32 materialID = texture ? (texture->GetRendererID() & 0xFFFF) : 0;
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::TwoD, shaderID, materialID, depth);
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: ScenePass is null!");
            return nullptr;
        }
        if (transforms.empty())
        {
            OLO_CORE_WARN("Renderer3D::DrawMeshInstanced: No transforms provided");
            return nullptr;
        }
        s_Data.Stats.TotalMeshes += static_cast<u32>(transforms.size());

        if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
        {
            if (!IsVisibleInFrustum(mesh, transforms[0]))
            {
                s_Data.Stats.CulledMeshes += static_cast<u32>(transforms.size());
                return nullptr;
            }
        }

        // Allocate space in FrameDataBuffer for instance transforms
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 transformCount = static_cast<u32>(transforms.size());
        u32 transformOffset = frameBuffer.AllocateTransforms(transformCount);
        if (transformOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: Failed to allocate transform buffer space");
            return nullptr;
        }
        frameBuffer.WriteTransforms(transformOffset, transforms.data(), transformCount);

        Ref<Shader> shaderToUse = material.GetShader() ? material.GetShader() : s_Data.LightingShader;

        // Create POD command
        CommandPacket* packet = CreateDrawCall<DrawMeshInstancedCommand>();
        auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();
        cmd->header.type = CommandType::DrawMeshInstanced;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = mesh->GetIndexCount();
        cmd->instanceCount = transformCount;
        cmd->transformBufferOffset = transformOffset;
        cmd->transformCount = transformCount;
        cmd->shaderHandle = shaderToUse->GetHandle();
        cmd->shaderRendererID = shaderToUse->GetRendererID();

        // Legacy material properties (POD)
        cmd->ambient = material.GetAmbient();
        cmd->diffuse = material.GetDiffuse();
        cmd->specular = material.GetSpecular();
        cmd->shininess = material.GetShininess();
        cmd->useTextureMaps = material.IsUsingTextureMaps();
        cmd->diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRendererID() : 0;
        cmd->specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRendererID() : 0;

        // PBR material properties (POD)
        cmd->enablePBR = (material.GetType() == MaterialType::PBR);
        cmd->baseColorFactor = material.GetBaseColorFactor();
        cmd->emissiveFactor = material.GetEmissiveFactor();
        cmd->metallicFactor = material.GetMetallicFactor();
        cmd->roughnessFactor = material.GetRoughnessFactor();
        cmd->normalScale = material.GetNormalScale();
        cmd->occlusionStrength = material.GetOcclusionStrength();
        cmd->enableIBL = material.IsIBLEnabled();

        // PBR texture renderer IDs (POD)
        cmd->albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRendererID() : 0;
        cmd->metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRendererID() : 0;
        cmd->normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRendererID() : 0;
        cmd->aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRendererID() : 0;
        cmd->emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRendererID() : 0;
        cmd->environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRendererID() : 0;
        cmd->irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRendererID() : 0;
        cmd->prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRendererID() : 0;
        cmd->brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRendererID() : 0;

        // Inlined POD render state
        cmd->renderState = CreatePODRenderStateForMaterial(material);

        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCountPerInstance = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for instanced mesh commands (use first transform for depth)
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderToUse->GetRendererID() & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = transforms.empty() ? 0 : ComputeDepthForSortKey(transforms[0]);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawLightCube(const glm::mat4& modelMatrix)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawLightCube: ScenePass is null!");
            return nullptr;
        }

        // Create POD command
        CommandPacket* packet = CreateDrawCall<DrawMeshCommand>();
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = s_Data.CubeMesh->GetHandle();
        cmd->vertexArrayID = s_Data.CubeMesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = s_Data.CubeMesh->GetIndexCount();
        cmd->transform = modelMatrix;
        cmd->shaderHandle = s_Data.LightCubeShader->GetHandle();
        cmd->shaderRendererID = s_Data.LightCubeShader->GetRendererID();

        // Legacy material properties
        cmd->ambient = glm::vec3(1.0f);
        cmd->diffuse = glm::vec3(1.0f);
        cmd->specular = glm::vec3(1.0f);
        cmd->shininess = 32.0f;
        cmd->useTextureMaps = false;
        cmd->diffuseMapID = 0;
        cmd->specularMapID = 0;

        // PBR material properties (default values for light cube)
        cmd->enablePBR = false;
        cmd->baseColorFactor = glm::vec4(1.0f);
        cmd->emissiveFactor = glm::vec4(0.0f);
        cmd->metallicFactor = 0.0f;
        cmd->roughnessFactor = 1.0f;
        cmd->normalScale = 1.0f;
        cmd->occlusionStrength = 1.0f;
        cmd->enableIBL = false;
        cmd->albedoMapID = 0;
        cmd->metallicRoughnessMapID = 0;
        cmd->normalMapID = 0;
        cmd->aoMapID = 0;
        cmd->emissiveMapID = 0;
        cmd->environmentMapID = 0;
        cmd->irradianceMapID = 0;
        cmd->prefilterMapID = 0;
        cmd->brdfLutMapID = 0;

        // Inlined POD render state
        cmd->renderState = CreateDefaultPODRenderState();

        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for light cube
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = s_Data.LightCubeShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic)
    {
        return DrawMesh(s_Data.CubeMesh, modelMatrix, material, isStatic);
    }

    void Renderer3D::UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection)
    {
        OLO_PROFILE_FUNCTION();

        ShaderBindingLayout::CameraUBO cameraData;
        cameraData.ViewProjection = projection * view;
        cameraData.View = view;
        cameraData.Projection = projection;
        cameraData.Position = s_Data.ViewPos;
        cameraData._padding0 = 0.0f;

        constexpr u32 expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
        static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch");

        s_Data.CameraUBO->SetData(&cameraData, expectedSize);
    }

    void Renderer3D::UpdateLightPropertiesUBO()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.LightPropertiesUBO)
        {
            ShaderBindingLayout::LightUBO lightData;
            auto lightType = std::to_underlying(s_Data.SceneLight.Type);

            lightData.LightPosition = glm::vec4(s_Data.SceneLight.Position, 1.0f);
            lightData.LightDirection = glm::vec4(s_Data.SceneLight.Direction, 0.0f);
            lightData.LightAmbient = glm::vec4(s_Data.SceneLight.Ambient, 0.0f);
            lightData.LightDiffuse = glm::vec4(s_Data.SceneLight.Diffuse, 0.0f);
            lightData.LightSpecular = glm::vec4(s_Data.SceneLight.Specular, 0.0f);
            lightData.LightAttParams = glm::vec4(
                s_Data.SceneLight.Constant,
                s_Data.SceneLight.Linear,
                s_Data.SceneLight.Quadratic,
                0.0f);
            lightData.LightSpotParams = glm::vec4(
                s_Data.SceneLight.CutOff,
                s_Data.SceneLight.OuterCutOff,
                0.0f,
                0.0f);
            lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(lightType));

            s_Data.LightPropertiesUBO->SetData(&lightData, sizeof(ShaderBindingLayout::LightUBO));
        }
    }

    void Renderer3D::SetupRenderGraph(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Setting up Renderer3D RenderGraph with dimensions: {}x{}", width, height);

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("Invalid dimensions for RenderGraph: {}x{}", width, height);
            return;
        }

        s_Data.RGraph->Init(width, height);

        // Shadow pass (renders before scene, doesn't need scene framebuffer dimensions)
        FramebufferSpecification shadowPassSpec;
        shadowPassSpec.Width = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);
        shadowPassSpec.Height = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);

        s_Data.ShadowPass = Ref<ShadowRenderPass>::Create();
        s_Data.ShadowPass->SetName("ShadowPass");
        s_Data.ShadowPass->Init(shadowPassSpec);
        s_Data.ShadowPass->SetShadowMap(&s_Data.Shadow);

        FramebufferSpecification scenePassSpec;
        scenePassSpec.Width = width;
        scenePassSpec.Height = height;
        scenePassSpec.Samples = 1;
        scenePassSpec.Attachments = {
            FramebufferTextureFormat::RGBA16F,     // [0] HDR color output
            FramebufferTextureFormat::RED_INTEGER, // [1] Entity ID attachment
            FramebufferTextureFormat::RG16F,       // [2] View-space normals (octahedral encoded for SSAO)
            FramebufferTextureFormat::Depth
        };

        FramebufferSpecification finalPassSpec;
        finalPassSpec.Width = width;
        finalPassSpec.Height = height;

        s_Data.ScenePass = Ref<SceneRenderPass>::Create();
        s_Data.ScenePass->SetName("ScenePass");
        s_Data.ScenePass->Init(scenePassSpec);

        s_Data.ParticlePass = Ref<ParticleRenderPass>::Create();
        s_Data.ParticlePass->SetName("ParticlePass");
        s_Data.ParticlePass->Init(finalPassSpec);
        s_Data.ParticlePass->SetSceneFramebuffer(s_Data.ScenePass->GetTarget());

        s_Data.SSAOPass = Ref<SSAORenderPass>::Create();
        s_Data.SSAOPass->SetName("SSAOPass");
        s_Data.SSAOPass->Init(scenePassSpec);
        s_Data.SSAOPass->SetSceneFramebuffer(s_Data.ScenePass->GetTarget());
        s_Data.SSAOPass->SetSSAOUBO(s_Data.SSAOUBO, &s_Data.SSAOGPUData);

        s_Data.SSSPass = Ref<SSSRenderPass>::Create();
        s_Data.SSSPass->SetName("SSSPass");
        s_Data.SSSPass->Init(finalPassSpec);
        s_Data.SSSPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
        s_Data.SSSPass->SetSSSUBO(s_Data.SSSUBO, &s_Data.SSSGPUData);

        s_Data.PostProcessPass = Ref<PostProcessRenderPass>::Create();
        s_Data.PostProcessPass->SetName("PostProcessPass");
        s_Data.PostProcessPass->Init(finalPassSpec);
        s_Data.PostProcessPass->SetSceneDepthFramebuffer(s_Data.ScenePass->GetTarget());

        s_Data.FinalPass = Ref<FinalRenderPass>::Create();
        s_Data.FinalPass->SetName("FinalPass");
        s_Data.FinalPass->Init(finalPassSpec);

        s_Data.RGraph->AddPass(s_Data.ShadowPass);
        s_Data.RGraph->AddPass(s_Data.ScenePass);
        s_Data.RGraph->AddPass(s_Data.SSAOPass);
        s_Data.RGraph->AddPass(s_Data.ParticlePass);
        s_Data.RGraph->AddPass(s_Data.SSSPass);
        s_Data.RGraph->AddPass(s_Data.PostProcessPass);
        s_Data.RGraph->AddPass(s_Data.FinalPass);

        // ShadowPass -> ScenePass: ordering only (shadow textures are bound via UBO/texture slots)
        s_Data.RGraph->AddExecutionDependency("ShadowPass", "ScenePass");
        // ScenePass -> SSAOPass: ordering only (SSAO reads scene depth/normals via texture slots)
        s_Data.RGraph->AddExecutionDependency("ScenePass", "SSAOPass");
        // SSAOPass -> ParticlePass: ordering (SSAO must complete before particles render into scene FB)
        s_Data.RGraph->AddExecutionDependency("SSAOPass", "ParticlePass");
        // ParticlePass -> SSSPass: framebuffer piping (SSS reads scene color via SetInputFramebuffer)
        s_Data.RGraph->ConnectPass("ParticlePass", "SSSPass");
        // Graph connections: SSSPass -> PostProcessPass -> FinalPass use SetInputFramebuffer
        // via the graph's Execute() piping. No manual calls needed for these.
        s_Data.RGraph->ConnectPass("SSSPass", "PostProcessPass");
        s_Data.RGraph->ConnectPass("PostProcessPass", "FinalPass");

        // PostProcessPass needs the scene depth for DOF/MotionBlur (not piped by graph).
        s_Data.PostProcessPass->SetSceneDepthFramebuffer(s_Data.ScenePass->GetTarget());
        // PostProcessPass initial input is the scene FB (overridden by graph's SSSPass -> PostProcessPass
        // piping each frame, which passes SSSPass::GetTarget() = scene FB when SSS is disabled).
        s_Data.PostProcessPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
        OLO_CORE_INFO("Renderer3D: Render graph: ShadowPass -> ScenePass -> SSAOPass -> ParticlePass -> SSSPass -> PostProcessPass -> FinalPass");

        s_Data.RGraph->SetFinalPass("FinalPass");
    }

    void Renderer3D::OnWindowResize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Renderer3D::OnWindowResize: Resizing to {}x{}", width, height);

        if (s_Data.RGraph)
        {
            s_Data.RGraph->Resize(width, height);
        }
        else
        {
            OLO_CORE_WARN("Renderer3D::OnWindowResize: No render graph available!");
        }
    }

    CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic, i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: ScenePass is null!");
            return nullptr;
        }

        s_Data.Stats.TotalMeshes++;

        // For animated meshes, be more conservative with frustum culling
        // since bone transforms can move vertices significantly beyond rest pose bounds
        if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
        {
            // For animated draws, expand the bounding sphere more aggressively to account for skinning deformation
            if (!mesh || !mesh->GetMeshSource())
            {
                OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Invalid mesh or mesh source for frustum culling!");
                return nullptr;
            }

            BoundingSphere animatedSphere = mesh->GetTransformedBoundingSphere(modelMatrix);
            // Use a larger expansion factor for animated meshes to account for potential deformation
            animatedSphere.Radius *= 2.0f; // More conservative than the standard 1.3f for static meshes

            if (!s_Data.ViewFrustum.IsBoundingSphereVisible(animatedSphere))
            {
                s_Data.Stats.CulledMeshes++;
                return nullptr;
            }
        }

        if (!mesh || !mesh->GetMeshSource())
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Invalid mesh or mesh source!");
            return nullptr;
        }

        auto meshSource = mesh->GetMeshSource();

        // Validate that the mesh supports skinning
        OLO_CORE_ASSERT(meshSource->HasSkeleton(), "Animated mesh must have a skeleton!");
        OLO_CORE_ASSERT(!boneMatrices.empty(), "Bone matrices cannot be empty for animated mesh!");

        const auto* skeleton = meshSource->GetSkeleton();
        OLO_CORE_ASSERT(skeleton, "Mesh skeleton cannot be null!");

        // Validate bone matrix count matches skeleton bone count
        if (boneMatrices.size() != skeleton->m_BoneNames.size())
        {
            OLO_CORE_ERROR("Bone matrices count ({}) must match skeleton bone count ({})",
                           boneMatrices.size(), skeleton->m_BoneNames.size());
            OLO_CORE_ASSERT(false, "Bone matrix count mismatch!");
        }

        static bool s_FirstRun = true;
        if (s_FirstRun)
        {
            OLO_CORE_INFO("Renderer3D::DrawAnimatedMesh: First animated mesh with {} bone influences", meshSource->GetBoneInfluences().Num());
            s_FirstRun = false;
        }

        if (!meshSource->HasBoneInfluences())
        {
            OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: Mesh has no bone influences (size: {}), falling back to regular mesh rendering",
                          meshSource->GetBoneInfluences().Num());
            return DrawMesh(mesh, modelMatrix, material, isStatic);
        }

        Ref<Shader> shaderToUse;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            shaderToUse = s_Data.PBRSkinnedShader;
        }
        else
        {
            shaderToUse = s_Data.SkinnedLightingShader;
        }

        if (!shaderToUse)
        {
            OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: Preferred shader not available, falling back to Lighting3D");
            shaderToUse = s_Data.LightingShader;
        }
        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: No shader available!");
            return nullptr;
        }

        if (boneMatrices.empty())
        {
            OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: No bone matrices provided, using identity matrices");
        }

        // Check if VAO is valid before proceeding
        auto vertexArray = mesh->GetVertexArray();
        if (!vertexArray)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Mesh has null VAO (Vertex Array Object)!");
            return nullptr;
        }

        // Allocate space in FrameDataBuffer for bone matrices
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 boneCount = static_cast<u32>(boneMatrices.size());
        u32 boneBufferOffset = frameBuffer.AllocateBoneMatrices(boneCount);
        if (boneBufferOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Failed to allocate bone buffer space");
            return nullptr;
        }
        frameBuffer.WriteBoneMatrices(boneBufferOffset, boneMatrices.data(), boneCount);

        // Create POD command
        CommandPacket* packet = CreateDrawCall<DrawMeshCommand>();
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = vertexArray->GetRendererID();
        cmd->indexCount = mesh->GetIndexCount();
        cmd->transform = modelMatrix;
        cmd->shaderHandle = shaderToUse->GetHandle();
        cmd->shaderRendererID = shaderToUse->GetRendererID();

        // Legacy material properties (POD)
        cmd->ambient = material.GetAmbient();
        cmd->diffuse = material.GetDiffuse();
        cmd->specular = material.GetSpecular();
        cmd->shininess = material.GetShininess();
        cmd->useTextureMaps = material.IsUsingTextureMaps();
        cmd->diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRendererID() : 0;
        cmd->specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRendererID() : 0;

        // PBR material properties (POD)
        cmd->enablePBR = (material.GetType() == MaterialType::PBR);
        cmd->baseColorFactor = material.GetBaseColorFactor();
        cmd->emissiveFactor = material.GetEmissiveFactor();
        cmd->metallicFactor = material.GetMetallicFactor();
        cmd->roughnessFactor = material.GetRoughnessFactor();
        cmd->normalScale = material.GetNormalScale();
        cmd->occlusionStrength = material.GetOcclusionStrength();
        cmd->enableIBL = material.IsIBLEnabled();

        // PBR texture renderer IDs (POD)
        cmd->albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRendererID() : 0;
        cmd->metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRendererID() : 0;
        cmd->normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRendererID() : 0;
        cmd->aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRendererID() : 0;
        cmd->emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRendererID() : 0;
        cmd->environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRendererID() : 0;
        cmd->irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRendererID() : 0;
        cmd->prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRendererID() : 0;
        cmd->brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRendererID() : 0;

        // Inlined POD render state
        cmd->renderState = CreatePODRenderStateForMaterial(material);

        // Animation support - store offset/count into FrameDataBuffer
        cmd->isAnimatedMesh = true;
        cmd->boneBufferOffset = boneBufferOffset;
        cmd->boneCount = boneCount;

        // Entity ID for picking
        cmd->entityID = entityID;

        static bool s_LoggedBoneMatrices = false;
        if (!s_LoggedBoneMatrices && !boneMatrices.empty())
        {
            OLO_CORE_INFO("DrawAnimatedMesh: Storing {} bone matrices at offset {} in FrameDataBuffer", boneCount, boneBufferOffset);
            s_LoggedBoneMatrices = true;
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for animated mesh commands
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderToUse->GetRendererID() & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        return packet;
    }

    void Renderer3D::ApplyGlobalResources()
    {
        OLO_PROFILE_FUNCTION();

        const auto& shaderRegistries = s_Data.ShaderRegistries;

        for (const auto& [shaderID, registry] : shaderRegistries)
        {
            if (registry)
            {
                const auto& globalResources = s_Data.GlobalResourceRegistry.GetBoundResources();
                for (const auto& [resourceName, resource] : globalResources)
                {
                    if (registry->GetBindingInfo(resourceName) != nullptr)
                    {
                        ShaderResourceInput input;
                        if (std::holds_alternative<Ref<UniformBuffer>>(resource))
                        {
                            input = ShaderResourceInput(std::get<Ref<UniformBuffer>>(resource));
                        }
                        else if (std::holds_alternative<Ref<Texture2D>>(resource))
                        {
                            input = ShaderResourceInput(std::get<Ref<Texture2D>>(resource));
                        }
                        else if (std::holds_alternative<Ref<TextureCubemap>>(resource))
                        {
                            input = ShaderResourceInput(std::get<Ref<TextureCubemap>>(resource));
                        }

                        if (input.Type != ShaderResourceType::None)
                        {
                            registry->SetResource(resourceName, input);
                        }
                    }
                }
            }
        }
    }

    void Renderer3D::RenderAnimatedMeshes(const Ref<Scene>& scene, const Material& defaultMaterial)
    {
        OLO_PROFILE_FUNCTION();

        static bool s_FirstRun = true;
        if (s_FirstRun)
        {
            OLO_CORE_INFO("Renderer3D::RenderAnimatedMeshes: Starting animated mesh rendering");
            s_FirstRun = false;
        }

        if (!scene)
        {
            OLO_CORE_WARN("Renderer3D::RenderAnimatedMeshes: Scene is null");
            return;
        }

        // Get all entities with required components
        auto view = scene->GetAllEntitiesWith<MeshComponent, SkeletonComponent, TransformComponent>();

        // Collect mesh descriptors for parallel submission
        std::vector<MeshSubmitDesc> meshDescriptors;
        meshDescriptors.reserve(32); // Pre-allocate for typical case

        sizet entityCount = 0;
        for (auto entityID : view)
        {
            Entity entity = { entityID, scene.get() };
            s_Data.Stats.TotalAnimatedMeshes++;
            entityCount++;

            // Validate components
            if (!entity.HasComponent<MeshComponent>() ||
                !entity.HasComponent<SkeletonComponent>() ||
                !entity.HasComponent<TransformComponent>())
            {
                s_Data.Stats.SkippedAnimatedMeshes++;
                continue;
            }

            auto& meshComp = entity.GetComponent<MeshComponent>();
            auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
            auto& transformComp = entity.GetComponent<TransformComponent>();

            if (!meshComp.m_MeshSource || !skeletonComp.m_Skeleton)
            {
                s_Data.Stats.SkippedAnimatedMeshes++;
                continue;
            }

            glm::mat4 worldTransform = transformComp.GetTransform();
            const auto& boneMatrices = skeletonComp.m_Skeleton->m_FinalBoneMatrices;

            // Get material from entity or use default
            Material material = defaultMaterial;
            if (entity.HasComponent<MaterialComponent>())
            {
                material = entity.GetComponent<MaterialComponent>().m_Material;
            }

            // Check for RelationshipComponent to find child submeshes
            bool foundSubmeshes = false;
            if (entity.HasComponent<RelationshipComponent>())
            {
                const auto& relationshipComponent = entity.GetComponent<RelationshipComponent>();
                for (const UUID& childUUID : relationshipComponent.m_Children)
                {
                    auto submeshEntityOpt = scene->TryGetEntityWithUUID(childUUID);
                    if (submeshEntityOpt && submeshEntityOpt->HasComponent<SubmeshComponent>())
                    {
                        auto& submeshComponent = submeshEntityOpt->GetComponent<SubmeshComponent>();
                        if (submeshComponent.m_Mesh && submeshComponent.m_Visible)
                        {
                            // Get submesh material if available
                            Material submeshMaterial = material;
                            if (submeshEntityOpt->HasComponent<MaterialComponent>())
                            {
                                submeshMaterial = submeshEntityOpt->GetComponent<MaterialComponent>().m_Material;
                            }

                            meshDescriptors.push_back({ submeshComponent.m_Mesh,
                                                        worldTransform,
                                                        submeshMaterial,
                                                        false, // IsStatic
                                                        -1,    // EntityID
                                                        true,  // IsAnimated
                                                        &boneMatrices });
                            foundSubmeshes = true;
                        }
                    }
                }
            }

            // Fallback: if no submesh entities found, use first submesh from MeshSource
            if (!foundSubmeshes && meshComp.m_MeshSource->GetSubmeshes().Num() > 0)
            {
                auto mesh = Ref<Mesh>::Create(meshComp.m_MeshSource, 0);
                meshDescriptors.push_back({ mesh,
                                            worldTransform,
                                            material,
                                            false, // IsStatic
                                            -1,    // EntityID
                                            true,  // IsAnimated
                                            &boneMatrices });
            }

            s_Data.Stats.RenderedAnimatedMeshes++;
        }

        // Submit all animated meshes in parallel
        if (!meshDescriptors.empty())
        {
            SubmitMeshesParallel(meshDescriptors);
        }

        // Log stats when count changes
        static sizet s_LastEntityCount = 0;
        if (entityCount != s_LastEntityCount)
        {
            OLO_CORE_INFO("RenderAnimatedMeshes: Found {} animated entities, {} submeshes",
                          entityCount, meshDescriptors.size());
            s_LastEntityCount = entityCount;
        }
    }

    void Renderer3D::RenderAnimatedMesh(const Ref<Scene>& scene, Entity entity, const Material& defaultMaterial)
    {
        OLO_PROFILE_FUNCTION();

        if (!entity.HasComponent<MeshComponent>() ||
            !entity.HasComponent<SkeletonComponent>() ||
            !entity.HasComponent<TransformComponent>())
        {
            s_Data.Stats.SkippedAnimatedMeshes++;
            return;
        }

        auto& meshComp = entity.GetComponent<MeshComponent>();
        auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
        auto& transformComp = entity.GetComponent<TransformComponent>();

        if (!meshComp.m_MeshSource || !skeletonComp.m_Skeleton)
        {
            OLO_CORE_WARN("Renderer3D::RenderAnimatedMesh: Entity {} has invalid mesh or skeleton",
                          entity.GetComponent<TagComponent>().Tag);
            s_Data.Stats.SkippedAnimatedMeshes++;
            return;
        }

        glm::mat4 worldTransform = transformComp.GetTransform();

        // Get bone matrices from the skeleton
        const auto& boneMatrices = skeletonComp.m_Skeleton->m_FinalBoneMatrices;

        // Use MaterialComponent if available, otherwise use default material
        Material material = defaultMaterial;
        if (entity.HasComponent<MaterialComponent>())
        {
            material = entity.GetComponent<MaterialComponent>().m_Material;
        }

        // Find and render all child entities with SubmeshComponent
        bool renderedAnySubmesh = false;

        // Check if entity has RelationshipComponent before accessing it
        if (!entity.HasComponent<RelationshipComponent>())
        {
            OLO_CORE_WARN("DrawAnimatedMesh: Entity does not have RelationshipComponent, cannot render submeshes");
            return;
        }

        const auto& relationshipComponent = entity.GetComponent<RelationshipComponent>();
        for (const UUID& childUUID : relationshipComponent.m_Children)
        {
            auto submeshEntityOpt = scene->TryGetEntityWithUUID(childUUID);
            if (submeshEntityOpt && submeshEntityOpt->HasComponent<SubmeshComponent>())
            {
                auto& submeshComponent = submeshEntityOpt->GetComponent<SubmeshComponent>();
                if (submeshComponent.m_Mesh && submeshComponent.m_Visible)
                {
                    // Use MaterialComponent if available on submesh, otherwise use the parent's material
                    Material submeshMaterial = material;
                    if (submeshEntityOpt->HasComponent<MaterialComponent>())
                    {
                        submeshMaterial = submeshEntityOpt->GetComponent<MaterialComponent>().m_Material;
                    }

                    // Use the new MeshSource with bone influences directly
                    auto* packet = DrawAnimatedMesh(
                        submeshComponent.m_Mesh,
                        worldTransform,
                        submeshMaterial,
                        boneMatrices,
                        false);

                    if (packet)
                    {
                        SubmitPacket(packet);
                        renderedAnySubmesh = true;
                    }
                }
            }
        }

        // Fallback: if no submesh entities found, create a mesh from the first submesh
        if (!renderedAnySubmesh)
        {
            if (meshComp.m_MeshSource->GetSubmeshes().Num() > 0)
            {
                auto mesh = Ref<Mesh>::Create(meshComp.m_MeshSource, 0);

                auto* packet = DrawAnimatedMesh(
                    mesh,
                    worldTransform,
                    material,
                    boneMatrices,
                    false);

                if (packet)
                {
                    SubmitPacket(packet);
                    renderedAnySubmesh = true;
                }
            }
        }

        if (renderedAnySubmesh)
        {
            s_Data.Stats.RenderedAnimatedMeshes++;
        }
    }

    ShaderResourceRegistry* Renderer3D::GetShaderRegistry(u32 shaderID)
    {
        auto it = s_Data.ShaderRegistries.find(shaderID);
        return it != s_Data.ShaderRegistries.end() ? it->second : nullptr;
    }

    void Renderer3D::RegisterShaderRegistry(u32 shaderID, ShaderResourceRegistry* registry)
    {
        if (registry)
        {
            s_Data.ShaderRegistries[shaderID] = registry;
            OLO_CORE_TRACE("Renderer3D: Registered shader registry for shader ID: {0}", shaderID);
        }
    }

    void Renderer3D::UnregisterShaderRegistry(u32 shaderID)
    {
        auto it = s_Data.ShaderRegistries.find(shaderID);
        if (it != s_Data.ShaderRegistries.end())
        {
            s_Data.ShaderRegistries.erase(it);
            OLO_CORE_TRACE("Renderer3D: Unregistered shader registry for shader ID: {0}", shaderID);
        }
    }

    const std::unordered_map<u32, ShaderResourceRegistry*>& Renderer3D::GetShaderRegistries()
    {
        return s_Data.ShaderRegistries;
    }

    void Renderer3D::ApplyResourceBindings(u32 shaderID)
    {
        auto* registry = GetShaderRegistry(shaderID);
        if (registry)
        {
            registry->ApplyBindings();
        }
    }

    ShaderLibrary& Renderer3D::GetShaderLibrary()
    {
        return m_ShaderLibrary;
    }

    CommandPacket* Renderer3D::DrawSkybox(const Ref<TextureCubemap>& skyboxTexture)
    {
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkybox: ScenePass is null!");
            return nullptr;
        }

        if (!skyboxTexture)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkybox: Skybox texture is null!");
            return nullptr;
        }

        if (!s_Data.SkyboxMesh || !s_Data.SkyboxShader)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkybox: Skybox mesh or shader not initialized!");
            return nullptr;
        }

        // Create POD command
        CommandPacket* packet = CreateDrawCall<DrawSkyboxCommand>();
        auto* cmd = packet->GetCommandData<DrawSkyboxCommand>();
        cmd->header.type = CommandType::DrawSkybox;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = s_Data.SkyboxMesh->GetHandle();
        cmd->vertexArrayID = s_Data.SkyboxMesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = s_Data.SkyboxMesh->GetIndexCount();
        cmd->transform = glm::mat4(1.0f); // Identity matrix for skybox
        cmd->shaderHandle = s_Data.SkyboxShader->GetHandle();
        cmd->shaderRendererID = s_Data.SkyboxShader->GetRendererID();
        cmd->skyboxTextureID = skyboxTexture->GetRendererID();

        // Skybox-specific POD render state
        cmd->renderState = CreateDefaultPODRenderState();
        cmd->renderState.depthTestEnabled = true;
        cmd->renderState.depthFunction = GL_LEQUAL; // Important for skybox
        cmd->renderState.depthWriteMask = false;    // Don't write to depth buffer
        cmd->renderState.cullingEnabled = false;    // Don't cull faces for skybox

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for skybox (rendered last in skybox layer with max depth)
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = s_Data.SkyboxShader->GetRendererID() & 0xFFFF;
        // Skybox always renders at maximum depth (far plane)
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::Skybox, shaderID, 0, 0xFFFFFF);
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color, f32 thickness)
    {
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawLine: ScenePass is null!");
            return nullptr;
        }

        // Use a highly emissive material for skeleton visualization
        Material material{};
        material.SetType(MaterialType::PBR);
        material.SetBaseColorFactor(glm::vec4(color, 1.0f));
        material.SetMetallicFactor(0.0f);
        material.SetRoughnessFactor(1.0f);
        material.SetEmissiveFactor(glm::vec4(color * 5.0f, 1.0f)); // Very bright emissive for visibility through surfaces

        if (!s_Data.LineQuadMesh)
        {
            OLO_CORE_WARN("Renderer3D::DrawLine: LineQuadMesh not initialized");
            return nullptr;
        }

        // Build transform: translate to start, rotate to align +X with (end-start), scale X to length and Y to thickness
        const glm::vec3 seg = end - start;
        const float length = glm::length(seg);
        if (length <= 0.0001f)
            return nullptr;

        // Convert UI thickness to world thickness
        const float worldThickness = thickness * 0.005f; // matches previous visual scale

        // Compute rotation from +X axis to segment direction
        const glm::vec3 dir = seg / length;
        const glm::vec3 xAxis = glm::vec3(1, 0, 0);
        glm::mat4 rot(1.0f);
        const float dot = glm::clamp(glm::dot(xAxis, dir), -1.0f, 1.0f);
        if (dot < 0.9999f)
        {
            if (dot < -0.9999f) // Direction is opposite to +X axis (antiparallel)
            {
                // Use Y axis for 180-degree rotation to avoid zero cross product
                rot = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0, 1, 0));
            }
            else
            {
                glm::vec3 axis = glm::normalize(glm::cross(xAxis, dir));
                float angle = acosf(dot);
                rot = glm::rotate(glm::mat4(1.0f), angle, axis);
            }
        }
        // Scale: X=length, Y=worldThickness, Z=1
        glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(length, worldThickness, 1.0f));
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), start) * rot * scale;

        auto* packet = DrawMesh(s_Data.LineQuadMesh, transform, material);

        // Modify render state to ensure skeleton visibility
        if (packet)
        {
            auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
            if (drawCmd)
            {
                // Disable depth test so skeleton always shows on top
                drawCmd->renderState.depthTestEnabled = false;
                // Add some polygon offset to push skeleton forward
                drawCmd->renderState.polygonOffsetEnabled = true;
                drawCmd->renderState.polygonOffsetFactor = -2.0f;
                drawCmd->renderState.polygonOffsetUnits = -2.0f;
            }
        }

        return packet;
    }

    CommandPacket* Renderer3D::DrawSphere(const glm::vec3& position, f32 radius, const glm::vec3& color)
    {
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSphere: ScenePass is null!");
            return nullptr;
        }

        // Create transform matrix for the sphere
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(radius));

        // Use a highly emissive material for skeleton joints
        Material material{};
        material.SetType(MaterialType::PBR);
        material.SetBaseColorFactor(glm::vec4(color, 1.0f));
        material.SetMetallicFactor(0.0f);
        material.SetRoughnessFactor(0.8f);
        material.SetEmissiveFactor(glm::vec4(color * 3.0f, 1.0f)); // Very bright emission for visibility through surfaces

        CommandPacket* packet = nullptr;

        // Use the cube mesh as a simple sphere approximation for now
        // TODO: Create a proper sphere mesh in the renderer data
        if (s_Data.CubeMesh)
        {
            packet = DrawMesh(s_Data.CubeMesh, transform, material);
        }
        else
        {
            OLO_CORE_WARN("Renderer3D::DrawSphere: No sphere mesh available, using fallback");
            return nullptr;
        }

        // Modify render state to ensure skeleton joint visibility
        if (packet)
        {
            auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
            if (drawCmd)
            {
                // Disable depth test so skeleton joints always show on top
                drawCmd->renderState.depthTestEnabled = false;
                // Add polygon offset to push joints forward
                drawCmd->renderState.polygonOffsetEnabled = true;
                drawCmd->renderState.polygonOffsetFactor = -2.0f;
                drawCmd->renderState.polygonOffsetUnits = -2.0f;
            }
        }

        return packet;
    }

    void Renderer3D::DrawCameraFrustum(const glm::mat4& cameraTransform,
                                       f32 fov, f32 aspectRatio,
                                       f32 nearClip, f32 farClip,
                                       const glm::vec3& color,
                                       bool isPerspective,
                                       f32 orthoSize)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawCameraFrustum: ScenePass is null!");
            return;
        }

        // Clamp far plane for visualization (avoid frustums too large to be useful)
        const f32 visualFar = glm::min(farClip, 50.0f);
        const f32 lineThickness = 1.5f;

        // Extract camera position and orientation from transform
        glm::vec3 position = glm::vec3(cameraTransform[3]);
        glm::vec3 forward = -glm::normalize(glm::vec3(cameraTransform[2])); // -Z is forward
        glm::vec3 right = glm::normalize(glm::vec3(cameraTransform[0]));
        glm::vec3 up = glm::normalize(glm::vec3(cameraTransform[1]));

        // Calculate frustum corners
        std::array<glm::vec3, 8> corners;

        if (isPerspective)
        {
            // Perspective frustum
            const f32 tanHalfFov = std::tan(fov * 0.5f);

            // Near plane dimensions
            const f32 nearHeight = 2.0f * tanHalfFov * nearClip;
            const f32 nearWidth = nearHeight * aspectRatio;

            // Far plane dimensions (clamped for visualization)
            const f32 farHeight = 2.0f * tanHalfFov * visualFar;
            const f32 farWidth = farHeight * aspectRatio;

            // Near plane center
            glm::vec3 nearCenter = position + forward * nearClip;
            // Far plane center
            glm::vec3 farCenter = position + forward * visualFar;

            // Near plane corners (top-left, top-right, bottom-right, bottom-left)
            corners[0] = nearCenter + up * (nearHeight * 0.5f) - right * (nearWidth * 0.5f); // Near top-left
            corners[1] = nearCenter + up * (nearHeight * 0.5f) + right * (nearWidth * 0.5f); // Near top-right
            corners[2] = nearCenter - up * (nearHeight * 0.5f) + right * (nearWidth * 0.5f); // Near bottom-right
            corners[3] = nearCenter - up * (nearHeight * 0.5f) - right * (nearWidth * 0.5f); // Near bottom-left

            // Far plane corners
            corners[4] = farCenter + up * (farHeight * 0.5f) - right * (farWidth * 0.5f); // Far top-left
            corners[5] = farCenter + up * (farHeight * 0.5f) + right * (farWidth * 0.5f); // Far top-right
            corners[6] = farCenter - up * (farHeight * 0.5f) + right * (farWidth * 0.5f); // Far bottom-right
            corners[7] = farCenter - up * (farHeight * 0.5f) - right * (farWidth * 0.5f); // Far bottom-left
        }
        else
        {
            // Orthographic frustum
            const f32 halfHeight = orthoSize;
            const f32 halfWidth = orthoSize * aspectRatio;

            glm::vec3 nearCenter = position + forward * nearClip;
            glm::vec3 farCenter = position + forward * visualFar;

            // Near plane corners
            corners[0] = nearCenter + up * halfHeight - right * halfWidth;
            corners[1] = nearCenter + up * halfHeight + right * halfWidth;
            corners[2] = nearCenter - up * halfHeight + right * halfWidth;
            corners[3] = nearCenter - up * halfHeight - right * halfWidth;

            // Far plane corners
            corners[4] = farCenter + up * halfHeight - right * halfWidth;
            corners[5] = farCenter + up * halfHeight + right * halfWidth;
            corners[6] = farCenter - up * halfHeight + right * halfWidth;
            corners[7] = farCenter - up * halfHeight - right * halfWidth;
        }

        // Draw near plane (quad)
        auto* p0 = DrawLine(corners[0], corners[1], color, lineThickness);
        auto* p1 = DrawLine(corners[1], corners[2], color, lineThickness);
        auto* p2 = DrawLine(corners[2], corners[3], color, lineThickness);
        auto* p3 = DrawLine(corners[3], corners[0], color, lineThickness);
        if (p0)
            SubmitPacket(p0);
        if (p1)
            SubmitPacket(p1);
        if (p2)
            SubmitPacket(p2);
        if (p3)
            SubmitPacket(p3);

        // Draw far plane (quad)
        auto* p4 = DrawLine(corners[4], corners[5], color, lineThickness);
        auto* p5 = DrawLine(corners[5], corners[6], color, lineThickness);
        auto* p6 = DrawLine(corners[6], corners[7], color, lineThickness);
        auto* p7 = DrawLine(corners[7], corners[4], color, lineThickness);
        if (p4)
            SubmitPacket(p4);
        if (p5)
            SubmitPacket(p5);
        if (p6)
            SubmitPacket(p6);
        if (p7)
            SubmitPacket(p7);

        // Draw connecting edges (near to far)
        auto* p8 = DrawLine(corners[0], corners[4], color, lineThickness);
        auto* p9 = DrawLine(corners[1], corners[5], color, lineThickness);
        auto* p10 = DrawLine(corners[2], corners[6], color, lineThickness);
        auto* p11 = DrawLine(corners[3], corners[7], color, lineThickness);
        if (p8)
            SubmitPacket(p8);
        if (p9)
            SubmitPacket(p9);
        if (p10)
            SubmitPacket(p10);
        if (p11)
            SubmitPacket(p11);

        // Draw camera direction indicator (small arrow from camera position)
        const f32 arrowLength = 0.5f;
        const f32 arrowHeadSize = 0.15f;
        glm::vec3 arrowTip = position + forward * arrowLength;
        glm::vec3 arrowColor = glm::vec3(0.2f, 0.8f, 0.2f); // Green for direction

        auto* arrowLine = DrawLine(position, arrowTip, arrowColor, lineThickness * 1.5f);
        if (arrowLine)
            SubmitPacket(arrowLine);

        // Arrow head (two lines from tip going back)
        glm::vec3 arrowBack = position + forward * (arrowLength - arrowHeadSize);
        glm::vec3 arrowHead1 = arrowBack + up * arrowHeadSize * 0.5f;
        glm::vec3 arrowHead2 = arrowBack - up * arrowHeadSize * 0.5f;
        glm::vec3 arrowHead3 = arrowBack + right * arrowHeadSize * 0.5f;
        glm::vec3 arrowHead4 = arrowBack - right * arrowHeadSize * 0.5f;

        auto* ah1 = DrawLine(arrowTip, arrowHead1, arrowColor, lineThickness);
        auto* ah2 = DrawLine(arrowTip, arrowHead2, arrowColor, lineThickness);
        auto* ah3 = DrawLine(arrowTip, arrowHead3, arrowColor, lineThickness);
        auto* ah4 = DrawLine(arrowTip, arrowHead4, arrowColor, lineThickness);
        if (ah1)
            SubmitPacket(ah1);
        if (ah2)
            SubmitPacket(ah2);
        if (ah3)
            SubmitPacket(ah3);
        if (ah4)
            SubmitPacket(ah4);
    }

    CommandPacket* Renderer3D::DrawInfiniteGrid(f32 gridScale)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawInfiniteGrid: ScenePass is null!");
            return nullptr;
        }

        if (!s_Data.InfiniteGridShader)
        {
            OLO_CORE_ERROR("Renderer3D::DrawInfiniteGrid: InfiniteGrid shader not loaded!");
            return nullptr;
        }

        if (!s_Data.FullscreenQuadVAO)
        {
            OLO_CORE_ERROR("Renderer3D::DrawInfiniteGrid: FullscreenQuadVAO not initialized!");
            return nullptr;
        }

        // Create POD command packet
        CommandPacket* packet = CreateDrawCall<DrawInfiniteGridCommand>();
        auto* cmd = packet->GetCommandData<DrawInfiniteGridCommand>();
        cmd->header.type = CommandType::DrawInfiniteGrid;

        // Store renderer IDs (POD)
        cmd->shaderHandle = s_Data.InfiniteGridShader->GetHandle();
        cmd->shaderRendererID = s_Data.InfiniteGridShader->GetRendererID();
        cmd->quadVAOID = s_Data.FullscreenQuadVAO->GetRendererID();
        cmd->gridScale = gridScale;

        // Grid-specific render state
        cmd->renderState = CreateDefaultPODRenderState();
        cmd->renderState.blendEnabled = true;
        cmd->renderState.blendSrcFactor = GL_SRC_ALPHA;
        cmd->renderState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
        cmd->renderState.depthTestEnabled = true;
        cmd->renderState.depthWriteMask = true;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for grid (rendered in opaque layer, low priority)
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = s_Data.InfiniteGridShader->GetRendererID() & 0xFFFF;
        // Grid renders at medium depth, after opaque geometry
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, 0x800000);
        packet->SetMetadata(metadata);

        // Submit packet
        SubmitPacket(packet);
        return packet;
    }

    CommandPacket* Renderer3D::DrawTerrainPatch(
        RendererID vaoID, u32 indexCount, u32 patchVertexCount,
        const Ref<Shader>& shader,
        RendererID heightmapID, RendererID splatmapID, RendererID splatmap1ID,
        RendererID albedoArrayID, RendererID normalArrayID, RendererID armArrayID,
        const glm::mat4& transform,
        const ShaderBindingLayout::TerrainUBO& terrainUBO,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawTerrainPatch: ScenePass is null!");
            return nullptr;
        }

        if (vaoID == 0 || !shader)
        {
            return nullptr;
        }

        CommandPacket* packet = CreateDrawCall<DrawTerrainPatchCommand>();
        auto* cmd = packet->GetCommandData<DrawTerrainPatchCommand>();
        cmd->header.type = CommandType::DrawTerrainPatch;

        cmd->vertexArrayID = vaoID;
        cmd->indexCount = indexCount;
        cmd->patchVertexCount = patchVertexCount;
        cmd->shaderRendererID = shader->GetRendererID();
        cmd->heightmapTextureID = heightmapID;
        cmd->splatmapTextureID = splatmapID;
        cmd->splatmap1TextureID = splatmap1ID;
        cmd->albedoArrayTextureID = albedoArrayID;
        cmd->normalArrayTextureID = normalArrayID;
        cmd->armArrayTextureID = armArrayID;
        cmd->transform = transform;
        cmd->entityID = entityID;
        cmd->terrainUBOData = terrainUBO;

        // Terrain is opaque — depth test on, no blending, culling on
        cmd->renderState = CreateDefaultPODRenderState();
        cmd->renderState.blendEnabled = false;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: group by shader for state efficiency
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(transform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = true;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawVoxelMesh(
        RendererID vaoID, u32 indexCount,
        const Ref<Shader>& shader,
        RendererID albedoArrayID, RendererID normalArrayID, RendererID armArrayID,
        const glm::mat4& transform,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawVoxelMesh: ScenePass is null!");
            return nullptr;
        }

        if (vaoID == 0 || !shader)
        {
            return nullptr;
        }

        CommandPacket* packet = CreateDrawCall<DrawVoxelMeshCommand>();
        auto* cmd = packet->GetCommandData<DrawVoxelMeshCommand>();
        cmd->header.type = CommandType::DrawVoxelMesh;

        cmd->vertexArrayID = vaoID;
        cmd->indexCount = indexCount;
        cmd->shaderRendererID = shader->GetRendererID();
        cmd->albedoArrayTextureID = albedoArrayID;
        cmd->normalArrayTextureID = normalArrayID;
        cmd->armArrayTextureID = armArrayID;
        cmd->transform = transform;
        cmd->entityID = entityID;

        cmd->renderState = CreateDefaultPODRenderState();
        cmd->renderState.blendEnabled = false;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(transform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = true;
        packet->SetMetadata(metadata);

        return packet;
    }

    void Renderer3D::DrawDirectionalLightGizmo(const glm::vec3& position,
                                               const glm::vec3& direction,
                                               const glm::vec3& color,
                                               f32 intensity)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 2.0f;
        const f32 arrowLength = 1.5f;
        const f32 arrowHeadSize = 0.2f;

        // Normalize direction
        glm::vec3 dir = glm::normalize(direction);
        glm::vec3 arrowEnd = position + dir * arrowLength;

        // Main arrow line
        auto* mainLine = DrawLine(position, arrowEnd, color * intensity, lineThickness);
        if (mainLine)
            SubmitPacket(mainLine);

        // Create arrow head - find perpendicular vectors
        glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0, 1, 0))) < 0.99f
                           ? glm::vec3(0, 1, 0)
                           : glm::vec3(1, 0, 0);
        glm::vec3 right = glm::normalize(glm::cross(dir, up));
        up = glm::normalize(glm::cross(right, dir));

        glm::vec3 arrowBack = arrowEnd - dir * arrowHeadSize;

        // Arrow head lines (4 lines forming a pyramid)
        auto* ah1 = DrawLine(arrowEnd, arrowBack + up * arrowHeadSize, color * intensity, lineThickness);
        auto* ah2 = DrawLine(arrowEnd, arrowBack - up * arrowHeadSize, color * intensity, lineThickness);
        auto* ah3 = DrawLine(arrowEnd, arrowBack + right * arrowHeadSize, color * intensity, lineThickness);
        auto* ah4 = DrawLine(arrowEnd, arrowBack - right * arrowHeadSize, color * intensity, lineThickness);
        if (ah1)
            SubmitPacket(ah1);
        if (ah2)
            SubmitPacket(ah2);
        if (ah3)
            SubmitPacket(ah3);
        if (ah4)
            SubmitPacket(ah4);

        // Draw a small sun-like icon at position (circle with rays)
        const f32 sunRadius = 0.2f;
        const i32 segments = 8;
        for (i32 i = 0; i < segments; ++i)
        {
            f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

            glm::vec3 p1 = position + right * std::cos(angle1) * sunRadius + up * std::sin(angle1) * sunRadius;
            glm::vec3 p2 = position + right * std::cos(angle2) * sunRadius + up * std::sin(angle2) * sunRadius;

            auto* seg = DrawLine(p1, p2, color * intensity, lineThickness * 0.8f);
            if (seg)
                SubmitPacket(seg);
        }

        // Rays emanating from the sun
        for (i32 i = 0; i < 8; ++i)
        {
            f32 angle = (f32(i) / 8.0f) * glm::two_pi<f32>();
            glm::vec3 rayDir = right * std::cos(angle) + up * std::sin(angle);
            glm::vec3 rayStart = position + rayDir * (sunRadius + 0.05f);
            glm::vec3 rayEnd = position + rayDir * (sunRadius + 0.15f);

            auto* ray = DrawLine(rayStart, rayEnd, color * intensity, lineThickness * 0.6f);
            if (ray)
                SubmitPacket(ray);
        }
    }

    void Renderer3D::DrawPointLightGizmo(const glm::vec3& position,
                                         f32 range,
                                         const glm::vec3& color,
                                         bool showRangeSphere)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 1.5f;

        // Draw a light bulb icon at position
        const f32 bulbRadius = 0.15f;
        const i32 segments = 12;

        // Draw three orthogonal circles to represent a sphere
        for (i32 axis = 0; axis < 3; ++axis)
        {
            for (i32 i = 0; i < segments; ++i)
            {
                f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                glm::vec3 offset1, offset2;
                if (axis == 0) // XY plane
                {
                    offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * bulbRadius;
                    offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * bulbRadius;
                }
                else if (axis == 1) // XZ plane
                {
                    offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * bulbRadius;
                    offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * bulbRadius;
                }
                else // YZ plane
                {
                    offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * bulbRadius;
                    offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * bulbRadius;
                }

                auto* seg = DrawLine(position + offset1, position + offset2, color, lineThickness);
                if (seg)
                    SubmitPacket(seg);
            }
        }

        // Draw range sphere if enabled (larger circle to show falloff range)
        if (showRangeSphere && range > 0.0f)
        {
            // Clamp range for visualization
            f32 visualRange = glm::min(range, 20.0f);
            const i32 rangeSegments = 24;
            glm::vec3 dimColor = color * 0.3f;

            // Draw range circles on three planes
            for (i32 axis = 0; axis < 3; ++axis)
            {
                for (i32 i = 0; i < rangeSegments; ++i)
                {
                    f32 angle1 = (f32(i) / f32(rangeSegments)) * glm::two_pi<f32>();
                    f32 angle2 = (f32(i + 1) / f32(rangeSegments)) * glm::two_pi<f32>();

                    glm::vec3 offset1, offset2;
                    if (axis == 0)
                    {
                        offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * visualRange;
                        offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * visualRange;
                    }
                    else if (axis == 1)
                    {
                        offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * visualRange;
                        offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * visualRange;
                    }
                    else
                    {
                        offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * visualRange;
                        offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * visualRange;
                    }

                    auto* seg = DrawLine(position + offset1, position + offset2, dimColor, lineThickness * 0.5f);
                    if (seg)
                        SubmitPacket(seg);
                }
            }
        }
    }

    void Renderer3D::DrawSpotLightGizmo(const glm::vec3& position,
                                        const glm::vec3& direction,
                                        f32 range,
                                        f32 innerCutoff,
                                        f32 outerCutoff,
                                        const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 1.5f;

        // Normalize direction
        glm::vec3 dir = glm::normalize(direction);

        // Clamp range for visualization
        f32 visualRange = glm::min(range, 15.0f);

        // Calculate outer cone radius at the end of the range
        f32 outerAngleRad = glm::radians(outerCutoff);
        f32 innerAngleRad = glm::radians(innerCutoff);
        f32 outerRadius = visualRange * std::tan(outerAngleRad);
        f32 innerRadius = visualRange * std::tan(innerAngleRad);

        // Create coordinate system aligned with direction
        glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0, 1, 0))) < 0.99f
                           ? glm::vec3(0, 1, 0)
                           : glm::vec3(1, 0, 0);
        glm::vec3 right = glm::normalize(glm::cross(dir, up));
        up = glm::normalize(glm::cross(right, dir));

        // Cone tip to base
        glm::vec3 coneEnd = position + dir * visualRange;

        // Draw central direction line
        auto* centerLine = DrawLine(position, coneEnd, color, lineThickness);
        if (centerLine)
            SubmitPacket(centerLine);

        // Draw outer cone edges (4 lines from tip to base)
        const i32 coneEdges = 8;
        for (i32 i = 0; i < coneEdges; ++i)
        {
            f32 angle = (f32(i) / f32(coneEdges)) * glm::two_pi<f32>();
            glm::vec3 offset = right * std::cos(angle) * outerRadius + up * std::sin(angle) * outerRadius;
            glm::vec3 edgeEnd = coneEnd + offset;

            auto* edge = DrawLine(position, edgeEnd, color * 0.6f, lineThickness * 0.8f);
            if (edge)
                SubmitPacket(edge);
        }

        // Draw outer cone base circle
        const i32 circleSegments = 16;
        for (i32 i = 0; i < circleSegments; ++i)
        {
            f32 angle1 = (f32(i) / f32(circleSegments)) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / f32(circleSegments)) * glm::two_pi<f32>();

            glm::vec3 p1 = coneEnd + right * std::cos(angle1) * outerRadius + up * std::sin(angle1) * outerRadius;
            glm::vec3 p2 = coneEnd + right * std::cos(angle2) * outerRadius + up * std::sin(angle2) * outerRadius;

            auto* seg = DrawLine(p1, p2, color * 0.5f, lineThickness * 0.7f);
            if (seg)
                SubmitPacket(seg);
        }

        // Draw inner cone base circle (brighter, showing hotspot)
        if (innerRadius > 0.01f && innerRadius < outerRadius)
        {
            for (i32 i = 0; i < circleSegments; ++i)
            {
                f32 angle1 = (f32(i) / f32(circleSegments)) * glm::two_pi<f32>();
                f32 angle2 = (f32(i + 1) / f32(circleSegments)) * glm::two_pi<f32>();

                glm::vec3 p1 = coneEnd + right * std::cos(angle1) * innerRadius + up * std::sin(angle1) * innerRadius;
                glm::vec3 p2 = coneEnd + right * std::cos(angle2) * innerRadius + up * std::sin(angle2) * innerRadius;

                auto* seg = DrawLine(p1, p2, color, lineThickness);
                if (seg)
                    SubmitPacket(seg);
            }
        }

        // Draw a small light bulb icon at position
        const f32 bulbRadius = 0.1f;
        for (i32 i = 0; i < 8; ++i)
        {
            f32 angle1 = (f32(i) / 8.0f) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / 8.0f) * glm::two_pi<f32>();

            glm::vec3 p1 = position + right * std::cos(angle1) * bulbRadius + up * std::sin(angle1) * bulbRadius;
            glm::vec3 p2 = position + right * std::cos(angle2) * bulbRadius + up * std::sin(angle2) * bulbRadius;

            auto* seg = DrawLine(p1, p2, color, lineThickness);
            if (seg)
                SubmitPacket(seg);
        }
    }

    void Renderer3D::DrawAudioSourceGizmo(const glm::vec3& position,
                                          f32 minDistance,
                                          f32 maxDistance,
                                          const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 1.5f;
        const i32 segments = 16;

        // Clamp distances for visualization
        f32 visualMin = glm::min(minDistance, 10.0f);
        f32 visualMax = glm::min(maxDistance, 30.0f);

        // Draw speaker icon at position (small)
        const f32 iconSize = 0.15f;
        glm::vec3 iconColor = color * 1.2f;

        // Speaker body (rectangle)
        auto* l1 = DrawLine(position + glm::vec3(-iconSize, -iconSize * 0.5f, 0),
                            position + glm::vec3(-iconSize, iconSize * 0.5f, 0), iconColor, lineThickness);
        auto* l2 = DrawLine(position + glm::vec3(-iconSize, iconSize * 0.5f, 0),
                            position + glm::vec3(0, iconSize * 0.5f, 0), iconColor, lineThickness);
        auto* l3 = DrawLine(position + glm::vec3(0, iconSize * 0.5f, 0),
                            position + glm::vec3(iconSize, iconSize, 0), iconColor, lineThickness);
        auto* l4 = DrawLine(position + glm::vec3(iconSize, iconSize, 0),
                            position + glm::vec3(iconSize, -iconSize, 0), iconColor, lineThickness);
        auto* l5 = DrawLine(position + glm::vec3(iconSize, -iconSize, 0),
                            position + glm::vec3(0, -iconSize * 0.5f, 0), iconColor, lineThickness);
        auto* l6 = DrawLine(position + glm::vec3(0, -iconSize * 0.5f, 0),
                            position + glm::vec3(-iconSize, -iconSize * 0.5f, 0), iconColor, lineThickness);
        if (l1)
            SubmitPacket(l1);
        if (l2)
            SubmitPacket(l2);
        if (l3)
            SubmitPacket(l3);
        if (l4)
            SubmitPacket(l4);
        if (l5)
            SubmitPacket(l5);
        if (l6)
            SubmitPacket(l6);

        // Draw min distance sphere (bright, full volume zone)
        if (visualMin > 0.01f)
        {
            for (i32 axis = 0; axis < 3; ++axis)
            {
                for (i32 i = 0; i < segments; ++i)
                {
                    f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                    f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                    glm::vec3 offset1, offset2;
                    if (axis == 0)
                    {
                        offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * visualMin;
                        offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * visualMin;
                    }
                    else if (axis == 1)
                    {
                        offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * visualMin;
                        offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * visualMin;
                    }
                    else
                    {
                        offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * visualMin;
                        offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * visualMin;
                    }

                    auto* seg = DrawLine(position + offset1, position + offset2, color, lineThickness);
                    if (seg)
                        SubmitPacket(seg);
                }
            }
        }

        // Draw max distance sphere (dimmer, falloff boundary)
        if (visualMax > visualMin)
        {
            glm::vec3 dimColor = color * 0.3f;
            for (i32 axis = 0; axis < 3; ++axis)
            {
                for (i32 i = 0; i < segments; ++i)
                {
                    f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                    f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                    glm::vec3 offset1, offset2;
                    if (axis == 0)
                    {
                        offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * visualMax;
                        offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * visualMax;
                    }
                    else if (axis == 1)
                    {
                        offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * visualMax;
                        offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * visualMax;
                    }
                    else
                    {
                        offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * visualMax;
                        offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * visualMax;
                    }

                    auto* seg = DrawLine(position + offset1, position + offset2, dimColor, lineThickness * 0.5f);
                    if (seg)
                        SubmitPacket(seg);
                }
            }
        }
    }

    void Renderer3D::DrawWorldAxisHelper(f32 axisLength)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 2.5f;
        const glm::vec3 origin(0.0f);

        // X axis - Red
        auto* xAxis = DrawLine(origin, glm::vec3(axisLength, 0.0f, 0.0f), glm::vec3(1.0f, 0.2f, 0.2f), lineThickness);
        if (xAxis)
            SubmitPacket(xAxis);

        // X axis arrow head
        auto* xArrow1 = DrawLine(glm::vec3(axisLength, 0, 0), glm::vec3(axisLength - 0.2f, 0.1f, 0), glm::vec3(1.0f, 0.2f, 0.2f), lineThickness);
        auto* xArrow2 = DrawLine(glm::vec3(axisLength, 0, 0), glm::vec3(axisLength - 0.2f, -0.1f, 0), glm::vec3(1.0f, 0.2f, 0.2f), lineThickness);
        if (xArrow1)
            SubmitPacket(xArrow1);
        if (xArrow2)
            SubmitPacket(xArrow2);

        // Y axis - Green
        auto* yAxis = DrawLine(origin, glm::vec3(0.0f, axisLength, 0.0f), glm::vec3(0.2f, 1.0f, 0.2f), lineThickness);
        if (yAxis)
            SubmitPacket(yAxis);

        // Y axis arrow head
        auto* yArrow1 = DrawLine(glm::vec3(0, axisLength, 0), glm::vec3(0.1f, axisLength - 0.2f, 0), glm::vec3(0.2f, 1.0f, 0.2f), lineThickness);
        auto* yArrow2 = DrawLine(glm::vec3(0, axisLength, 0), glm::vec3(-0.1f, axisLength - 0.2f, 0), glm::vec3(0.2f, 1.0f, 0.2f), lineThickness);
        if (yArrow1)
            SubmitPacket(yArrow1);
        if (yArrow2)
            SubmitPacket(yArrow2);

        // Z axis - Blue
        auto* zAxis = DrawLine(origin, glm::vec3(0.0f, 0.0f, axisLength), glm::vec3(0.2f, 0.2f, 1.0f), lineThickness);
        if (zAxis)
            SubmitPacket(zAxis);

        // Z axis arrow head
        auto* zArrow1 = DrawLine(glm::vec3(0, 0, axisLength), glm::vec3(0, 0.1f, axisLength - 0.2f), glm::vec3(0.2f, 0.2f, 1.0f), lineThickness);
        auto* zArrow2 = DrawLine(glm::vec3(0, 0, axisLength), glm::vec3(0, -0.1f, axisLength - 0.2f), glm::vec3(0.2f, 0.2f, 1.0f), lineThickness);
        if (zArrow1)
            SubmitPacket(zArrow1);
        if (zArrow2)
            SubmitPacket(zArrow2);

        // Draw small negative axis indicators (dashed appearance using short segments)
        const f32 negLength = axisLength * 0.3f;
        const f32 dashLen = 0.1f;

        // Negative X (dimmer red)
        for (f32 t = 0; t < negLength; t += dashLen * 2)
        {
            auto* dash = DrawLine(glm::vec3(-t, 0, 0), glm::vec3(-t - dashLen, 0, 0), glm::vec3(0.5f, 0.1f, 0.1f), lineThickness * 0.5f);
            if (dash)
                SubmitPacket(dash);
        }

        // Negative Y (dimmer green)
        for (f32 t = 0; t < negLength; t += dashLen * 2)
        {
            auto* dash = DrawLine(glm::vec3(0, -t, 0), glm::vec3(0, -t - dashLen, 0), glm::vec3(0.1f, 0.5f, 0.1f), lineThickness * 0.5f);
            if (dash)
                SubmitPacket(dash);
        }

        // Negative Z (dimmer blue)
        for (f32 t = 0; t < negLength; t += dashLen * 2)
        {
            auto* dash = DrawLine(glm::vec3(0, 0, -t), glm::vec3(0, 0, -t - dashLen), glm::vec3(0.1f, 0.1f, 0.5f), lineThickness * 0.5f);
            if (dash)
                SubmitPacket(dash);
        }
    }

    void Renderer3D::DrawBoxColliderGizmo(const glm::vec3& position,
                                          const glm::vec3& halfExtents,
                                          const glm::quat& rotation,
                                          const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 2.0f;

        // Create rotation matrix
        glm::mat3 rotMat = glm::mat3_cast(rotation);

        // Local corners of the box
        glm::vec3 corners[8] = {
            glm::vec3(-halfExtents.x, -halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x, -halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x, halfExtents.y, -halfExtents.z),
            glm::vec3(-halfExtents.x, halfExtents.y, -halfExtents.z),
            glm::vec3(-halfExtents.x, -halfExtents.y, halfExtents.z),
            glm::vec3(halfExtents.x, -halfExtents.y, halfExtents.z),
            glm::vec3(halfExtents.x, halfExtents.y, halfExtents.z),
            glm::vec3(-halfExtents.x, halfExtents.y, halfExtents.z)
        };

        // Transform corners to world space
        for (auto& corner : corners)
        {
            corner = position + rotMat * corner;
        }

        // Draw edges - bottom face
        auto* e1 = DrawLine(corners[0], corners[1], color, lineThickness);
        auto* e2 = DrawLine(corners[1], corners[2], color, lineThickness);
        auto* e3 = DrawLine(corners[2], corners[3], color, lineThickness);
        auto* e4 = DrawLine(corners[3], corners[0], color, lineThickness);
        if (e1)
            SubmitPacket(e1);
        if (e2)
            SubmitPacket(e2);
        if (e3)
            SubmitPacket(e3);
        if (e4)
            SubmitPacket(e4);

        // Top face
        auto* e5 = DrawLine(corners[4], corners[5], color, lineThickness);
        auto* e6 = DrawLine(corners[5], corners[6], color, lineThickness);
        auto* e7 = DrawLine(corners[6], corners[7], color, lineThickness);
        auto* e8 = DrawLine(corners[7], corners[4], color, lineThickness);
        if (e5)
            SubmitPacket(e5);
        if (e6)
            SubmitPacket(e6);
        if (e7)
            SubmitPacket(e7);
        if (e8)
            SubmitPacket(e8);

        // Vertical edges
        auto* e9 = DrawLine(corners[0], corners[4], color, lineThickness);
        auto* e10 = DrawLine(corners[1], corners[5], color, lineThickness);
        auto* e11 = DrawLine(corners[2], corners[6], color, lineThickness);
        auto* e12 = DrawLine(corners[3], corners[7], color, lineThickness);
        if (e9)
            SubmitPacket(e9);
        if (e10)
            SubmitPacket(e10);
        if (e11)
            SubmitPacket(e11);
        if (e12)
            SubmitPacket(e12);
    }

    void Renderer3D::DrawSphereColliderGizmo(const glm::vec3& position,
                                             f32 radius,
                                             const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 1.5f;
        const i32 segments = 24;

        // Draw three circles for each axis
        for (i32 axis = 0; axis < 3; ++axis)
        {
            for (i32 i = 0; i < segments; ++i)
            {
                f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                glm::vec3 p1, p2;
                if (axis == 0) // XY plane
                {
                    p1 = position + glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * radius;
                    p2 = position + glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * radius;
                }
                else if (axis == 1) // XZ plane
                {
                    p1 = position + glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * radius;
                    p2 = position + glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * radius;
                }
                else // YZ plane
                {
                    p1 = position + glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * radius;
                    p2 = position + glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * radius;
                }

                auto* seg = DrawLine(p1, p2, color, lineThickness);
                if (seg)
                    SubmitPacket(seg);
            }
        }
    }

    void Renderer3D::DrawCapsuleColliderGizmo(const glm::vec3& position,
                                              f32 radius,
                                              f32 halfHeight,
                                              const glm::quat& rotation,
                                              const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ScenePass)
        {
            return;
        }

        const f32 lineThickness = 1.5f;
        const i32 segments = 16;
        const i32 hemisphereSegments = 8;

        // Create rotation matrix
        glm::mat3 rotMat = glm::mat3_cast(rotation);

        // Local up vector (capsule aligned with Y axis locally)
        glm::vec3 localUp = rotMat * glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 localRight = rotMat * glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 localForward = rotMat * glm::vec3(0.0f, 0.0f, 1.0f);

        glm::vec3 topCenter = position + localUp * halfHeight;
        glm::vec3 bottomCenter = position - localUp * halfHeight;

        // Draw cylindrical body circles at top and bottom
        for (i32 i = 0; i < segments; ++i)
        {
            f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

            glm::vec3 offset1 = localRight * std::cos(angle1) * radius + localForward * std::sin(angle1) * radius;
            glm::vec3 offset2 = localRight * std::cos(angle2) * radius + localForward * std::sin(angle2) * radius;

            // Top circle
            auto* topSeg = DrawLine(topCenter + offset1, topCenter + offset2, color, lineThickness);
            if (topSeg)
                SubmitPacket(topSeg);

            // Bottom circle
            auto* bottomSeg = DrawLine(bottomCenter + offset1, bottomCenter + offset2, color, lineThickness);
            if (bottomSeg)
                SubmitPacket(bottomSeg);
        }

        // Draw vertical lines connecting top and bottom
        for (i32 i = 0; i < 4; ++i)
        {
            f32 angle = (f32(i) / 4.0f) * glm::two_pi<f32>();
            glm::vec3 offset = localRight * std::cos(angle) * radius + localForward * std::sin(angle) * radius;

            auto* vertLine = DrawLine(topCenter + offset, bottomCenter + offset, color, lineThickness);
            if (vertLine)
                SubmitPacket(vertLine);
        }

        // Draw top hemisphere
        for (i32 i = 0; i < hemisphereSegments; ++i)
        {
            f32 phi1 = (f32(i) / f32(hemisphereSegments)) * glm::half_pi<f32>();
            f32 phi2 = (f32(i + 1) / f32(hemisphereSegments)) * glm::half_pi<f32>();

            f32 r1 = radius * std::cos(phi1);
            f32 r2 = radius * std::cos(phi2);
            f32 y1 = radius * std::sin(phi1);
            f32 y2 = radius * std::sin(phi2);

            // Draw arc segments
            for (i32 j = 0; j < segments; ++j)
            {
                f32 theta = (f32(j) / f32(segments)) * glm::two_pi<f32>();
                glm::vec3 offset1 = localRight * std::cos(theta) * r1 + localForward * std::sin(theta) * r1;
                glm::vec3 offset2 = localRight * std::cos(theta) * r2 + localForward * std::sin(theta) * r2;

                auto* arcSeg = DrawLine(topCenter + offset1 + localUp * y1,
                                        topCenter + offset2 + localUp * y2, color, lineThickness * 0.5f);
                if (arcSeg)
                    SubmitPacket(arcSeg);
            }
        }

        // Draw bottom hemisphere
        for (i32 i = 0; i < hemisphereSegments; ++i)
        {
            f32 phi1 = (f32(i) / f32(hemisphereSegments)) * glm::half_pi<f32>();
            f32 phi2 = (f32(i + 1) / f32(hemisphereSegments)) * glm::half_pi<f32>();

            f32 r1 = radius * std::cos(phi1);
            f32 r2 = radius * std::cos(phi2);
            f32 y1 = radius * std::sin(phi1);
            f32 y2 = radius * std::sin(phi2);

            // Draw arc segments
            for (i32 j = 0; j < segments; ++j)
            {
                f32 theta = (f32(j) / f32(segments)) * glm::two_pi<f32>();
                glm::vec3 offset1 = localRight * std::cos(theta) * r1 + localForward * std::sin(theta) * r1;
                glm::vec3 offset2 = localRight * std::cos(theta) * r2 + localForward * std::sin(theta) * r2;

                auto* arcSeg = DrawLine(bottomCenter + offset1 - localUp * y1,
                                        bottomCenter + offset2 - localUp * y2, color, lineThickness * 0.5f);
                if (arcSeg)
                    SubmitPacket(arcSeg);
            }
        }
    }

    void Renderer3D::DrawSkeleton(const Skeleton& skeleton, const glm::mat4& modelMatrix,
                                  bool showBones, bool showJoints, f32 jointSize, f32 boneThickness)
    {
        if (!s_Data.ScenePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkeleton: ScenePass is null!");
            return;
        }

        if (skeleton.m_GlobalTransforms.empty() || skeleton.m_ParentIndices.empty())
        {
            OLO_CORE_WARN("Renderer3D::DrawSkeleton: Empty skeleton data");
            return;
        }

        if (skeleton.m_GlobalTransforms.size() != skeleton.m_ParentIndices.size())
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkeleton: Skeleton transforms and parents size mismatch");
            return;
        }

        // Colors for visualization
        const glm::vec3 boneColor(1.0f, 0.5f, 0.0f);  // Bright orange for bones
        const glm::vec3 jointColor(0.0f, 1.0f, 0.0f); // Bright green for joints

        // Debug: Log skeleton rendering attempt
        static int debugCount = 0;
        if (debugCount < 5) // Only log first 5 attempts to avoid spam
        {
            OLO_CORE_INFO("DrawSkeleton Debug #{}: showJoints={}, showBones={}, jointSize={}, boneThickness={}",
                          debugCount, showJoints, showBones, jointSize, boneThickness);
            OLO_CORE_INFO("  Skeleton size: {}, SkyboxMesh available: {}",
                          skeleton.m_GlobalTransforms.size(), (s_Data.SkyboxMesh != nullptr));
            debugCount++;
        }

        // Draw joints
        if (showJoints)
        {
            for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
            {
                glm::vec3 jointPosition = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[i] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

                // Debug: Log first few joint positions
                if (debugCount <= 5 && i < 3)
                {
                    OLO_CORE_INFO("  Joint {}: world position ({:.2f}, {:.2f}, {:.2f})",
                                  i, jointPosition.x, jointPosition.y, jointPosition.z);
                }

                auto* spherePacket = DrawSphere(jointPosition, jointSize, jointColor);
                if (spherePacket)
                {
                    SubmitPacket(spherePacket);
                    if (debugCount <= 5 && i < 3)
                    {
                        OLO_CORE_INFO("  Joint {} sphere packet submitted successfully", i);
                    }
                }
                else if (debugCount <= 5 && i < 3)
                {
                    OLO_CORE_WARN("  Joint {} sphere packet failed to create", i);
                }
            }
        }

        // Draw bones (connections between joints)
        if (showBones)
        {
            for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
            {
                i32 parentIndex = skeleton.m_ParentIndices[i];
                if (parentIndex >= 0 && parentIndex < static_cast<i32>(skeleton.m_GlobalTransforms.size()))
                {
                    glm::vec3 childPosition = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[i] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                    glm::vec3 parentPosition = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[parentIndex] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

                    // Calculate bone length to filter out unreasonable connections
                    f32 boneLength = glm::length(childPosition - parentPosition);

                    // Only draw bones that are reasonable length (filter out very long connections)
                    // For a human-sized model, bones longer than 2 units are probably incorrect connections
                    const f32 maxReasonableBoneLength = 2.0f;
                    if (boneLength > 0.001f && boneLength < maxReasonableBoneLength)
                    {
                        auto* linePacket = DrawLine(parentPosition, childPosition, boneColor, boneThickness);
                        if (linePacket)
                        {
                            SubmitPacket(linePacket);
                        }
                    }
                }
            }
        }
    }
} // namespace OloEngine
