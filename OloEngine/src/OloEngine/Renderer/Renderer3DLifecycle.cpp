#include "OloEnginePCH.h"
#include "OloEngine/Core/CVar.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Terrain/TerrainGPUQuadtree.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"

// Raw GL below is part of the issue #691 step-2 sweep backlog; the
// include is direct rather than transitive through RendererAPI.h, which is
// now GL-free.
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include "OloEngine/Renderer/CloudNoise.h"
#include "OloEngine/Renderer/CloudShadowMap.h"
#include "OloEngine/Renderer/VolumetricShadowMap.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"
#include "OloEngine/Renderer/Water/WaterSpraySystem.h"
#include "OloEngine/Renderer/Ocean/OceanFFTGpu.h"
#include "OloEngine/Renderer/Debug/GPUReadbackStats.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
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
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"

#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Asset/AssetManager.h"

#include <chrono>
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/GPUTimerQueryPool.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryShadow.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
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

namespace
{
    // The 3D shader filepaths Init() loads, and what Renderer3D::GetShaderFilepaths()
    // (issue #908) returns — ONE array, two readers, so the headless ShaderPack
    // bake can never enumerate a different set than what this library actually
    // tries to serve from the pack at runtime. Keep totalShaders3D (below) in
    // sync with this array's length.
    constexpr std::array kShaderPaths3D = {
        "assets/shaders/LightCube.glsl",
        "assets/shaders/Renderer3D_Quad.glsl",
        "assets/shaders/PBR_MultiLight.glsl",
        "assets/shaders/PBR_MultiLight_Skinned.glsl",
        "assets/shaders/PBR_GBuffer.glsl",
        "assets/shaders/PBR_GBuffer_Skinned.glsl",
        "assets/shaders/EquirectangularToCubemap.glsl",
        "assets/shaders/ProceduralSky.glsl",
        "assets/shaders/StarNestSky.glsl",
        "assets/shaders/AtmosphereSky.glsl",
        "assets/shaders/IrradianceConvolution.glsl",
        "assets/shaders/IrradianceConvolutionAdvanced.glsl",
        "assets/shaders/IrradianceFromSH.glsl",
        "assets/shaders/IBLPrefilter.glsl",
        "assets/shaders/IBLPrefilterImportance.glsl",
        "assets/shaders/BRDFLutGeneration.glsl",
        "assets/shaders/BRDFIntegrationAdvanced.glsl",
        "assets/shaders/Skybox.glsl",
        "assets/shaders/Skybox_GBuffer.glsl",
        "assets/shaders/LightCube_GBuffer.glsl",
        "assets/shaders/InfiniteGrid.glsl",
        "assets/shaders/InfiniteGrid_GBuffer.glsl",
        "assets/shaders/ShadowDepth.glsl",
        "assets/shaders/ShadowDepthSkinned.glsl",
        "assets/shaders/DepthPrepass.glsl",
        "assets/shaders/DepthPrepass_Skinned.glsl",
        "assets/shaders/DepthPrepass_Mask.glsl",
        "assets/shaders/DepthPrepass_MaskSkinned.glsl",
        "assets/shaders/Terrain_PBR.glsl",
        "assets/shaders/Terrain_GBuffer.glsl",
        "assets/shaders/Terrain_Depth.glsl",
        "assets/shaders/Terrain_Voxel.glsl",
        "assets/shaders/Terrain_Voxel_GBuffer.glsl",
        "assets/shaders/Terrain_VoxelDepth.glsl",
        "assets/shaders/Terrain_VoxelGreedy.glsl",
        "assets/shaders/Terrain_VoxelGreedy_GBuffer.glsl",
        "assets/shaders/Terrain_VoxelGreedyDepth.glsl",
        "assets/shaders/Foliage_Instance.glsl",
        "assets/shaders/Foliage_Instance_GBuffer.glsl",
        "assets/shaders/Foliage_Depth.glsl",
        "assets/shaders/Foliage_Impostor.glsl",
        "assets/shaders/Impostor_Bake.glsl",
        "assets/shaders/Water.glsl",
        "assets/shaders/Water_Depth.glsl",
        "assets/shaders/Decal.glsl",
        "assets/shaders/Decal_OIT.glsl",
        "assets/shaders/Decal_GBuffer.glsl",
        "assets/shaders/Decal_GBuffer_Normal.glsl",
        "assets/shaders/Decal_GBuffer_RMA.glsl",
        "assets/shaders/Decal_GBuffer_Emissive.glsl",
        "assets/shaders/OcclusionProxy.glsl",
        "assets/shaders/ForwardPlusDebug.glsl",
    };
} // namespace

namespace OloEngine
{
    Renderer3D::Renderer3DData Renderer3D::s_Data;
    ShaderLibrary Renderer3D::m_ShaderLibrary;

    std::vector<std::string> Renderer3D::GetShaderFilepaths()
    {
        return std::vector<std::string>(kShaderPaths3D.begin(), kShaderPaths3D.end());
    }

    void Renderer3D::Init(Window* loadingWindow)
    {
        OLO_PROFILE_FUNCTION();

        // Idempotency guard. Init() may be reached twice on the editor's
        // OnAttach path: once from OpenProject()->ApplyPreferences() (with a
        // still-0x0 window framebuffer, so the render graph is only partially
        // built and IsInitialized() stays false) and once directly from
        // OnAttach(). Without this guard the second call re-runs
        // FrameDataBufferManager::Init() and trips its "already initialized"
        // assert. The deferred SetupRenderGraph in OnWindowResize() finishes the
        // graph build once a real framebuffer size arrives.
        if (s_Data.CoreInitialized)
        {
            OLO_CORE_TRACE("Renderer3D::Init called while already initialized — ignoring re-entry.");
            return;
        }
        s_Data.CoreInitialized = true;

        OLO_CORE_INFO("Initializing Renderer3D.");

        RendererProfiler::GetInstance().Initialize();
        GPUPassTimerPool::GetInstance().Initialize();

        // Query driver MSAA caps once so the settings panel and the
        // ApplyRendererSettings clamp logic have the true max the GPU
        // supports. We take the min of colour-attachment and depth-texture
        // caps because the G-Buffer needs matching sample counts on both.
        {
            s_Data.MaxMSAASamplesColor = std::max(RenderCommand::GetMaxColorTextureSamples(), 1u);
            s_Data.MaxMSAASamplesDepth = std::max(RenderCommand::GetMaxDepthTextureSamples(), 1u);
            OLO_CORE_INFO("Renderer3D: Driver MSAA caps — max colour-texture samples={}, "
                          "max depth-texture samples={} (usable max = {})",
                          s_Data.MaxMSAASamplesColor,
                          s_Data.MaxMSAASamplesDepth,
                          std::min(s_Data.MaxMSAASamplesColor, s_Data.MaxMSAASamplesDepth));
        }

        FrameDataBufferManager::Init();
        FrameResourceManager::Get().Init();

        // GPU-pushable shader debug-draw channels (issue #725). Allocated and
        // bound here, before any shader compiles, because a shader that includes
        // include/DebugDrawCommon.glsl reads each channel's Capacity as its
        // disabled-path guard — and reading an unbound SSBO is undefined in GL.
        // Header-only (7 x 32 bytes) until the feature is switched on.
        ShaderDebugDraw::Init();

        // The GPU readback-stats channel (issue #721). Same reason for being
        // here as the debug-draw channels above, and it is stronger: EVERY
        // shader that includes include/GPUReadbackStats.glsl opens its helpers by
        // reading `b_StatsEnabled` out of this block, so the buffer has to be
        // allocated and bound before the first such shader can run. 144 bytes,
        // resident for the process.
        GPUReadbackStats::Init();

        CommandDispatch::Initialize();
        OLO_CORE_INFO("CommandDispatch system initialized.");

        s_Data.CubeMesh = MeshPrimitives::CreateCube();
        // Icosphere at subdivision level 2 (320 triangles) — uniform triangle
        // distribution gives clean joint markers and is cheap enough to reuse
        // for every DrawSphere call.
        s_Data.SphereMesh = MeshPrimitives::CreateIcosphere(1.0f, 2);
        s_Data.QuadMesh = MeshPrimitives::CreatePlane(1.0f, 1.0f);
        s_Data.SkyboxMesh = MeshPrimitives::CreateSkyboxCube();
        // Cached unit line mesh — a perpendicular cross of two quads (XY plane + XZ plane),
        // both length 1 along +X with half-extent 0.5 on the cross axes. A single planar
        // quad would vanish when the camera's view direction is parallel to the quad's
        // normal (the typical "I rotated to look down an axis and the AABB disappeared"
        // failure). The cross has visible area from every direction perpendicular to the
        // line, at the cost of doubling vertex/index count per line draw — negligible for
        // debug gizmos. DrawLine scales both Y and Z by worldThickness so the cross-section
        // stays thin in world units.
        {
            std::vector<Vertex> verts;
            verts.reserve(8);
            // Quad 1: XY plane (thickness in Y)
            verts.emplace_back(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f));
            verts.emplace_back(glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(1.0f, 0.0f));
            verts.emplace_back(glm::vec3(1.0f, 0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(1.0f));
            verts.emplace_back(glm::vec3(1.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f, 1.0f));
            // Quad 2: XZ plane (thickness in Z)
            verts.emplace_back(glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(0.0f), glm::vec2(0.0f));
            verts.emplace_back(glm::vec3(0.0f, 0.0f, 0.5f), glm::vec3(0.0f), glm::vec2(1.0f, 0.0f));
            verts.emplace_back(glm::vec3(1.0f, 0.0f, 0.5f), glm::vec3(0.0f), glm::vec2(1.0f));
            verts.emplace_back(glm::vec3(1.0f, 0.0f, -0.5f), glm::vec3(0.0f), glm::vec2(0.0f, 1.0f));

            std::vector<u32> inds = {
                0, 1, 2, 2, 3, 0, // XY quad
                4, 5, 6, 6, 7, 4  // XZ quad
            };

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

        // NOTE: Keep totalShaders3D in sync with kShaderPaths3D's length above.
        constexpr u32 totalShaders3D = 52;
        static_assert(kShaderPaths3D.size() == totalShaders3D);

        // Boot + fallback are idempotent — no-ops when already initialized by
        // Renderer::Init().  Needed here for the lazy-init path (EditorLayer
        // calls Renderer3D::Init() directly without going through Renderer::Init).
        ShaderWarmup::Init();
        ShaderLibrary::InitFallbackShader();

        // A CI-baked pack (issue #908) is optional — LoadShaderPack() is a
        // no-op when the file doesn't exist, so every Load() below falls
        // back to compiling from source, same as it always has.
        m_ShaderLibrary.LoadShaderPack("assets/ShaderPack.osp");

        Window* window = loadingWindow;

        // All Load() calls issue glLinkProgram() back-to-back WITHOUT checking
        // GL_LINK_STATUS. When GL_ARB_parallel_shader_compile is available the
        // driver links them all in parallel. Status is checked later via
        // PollPendingShaders() each frame from `RenderPipeline::PrepareFrame()`.
        //
        // CPU-side compile of independent shaders runs in parallel across
        // shaders (issue #907) — GL program creation/link still happens
        // sequentially afterward on this (render) thread; see
        // ShaderWarmup::LoadShadersParallel.
        const std::vector<std::string> shaderPaths3D(kShaderPaths3D.begin(), kShaderPaths3D.end());
        ShaderWarmup::LoadShadersParallel(m_ShaderLibrary, window, shaderPaths3D, "3D shaders", 1);

        // Log how many shaders are still compiling asynchronously
        if (const u32 pending = m_ShaderLibrary.GetPendingCount(); pending > 0)
        {
            OLO_CORE_INFO("{} of {} shaders issued for async linking", pending, totalShaders3D);
        }

        // Display a loading screen with progress bar while shaders finish linking.
        // This blocks here until ALL shaders are Ready, keeping the window responsive.
        ShaderWarmup::RunWarmupScreen(m_ShaderLibrary, window);

        s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
        // The legacy single-light forward shaders (Lighting3D /
        // SkinnedLighting3D_Simple, binding-1 LightUBO) were retired. The
        // default/fallback forward shader for materials without an explicit
        // shader is now the multi-light PBR path (binding-5 MultiLightUBO).
        // These stay forward-only, so the Deferred-path overlay rerouting in
        // Renderer3DMeshSubmission still applies unchanged.
        s_Data.DefaultForwardShader = m_ShaderLibrary.Get("PBR_MultiLight");
        s_Data.DefaultForwardSkinnedShader = m_ShaderLibrary.Get("PBR_MultiLight_Skinned");
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
        s_Data.DepthPrepassShader = m_ShaderLibrary.Get("DepthPrepass");
        s_Data.DepthPrepassSkinnedShader = m_ShaderLibrary.Get("DepthPrepass_Skinned");
        s_Data.DepthPrepassMaskShader = m_ShaderLibrary.Get("DepthPrepass_Mask");
        s_Data.DepthPrepassMaskSkinnedShader = m_ShaderLibrary.Get("DepthPrepass_MaskSkinned");
        s_Data.TerrainPBRShader = m_ShaderLibrary.Get("Terrain_PBR");
        s_Data.TerrainGBufferShader = m_ShaderLibrary.Get("Terrain_GBuffer");
        s_Data.TerrainDepthShader = m_ShaderLibrary.Get("Terrain_Depth");
        s_Data.VoxelPBRShader = m_ShaderLibrary.Get("Terrain_Voxel");
        s_Data.VoxelGBufferShader = m_ShaderLibrary.Get("Terrain_Voxel_GBuffer");
        s_Data.VoxelDepthShader = m_ShaderLibrary.Get("Terrain_VoxelDepth");
        s_Data.VoxelGreedyPBRShader = m_ShaderLibrary.Get("Terrain_VoxelGreedy");
        s_Data.VoxelGreedyGBufferShader = m_ShaderLibrary.Get("Terrain_VoxelGreedy_GBuffer");
        s_Data.VoxelGreedyDepthShader = m_ShaderLibrary.Get("Terrain_VoxelGreedyDepth");
        s_Data.FoliageShader = m_ShaderLibrary.Get("Foliage_Instance");
        s_Data.FoliageGBufferShader = m_ShaderLibrary.Get("Foliage_Instance_GBuffer");
        s_Data.FoliageDepthShader = m_ShaderLibrary.Get("Foliage_Depth");
        s_Data.FoliageImpostorShader = m_ShaderLibrary.Get("Foliage_Impostor");
        s_Data.WaterShader = m_ShaderLibrary.Get("Water");
        s_Data.WaterDepthShader = m_ShaderLibrary.Get("Water_Depth");
        s_Data.DecalShader = m_ShaderLibrary.Get("Decal");
        s_Data.DecalGBufferShader = m_ShaderLibrary.Get("Decal_GBuffer");
        s_Data.DecalGBufferNormalShader = m_ShaderLibrary.Get("Decal_GBuffer_Normal");
        s_Data.DecalGBufferRMAShader = m_ShaderLibrary.Get("Decal_GBuffer_RMA");
        s_Data.DecalGBufferEmissiveShader = m_ShaderLibrary.Get("Decal_GBuffer_Emissive");
        s_Data.DecalCubeMesh = MeshPrimitives::CreateCube();
        s_Data.WhiteTexture = Texture2D::Create(TextureSpecification());
        u32 whiteTextureData = 0xffffffffU;
        s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(u32));

        // 1x1x1 white RGBA16F placeholder for TEX_LIGHTMAP (issue #868). The
        // lightmap sampler is a sampler2DArray, so the plain WhiteTexture can
        // no longer stand in for it. Owned by the renderer's Init/Shutdown pair
        // rather than a lazy static (lazy-static-release-ownership.md).
        // SetLayerData's client data is NATIVE per format, so RGBA16F wants
        // halves — 0x3C00 is 1.0h.
        {
            Texture2DArraySpecification placeholderSpec;
            placeholderSpec.Width = 1;
            placeholderSpec.Height = 1;
            placeholderSpec.Layers = 1;
            placeholderSpec.Format = Texture2DArrayFormat::RGBA16F;
            placeholderSpec.GenerateMipmaps = false;
            s_Data.LightmapPlaceholderAtlas = Texture2DArray::Create(placeholderSpec);
            if (s_Data.LightmapPlaceholderAtlas)
            {
                const std::array<u16, 4> whiteHalf{ 0x3C00u, 0x3C00u, 0x3C00u, 0x3C00u };
                s_Data.LightmapPlaceholderAtlas->SetLayerData(0, whiteHalf.data(), 1, 1);
            }
        }

        s_Data.SharedSceneUBOs.Camera = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
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
        s_Data.SceneGPU.InitializeGPU();
        // Ray tracing (#978). Installs the device backend when the active
        // backend has one; on every other backend this is a no-op that leaves
        // the scene reporting BackendNotVulkan, and nothing downstream
        // changes. Deliberately AFTER the GPU Scene it consumes.
        s_Data.SceneRT.Init();
        // GPU per-instance frustum culler — compute shader is lazy-loaded on
        // first cull dispatch so a stripped-down embedded build that doesn't
        // ship the compute shaders can still drive the CPU path.
        s_Data.GPUFrustumCuller = Ref<GPUFrustumCuller>::Create();
        // Persistent Hi-Z occlusion pyramid (#431). Loads the HZB compute
        // shader now (GL context is live); the pyramid texture is sized lazily
        // on the first GenerateOcclusionHZB() call from the scene dimensions.
        s_Data.OcclusionHZB.Initialize();
        s_Data.OcclusionHZB.SetReduceMode(HZBGenerator::ReduceMode::Max);
        s_Data.BoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION);
        s_Data.PrevBoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION_PREV);
        s_Data.TerrainUBO = UniformBuffer::Create(ShaderBindingLayout::TerrainUBO::GetSize(), ShaderBindingLayout::UBO_TERRAIN);
        s_Data.FoliageUBO = UniformBuffer::Create(ShaderBindingLayout::FoliageUBO::GetSize(), ShaderBindingLayout::UBO_FOLIAGE);
        s_Data.WaterUBO = UniformBuffer::Create(ShaderBindingLayout::WaterUBO::GetSize(), ShaderBindingLayout::UBO_WATER);
        s_Data.PostProcessGPU.PostProcess = UniformBuffer::Create(PostProcessUBOData::GetSize(), ShaderBindingLayout::UBO_USER_0);
        s_Data.PostProcessGPU.MotionBlur = UniformBuffer::Create(MotionBlurUBOData::GetSize(), ShaderBindingLayout::UBO_USER_1);
        s_Data.PostProcessGPU.SSAO = UniformBuffer::Create(SSAOUBOData::GetSize(), ShaderBindingLayout::UBO_SSAO);
        s_Data.PostProcessGPU.GTAO = UniformBuffer::Create(UBOStructures::GTAOUBO::GetSize(), ShaderBindingLayout::UBO_GTAO);
        s_Data.PostProcessGPU.SSR = UniformBuffer::Create(SSRUBOData::GetSize(), ShaderBindingLayout::UBO_SSR);
        s_Data.PostProcessGPU.SSGI = UniformBuffer::Create(SSGIUBOData::GetSize(), ShaderBindingLayout::UBO_SSGI);
        s_Data.PostProcessGPU.ContactShadow = UniformBuffer::Create(ContactShadowUBOData::GetSize(), ShaderBindingLayout::UBO_CONTACT_SHADOW);
        s_Data.SceneEffectsGPU.Snow = UniformBuffer::Create(SnowUBOData::GetSize(), ShaderBindingLayout::UBO_SNOW);
        s_Data.SceneEffectsGPU.SSS = UniformBuffer::Create(SSSUBOData::GetSize(), ShaderBindingLayout::UBO_SSS);
        s_Data.SceneEffectsGPU.Fog = UniformBuffer::Create(FogUBOData::GetSize(), ShaderBindingLayout::UBO_FOG);
        s_Data.SceneEffectsGPU.FogVolumes = UniformBuffer::Create(FogVolumesUBOData::GetSize(), ShaderBindingLayout::UBO_FOG_VOLUMES);
        s_Data.SceneEffectsGPU.FogVolumesData = FogVolumesUBOData{};
        s_Data.DecalUBO = UniformBuffer::Create(ShaderBindingLayout::DecalUBO::GetSize(), ShaderBindingLayout::UBO_DECAL);
        s_Data.LightProbeVolumeUBO = UniformBuffer::Create(ShaderBindingLayout::LightProbeVolumeUBO::GetSize(), ShaderBindingLayout::UBO_LIGHT_PROBES);
        s_Data.LightmapUBO = UniformBuffer::Create(ShaderBindingLayout::LightmapUBO::GetSize(), ShaderBindingLayout::UBO_LIGHTMAP);
        s_Data.LightmapUBOUploaded = false; // fresh buffer — the dirty guard must not skip the first upload
        s_Data.SceneEffectsGPU.DRS = UniformBuffer::Create(DRSUBOData::GetSize(), ShaderBindingLayout::UBO_DRS);
        s_Data.UnderwaterFogBuffer = UniformBuffer::Create(UnderwaterFogUBOData::GetSize(), ShaderBindingLayout::UBO_UNDERWATER);

        // Initialize light probe UBO with disabled state and create a small zeroed SSBO
        // so shaders always have valid bindings at SSBO_LIGHT_PROBES
        {
            ShaderBindingLayout::LightProbeVolumeUBO disabledProbeUBO{};
            s_Data.LightProbeVolumeUBO->SetData(&disabledProbeUBO, ShaderBindingLayout::LightProbeVolumeUBO::GetSize());
            // Same guarantee for the lightmap block (issue #439): a renderer that
            // never sees a baked scene reads Enabled == 0, never garbage.
            ShaderBindingLayout::LightmapUBO disabledLightmapUBO{};
            s_Data.LightmapUBO->SetData(&disabledLightmapUBO, ShaderBindingLayout::LightmapUBO::GetSize());
            constexpr u32 dummySSBOSize = 16; // Minimum valid SSBO (one vec4)
            s_Data.LightProbeSHBuffer = StorageBuffer::Create(dummySSBOSize, ShaderBindingLayout::SSBO_LIGHT_PROBES);
            std::array<u8, dummySSBOSize> zeros{};
            s_Data.LightProbeSHBuffer->SetData(zeros.data(), dummySSBOSize);
        }

        CommandDispatch::SetUBOReferences(
            s_Data.SharedSceneUBOs.Camera,
            s_Data.SharedSceneUBOs.Material,
            s_Data.BoneMatricesUBO,
            s_Data.ModelInstanceBuffer,
            s_Data.PrevBoneMatricesUBO,
            &s_Data.ForwardPlus);

        EnvironmentMap::InitializeIBLSystem(m_ShaderLibrary);
        OLO_CORE_INFO("IBL system initialized.");

        s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);

        s_Data.Stats.Reset();

        s_Data.RGraph = Ref<RenderGraph>::Create();
        RenderGraphDebugRuntime::SetActiveGraph(s_Data.RGraph);

        // Flipping the aliasing lever has to evict the transient pool, or the
        // A/B compares a mixed state: objects acquired under the previous policy
        // are still bucketed and get handed straight back out under the new one.
        // `olo_render_debug_set` has always done this inline, which is why only
        // THAT path was correct — a console line, `--set` or any other write
        // left the stale buckets in place.
        //
        // A change callback fixes it for every path at once, and this is the
        // shape those callbacks are meant to have: it reads the CURRENT value
        // rather than a delta, and it runs on the game thread at the top of a
        // frame, which is the only place evicting GPU objects is safe.
        //
        // Function-local static: registered once for the process, not once per
        // Renderer3D::Init, and it looks the active graph up each time so it
        // survives a shutdown/init cycle. invokeNow is false because a freshly
        // constructed pool has nothing to evict.
        static const CVars::CallbackHandle s_AliasingChanged = CVars::AddChangeCallback(
            "OLO_RG_DISABLE_ALIASING",
            [](const CVars::CVarInfo&)
            {
                // Own (non-const) Ref: Ref<T> propagates constness through
                // operator->, and GetActiveGraph() returns a const Ref.
                if (Ref<RenderGraph> graph = RenderGraphDebugRuntime::GetActiveGraph(); graph)
                {
                    graph->GetTransientPool().Clear();
                }
            },
            /*invokeNow*/ false);
        (void)s_AliasingChanged;
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

        // Initialize the water-disturbance (boat wake foam) field, issue #967.
        // Owned by Renderer3D's lifecycle, not Renderer3D's 3D-draw path, so it
        // is released by the matching Shutdown below — a shared lazy static
        // released from a narrower scope leaks in every session that never
        // reaches that scope (docs/agent-rules/lazy-static-release-ownership.md).
        WaterDisturbanceSystem::Init();
        // Seabed depth field for shore wave deformation (issue #1033).
        WaterShoreDepthSystem::Init();
        // Crest spray particles (issue #1034, §2.3). Owned here for the same
        // reason the disturbance field is: a pool released from a narrower
        // scope leaks in every session that never reaches that scope.
        WaterSpraySystem::Init();

        // Initialize snow accumulation & ejecta systems
        SnowAccumulationSystem::Init();
        SnowEjectaSystem::Init(s_Data.SnowEjecta.MaxParticles);

        // Initialize precipitation system (use default max particle counts from settings)
        PrecipitationSystem::Init(s_Data.Precipitation.MaxParticlesNearField,
                                  s_Data.Precipitation.MaxParticlesFarField);
        ScreenSpacePrecipitation::Init();

        // Initialize the mockable per-frame dt trackers (see
        // RenderPipeline.cpp's SampleMockableDt). Seeded from the CURRENT
        // clock — the mock when one is already installed — so the first
        // frame's dt is 0 rather than a wall-clock-dependent clamp.
        s_Data.FogFrameIndex = 0;
        s_Data.FogPrevTimeSeconds = Time::GetTime();
        s_Data.AutoExposurePrevTimeSeconds = Time::GetTime();
        s_Data.WindPrevTimeSeconds = Time::GetTime();
        s_Data.FogTime = 0.0f;

        // Initialize Forward+ light culling system
        s_Data.ForwardPlus.Initialize(fbWidth, fbHeight);

        // Distance-impostor reflection probe arrays (issue #705)
        s_Data.ReflectionProbes.Init();

        OLO_CORE_INFO("Renderer3D initialization complete.");
    }

    bool Renderer3D::IsInitialized()
    {
        return s_Data.RGraph != nullptr && s_Data.Pipeline && s_Data.Pipeline->FrameCorePasses.Scene != nullptr;
    }

    bool Renderer3D::HasInitialized()
    {
        return s_Data.CoreInitialized;
    }

    std::vector<std::string> Renderer3D::DebugLiveGpuOwningStatics()
    {
        std::vector<std::string> live;
        auto note = [&live](const char* name, bool alive)
        {
            if (alive)
            {
                live.emplace_back(name);
            }
        };

        // Every s_Data member that owns GPU memory and must be released by Shutdown().
        // GPUFrustumCuller is listed FIRST because it is the one that was missing: it
        // outlived Shutdown, was destroyed at static-destruction time, and segfaulted the
        // process on exit after a clean-looking pass.
        note("GPUFrustumCuller", s_Data.GPUFrustumCuller != nullptr);
        note("RGraph", s_Data.RGraph != nullptr);
        note("MultiLightBuffer", s_Data.MultiLightBuffer != nullptr);
        note("ModelInstanceBuffer", s_Data.ModelInstanceBuffer != nullptr);
        note("GPUScene", s_Data.SceneGPU.HasGPUResources());
        note("BoneMatricesUBO", s_Data.BoneMatricesUBO != nullptr);
        note("TerrainUBO", s_Data.TerrainUBO != nullptr);
        note("FoliageUBO", s_Data.FoliageUBO != nullptr);
        note("WaterUBO", s_Data.WaterUBO != nullptr);
        note("DecalReceiverIntersectionQueries", s_Data.DecalReceiverIntersectionQueries[0].IsValid());
        note("DecalVisibilityQueries", s_Data.DecalVisibilityQueries[0].IsValid());
        // Reflection-probe cubemap arrays + UBO + cluster-mask SSBO (#705).
        note("ReflectionProbeArray", s_Data.ReflectionProbes.IsInitialized());

        return live;
    }

    void Renderer3D::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Shutting down Renderer3D.");

        // Flush any shaders still compiling asynchronously
        m_ShaderLibrary.FlushPendingShaders();

        ParticleBatchRenderer::Shutdown();

        if (s_Data.DecalVisibilityQueriesInitialized)
        {
            RenderCommand::DeleteQueries(s_Data.DecalReceiverIntersectionQueries);
            s_Data.DecalReceiverIntersectionQueries = {};
            RenderCommand::DeleteQueries(s_Data.DecalVisibilityQueries);
            s_Data.DecalVisibilityQueries = {};
            s_Data.DecalVisibilityFrames = {};
            s_Data.DecalVisibilityQueriesInitialized = false;
            s_Data.DecalVisibilityTarget = -1;
            s_Data.LastDecalVisibilityEntity = -1;
            s_Data.LastDecalVisibilityObservation = {};
        }

        // Shutdown occlusion culling systems
        OcclusionCuller::GetInstance().Shutdown();
        OcclusionQueryPool::GetInstance().Shutdown();
        OcclusionStateManager::GetInstance().Clear();
        // Release the persistent Hi-Z occlusion pyramid (#431).
        s_Data.OcclusionHZB.Shutdown();
        s_Data.OcclusionHZBValid = false;

        // Release the debug-draw channels + their readback staging (#725) while
        // GL is alive.
        ShaderDebugDraw::Shutdown();

        // Release the readback-stats SSBO, its staging ring and any fence still
        // in flight (#721) while GL is alive. A pending ring slot holds a live
        // sync object, and destroying it after the context is gone is the
        // lazy-static-release trap documented in
        // docs/agent-rules/lazy-static-release-ownership.md.
        GPUReadbackStats::Shutdown();

        // Release the terrain GPU-LOD patch mesh (#714) while GL is alive — it
        // is a process-wide static, so its Ref would otherwise run
        // glDeleteVertexArrays at process exit against a dead context.
        TerrainGPUQuadtree::ReleaseSharedPatchMesh();

        // Release the virtualized-geometry GPU pools (#629) while GL is alive.
        VirtualMeshRegistry::Get().Shutdown();
        VirtualGeometryShadow::Shutdown();

        // Shutdown wind system
        WindSystem::Shutdown();

        // Shutdown volumetric cloudscape systems (issue #633) and the shared
        // media self-shadowing volume (issue #723 — before CloudNoise, which
        // owns the field the generator marches).
        VolumetricShadowMap::Shutdown();
        CloudShadowMap::Shutdown();
        CloudNoise::Shutdown();

        // Release the shared ocean FFT params UBO (#691) while the
        // graphics device is still valid.
        Ocean::OceanFFTGpu::ShutdownSharedResources();

        // Shutdown precipitation system
        ScreenSpacePrecipitation::Shutdown();
        PrecipitationSystem::Shutdown();

        // Shutdown snow systems
        SnowEjectaSystem::Shutdown();
        SnowAccumulationSystem::Shutdown();

        // Water-disturbance field (issue #967)
        WaterDisturbanceSystem::Shutdown();
        WaterShoreDepthSystem::Shutdown();
        WaterSpraySystem::Shutdown();

        // Shutdown Forward+ system
        s_Data.ForwardPlus.Shutdown();

        // Shutdown the reflection-probe arrays (issue #705)
        s_Data.ReflectionProbes.Shutdown();

        // Shutdown shadow mapping
        s_Data.Shadow.Shutdown();
        // Release the static placeholder shadow textures lazy-initialised on
        // first bind-when-real-shadow-absent. Idempotent.
        ShadowMap::ShutdownPlaceholders();

        // Clear any pending GPU resource commands
        GPUResourceQueue::Clear();

        // Reset the mockable per-frame dt trackers + fog temporal state
        s_Data.FogFrameIndex = 0;
        s_Data.FogPrevTimeSeconds = 0.0f;
        s_Data.AutoExposurePrevTimeSeconds = 0.0f;
        s_Data.WindPrevTimeSeconds = 0.0f;
        s_Data.FogTime = 0.0f;

        // Reset cloudscape state + wind-advection accumulators (issue #633)
        s_Data.Cloudscape = {};
        s_Data.CloudWindOffset = glm::vec2(0.0f);
        s_Data.CloudTime = 0.0f;
        s_Data.CloudFrameIndex = 0;
        s_Data.CloudPrevTimeSeconds = 0.0f;

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
        // Ray tracing before the GPU Scene it keys off, so no acceleration
        // structure outlives the records that named it.
        s_Data.SceneRT.Shutdown();
        s_Data.SceneGPU.Shutdown();
        s_Data.GPUSceneExtractionActive = false;
        // The two-phase GPU culler (#431) owns a pool of StorageBuffers / InstanceBuffers.
        // It was created in Init but never released here, so it outlived Shutdown and was
        // destroyed during STATIC destruction at process exit — by which time the Meyer's
        // singletons its buffers' destructors call (FrameResourceManager,
        // RendererMemoryTracker, GPUResourceInspector) are already gone. That segfaulted the
        // test binary on the way out (exit 139), after every result had printed, so it looked
        // like a passing run. Only the occlusion tests populate the pool, which is why it took
        // a suite-wide bisect to find. Pinned by RendererShutdownTest.
        s_Data.GPUFrustumCuller.Reset();
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
        s_Data.UnderwaterFogBuffer.Reset();

        // The primitive meshes are the same static-outlives-the-context shape
        // as the GPUFrustumCuller above: each MeshSource holds a main + shadow
        // vertex array whose VMA allocations must die before the window
        // destroys the graphics context, or vmaDestroyAllocator aborts with
        // "allocations not freed" on Vulkan (#691, the close-button
        // crash — found via the surviving-VertexArray teardown dump).
        s_Data.CubeMesh.Reset();
        s_Data.SphereMesh.Reset();
        s_Data.QuadMesh.Reset();
        s_Data.SkyboxMesh.Reset();
        s_Data.DecalCubeMesh.Reset();
        s_Data.LineQuadMesh.Reset();
        s_Data.FullscreenQuadVAO.Reset();
        s_Data.WhiteTexture.Reset();
        s_Data.LightmapPlaceholderAtlas.Reset();

        // CommandDispatch's static mirror of the shared UBO / instance-buffer
        // Refs (SetUBOReferences) is a CO-OWNER: without this, the
        // ModelInstanceBuffer's storage survives to static destruction and
        // trips the same vmaDestroyAllocator abort as the meshes above.
        CommandDispatch::Shutdown();

        // Shaders: the static library plus s_Data's named aliases. Shaders
        // surviving to static destruction leak their VkShaderModules into
        // vkDestroyDevice (VUID-vkDestroyDevice-device-05137).
        s_Data.LightCubeShader.Reset();
        s_Data.DefaultForwardShader.Reset();
        s_Data.DefaultForwardSkinnedShader.Reset();
        s_Data.QuadShader.Reset();
        s_Data.PBRShader.Reset();
        s_Data.PBRSkinnedShader.Reset();
        s_Data.PBRMultiLightShader.Reset();
        s_Data.PBRMultiLightSkinnedShader.Reset();
        s_Data.PBRGBufferShader.Reset();
        s_Data.PBRGBufferSkinnedShader.Reset();
        s_Data.SkyboxShader.Reset();
        s_Data.SkyboxGBufferShader.Reset();
        s_Data.LightCubeGBufferShader.Reset();
        s_Data.InfiniteGridShader.Reset();
        s_Data.InfiniteGridGBufferShader.Reset();
        s_Data.ForwardPlusDebugShader.Reset();
        s_Data.ShadowDepthShader.Reset();
        s_Data.ShadowDepthSkinnedShader.Reset();
        s_Data.DepthPrepassShader.Reset();
        s_Data.DepthPrepassSkinnedShader.Reset();
        s_Data.DepthPrepassMaskShader.Reset();
        s_Data.DepthPrepassMaskSkinnedShader.Reset();
        s_Data.TerrainPBRShader.Reset();
        s_Data.TerrainGBufferShader.Reset();
        s_Data.TerrainDepthShader.Reset();
        s_Data.VoxelPBRShader.Reset();
        s_Data.VoxelGBufferShader.Reset();
        s_Data.VoxelDepthShader.Reset();
        s_Data.VoxelGreedyPBRShader.Reset();
        s_Data.VoxelGreedyGBufferShader.Reset();
        s_Data.VoxelGreedyDepthShader.Reset();
        s_Data.FoliageShader.Reset();
        s_Data.FoliageGBufferShader.Reset();
        s_Data.FoliageDepthShader.Reset();
        s_Data.FoliageImpostorShader.Reset();
        s_Data.WaterShader.Reset();
        s_Data.WaterDepthShader.Reset();
        s_Data.DecalShader.Reset();
        s_Data.DecalGBufferShader.Reset();
        s_Data.DecalGBufferNormalShader.Reset();
        s_Data.DecalGBufferRMAShader.Reset();
        s_Data.DecalGBufferEmissiveShader.Reset();
        // ParallelSceneContext caches its OWN Ref<Shader> copies for the
        // worker-thread submission path — the co-owner the teardown
        // ref-holder scan found at s_Data.ParallelContext (the five
        // "surviving shader" traces on every close).
        s_Data.ParallelContext = ParallelSceneContext{};
        m_ShaderLibrary.Clear();
        ShaderLibrary::ShutdownFallbackShader();

        IBLPrecompute::Shutdown();
        ImpostorBaker::Shutdown();
        // MeshPrimitives is deliberately NOT released here — it is not a 3D-only
        // facility. Its fullscreen-triangle cache is first created from the 2D /
        // warmup path (Renderer2D::Init -> ShaderWarmup::RenderProgressFrame ->
        // GetFullscreenTriangle), so releasing it from a teardown that only runs
        // `if (Renderer3D::HasInitialized())` leaked it in every session that never
        // brought 3D up — the OloRuntime start-scene-missing path is exactly that
        // (#814). Renderer::Shutdown owns it now: the one teardown that always runs.
        // See docs/agent-rules/lazy-static-release-ownership.md.

        FrameResourceManager::Get().Shutdown();
        FrameDataBufferManager::Shutdown();

        // Boot/fallback shader shutdown is handled by Renderer::Shutdown()
        // (both are idempotent, but no need to call them twice).

        GPUPassTimerPool::GetInstance().Shutdown();
        // Its neighbour, and a different species of the same bug (#839): this pool has
        // had a correct Shutdown() since it was written and NOTHING called it. Grepping
        // "is this released?" finds the function and stops; the question that finds the
        // leak is "who calls it?". CommandBucket::ExecuteWithGPUTiming lazily
        // Initialize()s it on the first GPU-timed frame from SceneRenderPass, so it is
        // 3D-only by construction and Renderer3D::Shutdown() is unconditional for every
        // session that can create it. See docs/agent-rules/lazy-static-release-ownership.md.
        GPUTimerQueryPool::GetInstance().Shutdown();
        RendererProfiler::GetInstance().Shutdown();

        s_Data.CoreInitialized = false;

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

        // The virtualized-geometry registry caches a BAKED cluster LOD DAG (its own copy of
        // the vertex data) keyed by AssetHandle, and its callers only register a mesh they
        // have not seen before — so a reloaded MeshSource would otherwise keep rendering the
        // pre-edit geometry for the whole process, self-consistently and with nothing to trip
        // a validation check. Drop it; the next submission re-cooks from the new source.
        if (type == AssetType::MeshSource)
        {
            VirtualMeshRegistry::Get().Invalidate(handle);
            // Buffer identities and ranges can change under the same asset
            // handle. Retain generations while invalidating every live slot.
            ResetGPUScene();
        }

        // Commands are rebuilt each frame from Material/Mesh objects, so no
        // per-command patching is needed here — command buckets are cleared and
        // rebuilt every frame (stale RendererIDs survive at most one frame) and the
        // Material/Render-state tables in FrameDataBuffer are rebuilt each frame via
        // Reset(). What matters is that the Ref<T> a Material/Mesh/s_Data cache holds
        // ends up seeing the refreshed GPU resource on the NEXT frame's DrawMesh() /
        // DrawAnimatedMesh(). That holds only for the reload paths that refresh the
        // resource *in place* (same object identity behind the existing Ref):
        //  - Shaders: ShaderLibrary::ReloadShaders() calls OpenGLShader::Reload(),
        //    which recompiles onto the same Shader object.
        //  - Textures: EditorAssetManager::ReloadData() refreshes a Texture2D in place
        //    via Texture2D::Reload (issue #544) rather than swapping in a new
        //    object, so a Material's Ref<Texture2D> members pick up the new pixels.
        //
        // Asset types that ReloadData still *replaces* with a brand-new object (meshes,
        // etc.) are NOT covered by this: a consumer caching the old Ref keeps the
        // pre-edit object until it re-resolves the handle. Extending in-place reload (or
        // handle re-resolution) to those types is future work.
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
            if (Levers::RenderGraphDiagnostics())
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
