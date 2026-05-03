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

#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Asset/AssetManager.h"

#include <chrono>
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
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

namespace OloEngine
{
    static bool s_ForceDisableCulling = false;

    static auto ValidateDrawMeshRendererIDs(const char* context, const u32 vaoID, const u32 shaderID) -> bool
    {
        if (vaoID != 0 && shaderID != 0)
            return true;

        static std::atomic<u64> s_InvalidRendererIDWarnCount{ 0 };
        if (s_InvalidRendererIDWarnCount.fetch_add(1, std::memory_order_relaxed) < 1)
        {
            OLO_CORE_WARN("{}: Dropping draw with invalid renderer IDs (VAO={}, Shader={})",
                          context, vaoID, shaderID);
        }

        return false;
    }

    Renderer3D::Renderer3DData Renderer3D::s_Data;
    ShaderLibrary Renderer3D::m_ShaderLibrary;

    // @brief Identifies shaders that are safe to submit into the Deferred
    // ScenePass (i.e. write the G-Buffer MRT layout: Albedo/Metallic,
    // Normal/Roughness/AO, Emissive/Flags, Velocity). Any shader outside
    // this set uses the forward fragment layout and would alias its outputs
    // onto the G-Buffer slots - corrupting lighting for every subsequent
    // pixel in the frame. `material.GetShader()` overrides are checked
    // through this helper at every serial/parallel submission site so a
    // forward-only custom shader on a deferred-path draw is transparently
    // rerouted to ForwardOverlayPass (see the `overlayRoute` branches).
    bool Renderer3D::IsDeferredCapableShader(const Ref<Shader>& shader)
    {
        if (!shader)
            return false;
        const u64 handle = static_cast<u64>(shader->GetHandle());
        if (handle == 0)
            return false;

        // Primary path: ask the shader itself. `Shader::IsDeferredCapable()`
        // is populated by the backend's reflection pass (for OpenGL, at
        // Reflect() time by scanning the fragment stage's declared outputs
        // for the G-Buffer marker names — see `OpenGLShader::Reflect`).
        // This correctly classifies CUSTOM shaders that weren't loaded via
        // the built-in handle set, as long as they follow the engine's
        // opt-in naming convention (o_GBuffer* / gAlbedo / gNormalRoughAO /
        // gEmissive).
        if (shader->IsDeferredCapable())
            return true;

        // Compatibility shim: some built-in shaders are cached in s_Data
        // and may be queried before their reflection has run (e.g. during
        // engine startup, before the first Bind()/EnsureLinked()). Fall
        // back to identity comparison against the known deferred handle
        // set so those queries don't misclassify a not-yet-reflected shader
        // as forward-only.
        //
        // Compare by AssetHandle rather than RendererID — the latter is
        // re-issued on hot-reload whereas the handle is stable across the
        // asset lifetime.
        auto matches = [handle](const Ref<Shader>& candidate)
        {
            return candidate && static_cast<u64>(candidate->GetHandle()) == handle;
        };
        return matches(s_Data.PBRGBufferShader) || matches(s_Data.PBRGBufferSkinnedShader) ||
               matches(s_Data.SkyboxGBufferShader) || matches(s_Data.LightCubeGBufferShader) ||
               matches(s_Data.InfiniteGridGBufferShader) || matches(s_Data.TerrainGBufferShader) ||
               matches(s_Data.VoxelGBufferShader) || matches(s_Data.FoliageGBufferShader) ||
               matches(s_Data.DecalGBufferShader) || matches(s_Data.DecalGBufferNormalShader) ||
               matches(s_Data.DecalGBufferRMAShader) || matches(s_Data.DecalGBufferEmissiveShader);
    }

    // Halton low-discrepancy sequence used for TAA sub-pixel jitter. Index is
    // 1-based (index 0 is undefined for Halton); the sequence repeats every
    // kHaltonSequenceLength samples which is long enough to de-correlate the
    // jitter pattern from any typical framerate / scene loop.
    static constexpr u32 kHaltonSequenceLength = 8;
    static f32 HaltonSample(u32 index, u32 base)
    {
        f32 f = 1.0f;
        f32 r = 0.0f;
        while (index > 0)
        {
            f /= static_cast<f32>(base);
            r += f * static_cast<f32>(index % base);
            index /= base;
        }
        return r;
    }

    // Helper function to compute depth from camera space for sort key
    // Returns a quantized depth value in range [0, 0xFFFFFF] for 24-bit depth
    // @param modelMatrix The model transformation matrix
    // @param boundingSphereCenter Optional world-space bounding sphere center. If provided,
    //        uses this for more accurate depth sorting for off-center meshes. If nullptr,
    //        falls back to using modelMatrix[3] (the origin of the transformed object).
    static u32 ComputeDepthForSortKeyWithView(const glm::mat4& modelMatrix, const glm::mat4& viewMatrix, const glm::vec3* boundingSphereCenter = nullptr)
    {

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

    static u32 ComputeDepthForSortKey(const glm::mat4& modelMatrix, const glm::vec3* boundingSphereCenter = nullptr)
    {
        return ComputeDepthForSortKeyWithView(modelMatrix, CommandDispatch::GetViewMatrix(), boundingSphereCenter);
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

    // Helper to populate POD material data from Material object
    static PODMaterialData CreatePODMaterialDataForMaterial(const Material& material, RendererID shaderRendererID)
    {
        PODMaterialData data{};
        data.shaderRendererID = shaderRendererID;

        // Legacy material properties
        data.ambient = material.GetAmbient();
        data.diffuse = material.GetDiffuse();
        data.specular = material.GetSpecular();
        data.shininess = material.GetShininess();
        data.useTextureMaps = material.IsUsingTextureMaps();
        data.diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRendererID() : 0;
        data.specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRendererID() : 0;

        // PBR material properties
        data.enablePBR = (material.GetType() == MaterialType::PBR);
        data.baseColorFactor = material.GetBaseColorFactor();
        data.emissiveFactor = material.GetEmissiveFactor();
        data.metallicFactor = material.GetMetallicFactor();
        data.roughnessFactor = material.GetRoughnessFactor();
        data.normalScale = material.GetNormalScale();
        data.occlusionStrength = material.GetOcclusionStrength();
        data.enableIBL = material.IsIBLEnabled();

        // PBR texture renderer IDs
        data.albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRendererID() : 0;
        data.metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRendererID() : 0;
        data.normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRendererID() : 0;
        data.aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRendererID() : 0;
        data.emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRendererID() : 0;
        data.environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRendererID() : 0;
        data.irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRendererID() : 0;
        data.prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRendererID() : 0;
        data.brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRendererID() : 0;

        // Fall back to global IBL when the material has no IBL configured
        if (data.enablePBR && data.irradianceMapID == 0 && Renderer3D::GetGlobalIrradianceMapID() != 0)
        {
            data.irradianceMapID = Renderer3D::GetGlobalIrradianceMapID();
            data.prefilterMapID = Renderer3D::GetGlobalPrefilterMapID();
            data.brdfLutMapID = Renderer3D::GetGlobalBRDFLutMapID();
            if (data.environmentMapID == 0)
                data.environmentMapID = Renderer3D::GetGlobalEnvironmentMapID();
            data.enableIBL = true;
            data.iblIntensity = Renderer3D::GetGlobalIBLIntensity();
        }

        return data;
    }

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

        u32 shaderIdx = 0;
        // NOTE: Keep totalShaders3D in sync with the number of Load() calls below.
        constexpr u32 totalShaders3D = 42;

        // Boot + fallback are idempotent — no-ops when already initialized by
        // Renderer::Init().  Needed here for the lazy-init path (EditorLayer
        // calls Renderer3D::Init() directly without going through Renderer::Init).
        ShaderWarmup::Init();
        ShaderLibrary::InitFallbackShader();

        Window* window = loadingWindow;

        // All Load() calls issue glLinkProgram() back-to-back WITHOUT checking
        // GL_LINK_STATUS. When GL_ARB_parallel_shader_compile is available the
        // driver links them all in parallel. Status is checked later via
        // PollPendingShaders() each frame in BeginSceneCommon().
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
            "assets/shaders/Water_OIT.glsl",
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

        s_Data.CameraUBO = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        s_Data.LightPropertiesUBO = UniformBuffer::Create(ShaderBindingLayout::LightUBO::GetSize(), ShaderBindingLayout::UBO_LIGHTS);
        // Allocate enough for the larger PBR layout (PBRMaterialUBO > MaterialUBO)
        constexpr u32 materialBufferSize = std::max(ShaderBindingLayout::MaterialUBO::GetSize(), ShaderBindingLayout::PBRMaterialUBO::GetSize());
        s_Data.MaterialUBO = UniformBuffer::Create(materialBufferSize, ShaderBindingLayout::UBO_MATERIAL);
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

        s_Data.ModelMatrixUBO = UniformBuffer::Create(ShaderBindingLayout::ModelUBO::GetSize(), ShaderBindingLayout::UBO_MODEL);
        s_Data.BoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION);
        s_Data.PrevBoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION_PREV);
        s_Data.TerrainUBO = UniformBuffer::Create(ShaderBindingLayout::TerrainUBO::GetSize(), ShaderBindingLayout::UBO_TERRAIN);
        s_Data.FoliageUBO = UniformBuffer::Create(ShaderBindingLayout::FoliageUBO::GetSize(), ShaderBindingLayout::UBO_FOLIAGE);
        s_Data.WaterUBO = UniformBuffer::Create(ShaderBindingLayout::WaterUBO::GetSize(), ShaderBindingLayout::UBO_WATER);
        s_Data.PostProcessUBO = UniformBuffer::Create(PostProcessUBOData::GetSize(), ShaderBindingLayout::UBO_USER_0);
        s_Data.MotionBlurUBO = UniformBuffer::Create(MotionBlurUBOData::GetSize(), ShaderBindingLayout::UBO_USER_1);
        s_Data.SSAOUBO = UniformBuffer::Create(SSAOUBOData::GetSize(), ShaderBindingLayout::UBO_SSAO);
        s_Data.GTAOUBO = UniformBuffer::Create(UBOStructures::GTAOUBO::GetSize(), ShaderBindingLayout::UBO_GTAO);
        s_Data.SnowUBO = UniformBuffer::Create(SnowUBOData::GetSize(), ShaderBindingLayout::UBO_SNOW);
        s_Data.SSSUBO = UniformBuffer::Create(SSSUBOData::GetSize(), ShaderBindingLayout::UBO_SSS);
        s_Data.FogUBO = UniformBuffer::Create(FogUBOData::GetSize(), ShaderBindingLayout::UBO_FOG);
        s_Data.FogVolumesUBO = UniformBuffer::Create(FogVolumesUBOData::GetSize(), ShaderBindingLayout::UBO_FOG_VOLUMES);
        s_Data.FogVolumesGPUData = FogVolumesUBOData{};
        s_Data.DecalUBO = UniformBuffer::Create(ShaderBindingLayout::DecalUBO::GetSize(), ShaderBindingLayout::UBO_DECAL);
        s_Data.LightProbeVolumeUBO = UniformBuffer::Create(ShaderBindingLayout::LightProbeVolumeUBO::GetSize(), ShaderBindingLayout::UBO_LIGHT_PROBES);

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
            s_Data.CameraUBO,
            s_Data.MaterialUBO,
            s_Data.LightPropertiesUBO,
            s_Data.BoneMatricesUBO,
            s_Data.ModelMatrixUBO,
            s_Data.PrevBoneMatricesUBO);

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
        return s_Data.RGraph != nullptr && s_Data.ScenePass != nullptr;
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

        // Clear any pending GPU resource commands
        GPUResourceQueue::Clear();

        // Clear shader registries
        s_Data.ShaderRegistries.clear();

        // Reset fog temporal state
        s_Data.FogFrameIndex = 0;
        s_Data.FogLastTime = {};
        s_Data.FogTime = 0.0f;

        if (s_Data.RGraph)
            s_Data.RGraph->Shutdown();

        // Release all render passes now while the GL context and RendererAPI are still alive.
        // Their destructors call RenderCommand::DeleteTexture() which needs s_RendererAPI.
        s_Data.ShadowPass.Reset();
        s_Data.ScenePass.Reset();
        s_Data.SSAOPass.Reset();
        s_Data.GTAOPass.Reset();
        s_Data.ParticlePass.Reset();
        s_Data.OITResolvePass.Reset();
        s_Data.SSSPass.Reset();
        s_Data.AOApplyPass.Reset();
        s_Data.PostProcessPass.Reset();
        s_Data.BloomPass.Reset();
        s_Data.DOFPass.Reset();
        s_Data.MotionBlurPass.Reset();
        s_Data.TAAPass.Reset();
        s_Data.PrecipitationPass.Reset();
        s_Data.FogPass.Reset();
        s_Data.ChromAberrationPass.Reset();
        s_Data.ColorGradingPass.Reset();
        s_Data.ToneMapPass.Reset();
        s_Data.VignettePass.Reset();
        if (s_Data.FXAAPass)
        {
            s_Data.FXAAPass.Reset();
        }
        if (s_Data.SelectionOutlinePass)
        {
            s_Data.SelectionOutlinePass.Reset();
        }
        s_Data.UICompositePass.Reset();
        s_Data.FinalPass.Reset();
        s_Data.FoliagePass.Reset();
        s_Data.WaterPass.Reset();
        s_Data.DecalPass.Reset();
        s_Data.DeferredLightPass.Reset();
        s_Data.OpaqueDecalPass.Reset();
        s_Data.ForwardOverlayPass.Reset();
        s_Data.RGraph.Reset();

        // Release UBOs explicitly while the GL context is still alive
        s_Data.CameraUBO.Reset();
        s_Data.LightPropertiesUBO.Reset();
        s_Data.MaterialUBO.Reset();
        s_Data.MultiLightBuffer.Reset();
        s_Data.ModelMatrixUBO.Reset();
        s_Data.BoneMatricesUBO.Reset();
        s_Data.TerrainUBO.Reset();
        s_Data.FoliageUBO.Reset();
        s_Data.WaterUBO.Reset();
        s_Data.PostProcessUBO.Reset();
        s_Data.MotionBlurUBO.Reset();
        s_Data.SSAOUBO.Reset();
        s_Data.SnowUBO.Reset();
        s_Data.SSSUBO.Reset();
        s_Data.FogUBO.Reset();
        s_Data.FogVolumesUBO.Reset();
        s_Data.DecalUBO.Reset();
        s_Data.LightProbeVolumeUBO.Reset();
        s_Data.LightProbeSHBuffer.Reset();
        s_Data.PrevBoneMatricesUBO.Reset();

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

    void Renderer3D::BeginSceneCommon()
    {
        OLO_PROFILE_FUNCTION();

        // Process any pending GPU resource creation commands from async loaders
        GPUResourceQueue::ProcessAll();

        // Poll shaders that are still being linked asynchronously by the driver
        // (GL_ARB_parallel_shader_compile). Finalize any that are done.
        if (m_ShaderLibrary.HasPendingShaders())
        {
            const u32 completed = m_ShaderLibrary.PollPendingShaders();
            if (completed > 0)
            {
                OLO_CORE_TRACE("{} shader(s) finished async linking ({} still pending)",
                               completed, m_ShaderLibrary.GetPendingCount());
            }
        }

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

        // Rotate the per-entity transform history so DrawMesh/DrawAnimated
        // submission can look up "previous frame" transforms. In Forward /
        // Forward+ these maps stay empty because those paths never call
        // GetAndRecordPrevTransform.
        s_Data.PrevEntityTransforms = std::move(s_Data.CurrEntityTransforms);
        s_Data.CurrEntityTransforms.clear();
        s_Data.PrevInstanceTransforms = std::move(s_Data.CurrInstanceTransforms);
        s_Data.CurrInstanceTransforms.clear();

        // Get main-thread allocator for this frame (already reset by BeginFrame)
        CommandAllocator* frameAllocator = FrameResourceManager::Get().GetMainAllocator();
        s_Data.ScenePass->GetCommandBucket().SetAllocator(frameAllocator);
        if (s_Data.DecalPass)
            s_Data.DecalPass->GetCommandBucket().SetAllocator(frameAllocator);
        if (s_Data.FoliagePass)
            s_Data.FoliagePass->GetCommandBucket().SetAllocator(frameAllocator);
        if (s_Data.ForwardOverlayPass)
            s_Data.ForwardOverlayPass->GetCommandBucket().SetAllocator(frameAllocator);
        if (s_Data.WaterPass)
            s_Data.WaterPass->GetCommandBucket().SetAllocator(frameAllocator);

        // TAA projection jitter. We bake a sub-pixel Halton offset into the
        // projection matrix so the same pixel samples a slightly different
        // geometric position each frame; the TAA accumulator then averages
        // across frames for sub-pixel anti-aliasing. The jitter is applied
        // uniformly to ProjectionMatrix and therefore ViewProjectionMatrix,
        // so every downstream pass (G-Buffer, lighting, decals, water,
        // SSAO/GTAO, post-process) observes the same jittered camera. Both
        // the current and previous ViewProjection carry their respective
        // jitters so depth-based reprojection in TAA remains self-consistent
        // without requiring an explicit unjitter uniform.
        s_Data.PrevJitterUV = s_Data.CurrJitterUV;
        s_Data.CurrJitterUV = glm::vec2(0.0f);
        if (s_Data.PostProcess.TAAEnabled && s_Data.ScenePass && s_Data.ScenePass->GetTarget())
        {
            const auto& spec = s_Data.ScenePass->GetTarget()->GetSpecification();
            if (spec.Width > 0 && spec.Height > 0)
            {
                // 1-based Halton index; Halton(0) is undefined. Loop modulo
                // kHaltonSequenceLength keeps the pattern short and stable.
                const u32 idx = (s_Data.TAAJitterFrameIndex % kHaltonSequenceLength) + 1;
                // Halton samples land in [0, 1]; remap to [-0.5, 0.5] so the
                // jitter is centred around the unperturbed pixel.
                const f32 jx = HaltonSample(idx, 2) - 0.5f;
                const f32 jy = HaltonSample(idx, 3) - 0.5f;

                // Convert pixel offset to NDC — 2 NDC units span the screen,
                // so one pixel in NDC = 2 / resolution.
                const f32 jitterNdcX = jx * (2.0f / static_cast<f32>(spec.Width));
                const f32 jitterNdcY = jy * (2.0f / static_cast<f32>(spec.Height));

                // For perspective projections (P[3][3] == 0, P[2][3] == -1),
                // inject jitter via the z-column of the projection matrix.
                // After the perspective divide this becomes a constant NDC
                // offset (x_ndc = P[2][0] * z / w_clip = P[2][0] * z / -z = -P[2][0])
                // which is exactly the sub-pixel shift we want.
                //
                // For orthographic projections (P[3][3] == 1, P[2][3] == 0),
                // writing to P[2][0/1] produces a *depth-dependent* shear:
                // x_ndc = P[0][0]*x + P[2][0]*z + P[3][0]. Instead, add the
                // jitter to the translation row so every fragment gets the
                // same sub-pixel shift independent of depth.
                const bool isOrthographic = glm::abs(s_Data.ProjectionMatrix[3][3] - 1.0f) < 1e-5f;
                if (isOrthographic)
                {
                    s_Data.ProjectionMatrix[3][0] += jitterNdcX;
                    s_Data.ProjectionMatrix[3][1] += jitterNdcY;
                }
                else
                {
                    s_Data.ProjectionMatrix[2][0] += jitterNdcX;
                    s_Data.ProjectionMatrix[2][1] += jitterNdcY;
                }
                s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;

                // Track jitter in UV-space so the TAA shader (or any future
                // consumer) can subtract it if needed. NDC -> UV is * 0.5.
                s_Data.CurrJitterUV = glm::vec2(jitterNdcX * 0.5f, jitterNdcY * 0.5f);

                s_Data.TAAJitterFrameIndex = (s_Data.TAAJitterFrameIndex + 1) % kHaltonSequenceLength;
            }
        }
        else
        {
            s_Data.TAAJitterFrameIndex = 0;
        }

        CommandDispatch::SetViewProjectionMatrix(s_Data.ViewProjectionMatrix);
        CommandDispatch::SetViewMatrix(s_Data.ViewMatrix);
        CommandDispatch::SetProjectionMatrix(s_Data.ProjectionMatrix);
        // Mirror the previous-frame view-projection into CommandDispatch so
        // dispatch paths that upload the shared CameraUBO themselves
        // (Terrain / Voxel / Decal) can fill
        // `CameraUBO::PrevViewProjection` with the true history instead of
        // aliasing the current VP — the latter wipes the matrix any other
        // consumer (TAA velocity reconstruction, motion blur) reads this
        // frame.
        CommandDispatch::SetPrevViewProjectionMatrix(s_Data.PrevViewProjectionMatrix);

        s_Data.InverseViewProjectionMatrix = glm::inverse(s_Data.ViewProjectionMatrix);
        s_Data.ViewFrustum.Update(s_Data.ViewProjectionMatrix);

        s_Data.Stats.Reset();
        s_Data.CommandCounter = 0;

        // Advance occlusion culling frame (reads back previous frame's query results)
        if (s_Data.OcclusionCullingEnabled)
        {
            s_Data.OcclusionResultsAvailable = OcclusionQueryPool::GetInstance().BeginFrame();
            OcclusionStateManager::GetInstance().BeginFrame();
        }
        else
        {
            s_Data.OcclusionResultsAvailable = false;
        }

        UpdateCameraMatricesUBO(s_Data.ViewMatrix, s_Data.ProjectionMatrix);
        UpdateLightPropertiesUBO();

        CommandDispatch::SetSceneLight(s_Data.SceneLight);
        CommandDispatch::SetViewPosition(s_Data.ViewPos);

        s_Data.ScenePass->ResetCommandBucket();
        if (s_Data.DecalPass)
            s_Data.DecalPass->ResetCommandBucket();
        if (s_Data.FoliagePass)
            s_Data.FoliagePass->ResetCommandBucket();
        if (s_Data.ForwardOverlayPass)
            s_Data.ForwardOverlayPass->ResetCommandBucket();
        if (s_Data.WaterPass)
            s_Data.WaterPass->ResetCommandBucket();

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
        // Route PBR shader slot to the G-Buffer write variant in Deferred mode
        // so parallel-submission workers pick the correct program without
        // needing to query RendererSettings per draw.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        s_Data.ParallelContext.PBRShader = (deferredActive && s_Data.PBRGBufferShader)
                                               ? s_Data.PBRGBufferShader
                                               : s_Data.PBRShader;
        s_Data.ParallelContext.PBRSkinnedShader = (deferredActive && s_Data.PBRGBufferSkinnedShader)
                                                      ? s_Data.PBRGBufferSkinnedShader
                                                      : s_Data.PBRSkinnedShader;
        s_Data.ParallelContext.LightCubeShader = s_Data.LightCubeShader;
        s_Data.ParallelContext.SkyboxShader = s_Data.SkyboxShader;
        s_Data.ParallelContext.QuadShader = s_Data.QuadShader;

        s_Data.ParallelSubmissionActive = false;

        // Phase B — populate the graph blackboard with live physical resources.
        SetupFrameBlackboard();
    }

    void Renderer3D::SetupFrameBlackboard()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.RGraph)
            return;

        auto& graph = *s_Data.RGraph;

        // Clear prior-frame handles so stale handles are never accidentally resolved.
        graph.ClearBlackboard();
        graph.ClearImportedResources();

        auto& board = graph.GetBlackboard();

        // ------------------------------------------------------------------
        // Scene outputs
        // ------------------------------------------------------------------
        if (s_Data.ScenePass && s_Data.ScenePass->GetTarget())
        {
            board.SceneColor = graph.ImportFramebuffer(
                ResourceNames::SceneColor, s_Data.ScenePass->GetTarget());

            // Sanity-check: importing must immediately resolve to the same
            // framebuffer. If not, the RenderGraph handle layer is broken.
            // Logged ONCE per change so we notice regressions without
            // spamming the log every frame.
            {
                static u32 s_PrevFbGL = 0;
                static u32 s_PrevTex0 = 0;
                const auto importedFB = s_Data.ScenePass->GetTarget();
                const auto resolveNow = graph.ResolveFramebuffer(board.SceneColor);
                const u32 importedFbGL = importedFB->GetRendererID();
                const u32 importedTex0 = importedFB->GetColorAttachmentRendererID(0);
                const u32 resolveFbGL = resolveNow ? resolveNow->GetRendererID() : 0u;
                const u32 resolveTex0 = resolveNow ? resolveNow->GetColorAttachmentRendererID(0) : 0u;
                if (importedFbGL != resolveFbGL || importedTex0 != resolveTex0)
                {
                    OLO_CORE_ERROR("Renderer3D: SceneColor IMPORT/RESOLVE MISMATCH: handle=(idx={}, gen={}) importedFbGL={} importedTex0={} resolveFbGL={} resolveTex0={}",
                                   board.SceneColor.Index, board.SceneColor.Generation,
                                   importedFbGL, importedTex0, resolveFbGL, resolveTex0);
                }
                else if (importedFbGL != s_PrevFbGL || importedTex0 != s_PrevTex0)
                {
                    OLO_CORE_INFO("Renderer3D: SceneColor IMPORT OK: handle=(idx={}, gen={}) fbGL={} tex0={}",
                                  board.SceneColor.Index, board.SceneColor.Generation, importedFbGL, importedTex0);
                    s_PrevFbGL = importedFbGL;
                    s_PrevTex0 = importedTex0;
                }
            }

            const u32 depthID = s_Data.ScenePass->GetTarget()->GetDepthAttachmentRendererID();
            board.SceneDepth = graph.ImportTexture(
                ResourceNames::SceneDepth, depthID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepth));

            const u32 sceneNormalsID = s_Data.ScenePass->GetTarget()->GetColorAttachmentRendererID(2);
            board.SceneNormals = graph.ImportTexture(
                ResourceNames::SceneNormals, sceneNormalsID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::SceneNormals));
        }

        // ------------------------------------------------------------------
        // G-Buffer (deferred path only)
        // ------------------------------------------------------------------
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        if (deferredActive && s_Data.ScenePass && s_Data.ScenePass->GetGBuffer())
        {
            // G-Buffer attachments come from the dedicated GBuffer object,
            // NOT from ScenePass->GetTarget() (which is the lit-output FB in
            // deferred mode and only has a single color attachment).
            //
            // Layout matches GBuffer::AttachmentIndex:
            //   RT0 Albedo   — albedo.rgb + metallic.a
            //   RT1 Normal   — octahedral normal + roughness + AO
            //   RT2 Emissive — emissive HDR
            //   RT3 Velocity — exposed via the dedicated `Velocity` import
            //                  block below; not duplicated here.
            const auto& gbuffer = s_Data.ScenePass->GetGBuffer();
            auto importGBuf = [&](std::string_view name, GBuffer::AttachmentIndex slot) -> RGTextureHandle
            {
                const u32 id = gbuffer->GetColorAttachmentID(slot);
                return graph.ImportTexture(name, id,
                                           RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, name));
            };

            board.GBufferAlbedo = importGBuf(ResourceNames::GBufferAlbedo, GBuffer::Albedo);
            board.GBufferNormal = importGBuf(ResourceNames::GBufferNormal, GBuffer::Normal);
            board.GBufferEmissive = importGBuf(ResourceNames::GBufferEmissive, GBuffer::Emissive);

            // Phase F slice 13 — multisample companion handles. Imported
            // only when MSAA is active so the typed-handle path can drive
            // per-sample shading without going through the raw GBuffer
            // accessor. SceneDepthMS is also exposed here (rather than
            // alongside SceneDepth) because the multisample depth lives on
            // the G-Buffer, not on the lit scene framebuffer.
            if (gbuffer->GetSampleCount() > 1u)
            {
                auto importGBufMS = [&](std::string_view name, GBuffer::AttachmentIndex slot) -> RGTextureHandle
                {
                    const u32 id = gbuffer->GetMSColorAttachmentID(slot);
                    return graph.ImportTexture(name, id,
                                               RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, name));
                };

                board.GBufferAlbedoMS = importGBufMS(ResourceNames::GBufferAlbedoMS, GBuffer::Albedo);
                board.GBufferNormalMS = importGBufMS(ResourceNames::GBufferNormalMS, GBuffer::Normal);
                board.GBufferEmissiveMS = importGBufMS(ResourceNames::GBufferEmissiveMS, GBuffer::Emissive);
                board.VelocityMS = importGBufMS(ResourceNames::VelocityMS, GBuffer::Velocity);

                if (const u32 depthMSID = gbuffer->GetMSDepthAttachmentID(); depthMSID != 0)
                {
                    board.SceneDepthMS = graph.ImportTexture(
                        ResourceNames::SceneDepthMS, depthMSID,
                        RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepthMS));
                }
            }
        }

        // ------------------------------------------------------------------
        // Velocity buffer
        // ------------------------------------------------------------------
        {
            u32 velocityID = 0;
            if (s_Data.Settings.Path == RenderingPath::Deferred &&
                s_Data.ScenePass && s_Data.ScenePass->GetGBuffer())
            {
                velocityID = s_Data.ScenePass->GetGBuffer()->GetColorAttachmentID(GBuffer::Velocity);
            }
            else if (s_Data.ScenePass && s_Data.ScenePass->GetTarget())
            {
                velocityID = s_Data.ScenePass->GetTarget()->GetColorAttachmentRendererID(3);
            }
            if (velocityID != 0)
            {
                board.Velocity = graph.ImportTexture(
                    ResourceNames::Velocity, velocityID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::Velocity));
            }
        }

        // ------------------------------------------------------------------
        // AO buffer
        // ------------------------------------------------------------------
        {
            u32 aoID = 0;
            if (s_Data.SSAOPass && s_Data.PostProcess.SSAOEnabled &&
                s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO)
            {
                aoID = s_Data.SSAOPass->GetSSAOTextureID();
            }
            else if (s_Data.GTAOPass && s_Data.PostProcess.GTAOEnabled &&
                     s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO)
            {
                aoID = s_Data.GTAOPass->GetGTAOTextureID();
            }
            if (aoID != 0)
            {
                board.AOBuffer = graph.ImportTexture(
                    ResourceNames::AOBuffer, aoID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::AOBuffer));
            }
        }

        // ------------------------------------------------------------------
        // Shadow maps
        // ------------------------------------------------------------------
        {
            const u32 csmID = s_Data.Shadow.GetCSMRendererID();
            const u32 spotID = s_Data.Shadow.GetSpotRendererID();
            if (csmID != 0)
            {
                board.ShadowMapCSM = graph.ImportTexture(
                    ResourceNames::ShadowMapCSM, csmID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2DArray, ResourceNames::ShadowMapCSM));
            }
            if (spotID != 0)
            {
                board.ShadowMapSpot = graph.ImportTexture(
                    ResourceNames::ShadowMapSpot, spotID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::ShadowMapSpot));
            }
            // Point-light shadow cubemaps — import each active light slot separately.
            for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            {
                const u32 pointID = s_Data.Shadow.GetPointRendererID(i);
                if (pointID != 0)
                {
                    board.ShadowMapPoint[i] = graph.ImportTexture(
                        ResourceNames::ShadowMapPoint[i], pointID,
                        RGResourceDesc::FromLegacy(ResourceHandle::Kind::TextureCube, ResourceNames::ShadowMapPoint[i]));
                }
            }
        }

        // ------------------------------------------------------------------
        // Post-process chain outputs
        // ------------------------------------------------------------------
        if (s_Data.SSSPass && s_Data.Snow.Enabled && s_Data.Snow.SSSBlurEnabled)
        {
            board.SSSColor = graph.ImportFramebuffer(
                ResourceNames::SSSColor, s_Data.SSSPass->GetTarget());
        }

        // Phase F slice 24 — AOApplyColor imported only when SSAO or GTAO is enabled.
        if (s_Data.AOApplyPass)
        {
            const bool ssaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO && s_Data.PostProcess.SSAOEnabled;
            const bool gtaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled;
            if (ssaoEnabled || gtaoEnabled)
            {
                board.AOApplyColor = graph.ImportFramebuffer(
                    ResourceNames::AOApplyColor, s_Data.AOApplyPass->GetTarget());
            }
        }

        if (s_Data.PostProcessPass)
        {
            // Phase F slice 25 — when all effects are handled by standalone passes,
            // PostProcessRenderPass is a transparent node (Execute returns immediately,
            // no blit). Alias PostProcessColor to the best available upstream source so
            // downstream passes (BloomPass, etc.) receive valid data on every frame.
            Ref<Framebuffer> postTarget;
            if (s_Data.PostProcessPass->IsAllHandledExternally())
            {
                if (s_Data.AOApplyPass)
                    postTarget = s_Data.AOApplyPass->GetTarget();
                if (!postTarget && s_Data.SSSPass)
                    postTarget = s_Data.SSSPass->GetTarget();
                if (!postTarget && s_Data.ScenePass)
                    postTarget = s_Data.ScenePass->GetTarget();
            }
            if (!postTarget)
                postTarget = s_Data.PostProcessPass->GetTarget();
            board.PostProcessColor = graph.ImportFramebuffer(
                ResourceNames::PostProcessColor, postTarget);
        }

        // Phase F slice 23 — BloomColor imported only when Bloom is enabled.
        if (s_Data.BloomPass && s_Data.PostProcess.BloomEnabled)
        {
            board.BloomColor = graph.ImportFramebuffer(
                ResourceNames::BloomColor, s_Data.BloomPass->GetTarget());
        }

        // Phase F slice 22 — DOFColor imported only when DOF is enabled.
        if (s_Data.DOFPass && s_Data.PostProcess.DOFEnabled)
        {
            board.DOFColor = graph.ImportFramebuffer(
                ResourceNames::DOFColor, s_Data.DOFPass->GetTarget());
        }

        // Phase F slice 21 — MotionBlurColor imported only when motion blur is enabled.
        if (s_Data.MotionBlurPass && s_Data.PostProcess.MotionBlurEnabled)
        {
            board.MotionBlurColor = graph.ImportFramebuffer(
                ResourceNames::MotionBlurColor, s_Data.MotionBlurPass->GetTarget());
        }

        // Phase F slice 19 — TAAColor imported only when TAA is enabled.
        if (s_Data.TAAPass && s_Data.PostProcess.TAAEnabled)
        {
            board.TAAColor = graph.ImportFramebuffer(
                ResourceNames::TAAColor, s_Data.TAAPass->GetTarget());
        }

        // Phase F slice 20 — PrecipitationColor imported only when screen FX are active.
        const bool precipScreenEnabled = s_Data.Precipitation.Enabled &&
                                         (s_Data.Precipitation.ScreenStreaksEnabled ||
                                          s_Data.Precipitation.LensImpactsEnabled);
        if (s_Data.PrecipitationPass && precipScreenEnabled)
        {
            board.PrecipitationColor = graph.ImportFramebuffer(
                ResourceNames::PrecipitationColor, s_Data.PrecipitationPass->GetTarget());
        }

        // Phase F slice 18 — FogColor imported only when fog is enabled.
        if (s_Data.FogPass && s_Data.Fog.Enabled)
        {
            board.FogColor = graph.ImportFramebuffer(
                ResourceNames::FogColor, s_Data.FogPass->GetTarget());
        }

        // Phase F slice 17 — extracted effect sub-chain. Each handle is
        // imported only when its effect is enabled so downstream consumers
        // can rely on IsValid() as the canonical "effect ran" signal.
        // ToneMap is imported unconditionally (no settings gate).
        if (s_Data.ChromAberrationPass && s_Data.PostProcess.ChromaticAberrationEnabled)
        {
            board.ChromAbColor = graph.ImportFramebuffer(
                ResourceNames::ChromAbColor, s_Data.ChromAberrationPass->GetTarget());
        }
        if (s_Data.ColorGradingPass && s_Data.PostProcess.ColorGradingEnabled)
        {
            board.ColorGradingColor = graph.ImportFramebuffer(
                ResourceNames::ColorGradingColor, s_Data.ColorGradingPass->GetTarget());
        }
        if (s_Data.ToneMapPass)
        {
            board.ToneMapColor = graph.ImportFramebuffer(
                ResourceNames::ToneMapColor, s_Data.ToneMapPass->GetTarget());
        }
        if (s_Data.VignettePass && s_Data.PostProcess.VignetteEnabled)
        {
            board.VignetteColor = graph.ImportFramebuffer(
                ResourceNames::VignetteColor, s_Data.VignettePass->GetTarget());
        }

        // Phase F slice 16 — only import FXAAColor when FXAA is active so
        // downstream consumers can rely on `board.FXAAColor.IsValid()` as
        // the canonical "anti-aliased post-process available" signal.
        if (s_Data.FXAAPass && s_Data.PostProcess.FXAAEnabled)
        {
            if (auto fxaaTarget = s_Data.FXAAPass->GetTarget())
            {
                board.FXAAColor = graph.ImportFramebuffer(
                    ResourceNames::FXAAColor, fxaaTarget);
            }
        }

        if (s_Data.SelectionOutlinePass && s_Data.EnableSelectionOutline)
        {
            board.SelectionOutlineColor = graph.ImportFramebuffer(
                ResourceNames::SelectionOutlineColor, s_Data.SelectionOutlinePass->GetTarget());
        }

        if (s_Data.UICompositePass)
        {
            board.UIComposite = graph.ImportFramebuffer(
                ResourceNames::UIComposite, s_Data.UICompositePass->GetTarget());
        }

        // Default framebuffer / swapchain target represented as an imported
        // external output resource. Backing framebuffer is null by design;
        // FinalPass presents via RGCommandContext::BindDefaultFramebuffer().
        board.Backbuffer = graph.ImportFramebuffer(
            ResourceNames::Backbuffer, nullptr,
            RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, ResourceNames::Backbuffer));

        // ------------------------------------------------------------------
        // OIT buffers
        // ------------------------------------------------------------------
        // Phase F slice 14 — OIT graph resources are imported only when
        // weighted-blended OIT is *actually* active for this frame. Skipping
        // the import when OIT is disabled means transparent contributor
        // passes (Particle / Water / Decal) bail out of their
        // `builder.Write(board.OITAccum, ...)` declarations
        // (`if (board.OITAccum.IsValid())` is already guarded), so the
        // graph never sees write edges into a buffer that nothing reads.
        // OITResolvePass also self-skips via `m_Enabled`. The underlying
        // `OITBuffer` Ref persists across toggles to avoid allocator churn
        // when the user flips the setting interactively.
        const bool oitActive = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                               s_Data.Settings.Deferred.OITEnabled &&
                               s_Data.OITResolvePass;
        if (oitActive)
        {
            // Phase F slice 15 — trigger lazy creation. If OIT was never
            // active in any prior frame, this is when the OITBuffer is
            // first allocated.
            const auto& oitBuf = s_Data.OITResolvePass->GetOrCreateOITBuffer();
            if (oitBuf)
            {
                // OITBuffer owns a single MRT framebuffer with accum(RT0) and revealage(RT1).
                board.OITAccum = graph.ImportFramebuffer(
                    ResourceNames::OITAccum, oitBuf->GetFramebuffer());
                board.OITRevealage = graph.ImportFramebuffer(
                    ResourceNames::OITRevealage, oitBuf->GetFramebuffer());
            }
        }

        // ------------------------------------------------------------------
        // Temporal histories (imported from prior frame)
        // ------------------------------------------------------------------
        // Phase F slice 19 — TAAHistory is now owned by TAARenderPass.
        // Fall back to PostProcessPass when TAAPass is not yet created
        // (e.g. headless contexts / future partial configs).
        if (s_Data.TAAPass)
        {
            board.TAAHistory = graph.ImportHistory(
                ResourceNames::TAAHistory, s_Data.TAAPass->GetTAAHistoryTextureID());
        }
        else if (s_Data.PostProcessPass)
        {
            board.TAAHistory = graph.ImportHistory(
                ResourceNames::TAAHistory, s_Data.PostProcessPass->GetTAAHistoryTextureID());
        }
        // Phase F slice 18 — FogHistory is now owned by FogRenderPass.
        // Fall back to PostProcessPass when FogPass is not yet created
        // (e.g. headless contexts / future partial configs).
        if (s_Data.FogPass)
        {
            board.FogHistory = graph.ImportHistory(
                ResourceNames::FogHistory, s_Data.FogPass->GetFogHistoryTextureID());
        }
        else if (s_Data.PostProcessPass)
        {
            board.FogHistory = graph.ImportHistory(
                ResourceNames::FogHistory, s_Data.PostProcessPass->GetFogHistoryTextureID());
        }

        // ------------------------------------------------------------------
        // IBL resources
        // ------------------------------------------------------------------
        if (s_Data.GlobalIrradianceMapID != 0)
        {
            board.IrradianceMap = graph.ImportTexture(
                ResourceNames::IrradianceMap, s_Data.GlobalIrradianceMapID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::TextureCube, ResourceNames::IrradianceMap));
        }
        if (s_Data.GlobalPrefilterMapID != 0)
        {
            board.PrefilterMap = graph.ImportTexture(
                ResourceNames::PrefilterMap, s_Data.GlobalPrefilterMapID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::TextureCube, ResourceNames::PrefilterMap));
        }
        if (s_Data.GlobalBRDFLutMapID != 0)
        {
            board.BrdfLut = graph.ImportTexture(
                ResourceNames::BrdfLut, s_Data.GlobalBRDFLutMapID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::BrdfLut));
        }
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

    void Renderer3D::UploadFogVolumes(const FogVolumesUBOData& data)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.FogVolumesGPUData = data;
    }

    CommandPacket* Renderer3D::DrawDecal(
        const glm::mat4& decalTransform,
        const glm::mat4& inverseDecalTransform,
        const glm::vec4& decalColor,
        const glm::vec4& decalParams,
        RendererID albedoTextureID,
        i32 entityID)
    {
        // Delegate to the extended variant with Albedo mode + zero extra textures.
        return DrawDecal(decalTransform, inverseDecalTransform, decalColor, decalParams,
                         albedoTextureID, /*normal*/ 0u, /*rma*/ 0u,
                         DrawDecalCommand::DecalMode::Albedo,
                         /*transparent*/ false, entityID);
    }

    CommandPacket* Renderer3D::DrawDecal(
        const glm::mat4& decalTransform,
        const glm::mat4& inverseDecalTransform,
        const glm::vec4& decalColor,
        const glm::vec4& decalParams,
        RendererID albedoTextureID,
        RendererID normalTextureID,
        RendererID rmaTextureID,
        DrawDecalCommand::DecalMode mode,
        bool transparent,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.DecalPass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawDecal: DecalPass is null!");
            return nullptr;
        }

        if (!s_Data.DecalShader || !s_Data.DecalCubeMesh)
        {
            return nullptr;
        }

        auto va = s_Data.DecalCubeMesh->GetVertexArray();
        if (!va)
        {
            return nullptr;
        }

        CommandPacket* packet = CreateDecalDrawCall<DrawDecalCommand>();
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawDecal: Failed to allocate decal command packet!");
            return nullptr;
        }
        auto* cmd = packet->GetCommandData<DrawDecalCommand>();
        cmd->header.type = CommandType::DrawDecal;

        // Deferred-path decals write into the G-Buffer attachment that matches
        // the decal mode (Albedo → RT0, Normal → RT1, RMA → RT0.a+RT1.zw) BEFORE
        // the lighting pass, so they are re-lit by DeferredLightingPass. In
        // Forward/Forward+, every mode collapses to the existing transparent
        // overlay shader — there is no forward-path normal/RMA decal. Transparent
        // decals always route through the forward shader even in Deferred, so
        // they composite over the lit scene colour after DeferredLightingPass.
        const bool deferredPath = !transparent &&
                                  s_Data.Settings.Path == RenderingPath::Deferred &&
                                  s_Data.DecalGBufferShader != nullptr;
        Ref<Shader> decalShader = s_Data.DecalShader;
        if (deferredPath)
        {
            switch (mode)
            {
                case DrawDecalCommand::DecalMode::Normal:
                    if (s_Data.DecalGBufferNormalShader)
                        decalShader = s_Data.DecalGBufferNormalShader;
                    else
                        decalShader = s_Data.DecalGBufferShader;
                    break;
                case DrawDecalCommand::DecalMode::RMA:
                    if (s_Data.DecalGBufferRMAShader)
                        decalShader = s_Data.DecalGBufferRMAShader;
                    else
                        decalShader = s_Data.DecalGBufferShader;
                    break;
                case DrawDecalCommand::DecalMode::Emissive:
                    if (s_Data.DecalGBufferEmissiveShader)
                        decalShader = s_Data.DecalGBufferEmissiveShader;
                    else
                        decalShader = s_Data.DecalGBufferShader;
                    break;
                case DrawDecalCommand::DecalMode::Albedo:
                default:
                    decalShader = s_Data.DecalGBufferShader;
                    break;
            }
        }

        cmd->vertexArrayID = va->GetRendererID();
        cmd->indexCount = s_Data.DecalCubeMesh->GetIndexCount();
        cmd->shaderRendererID = decalShader->GetRendererID();
        cmd->decalTransform = decalTransform;
        cmd->inverseDecalTransform = inverseDecalTransform;
        cmd->inverseViewProjection = s_Data.InverseViewProjectionMatrix;
        cmd->decalColor = decalColor;
        cmd->decalParams = decalParams;
        cmd->albedoTextureID = albedoTextureID;
        cmd->normalTextureID = normalTextureID;
        cmd->rmaTextureID = rmaTextureID;
        cmd->mode = deferredPath
                        ? mode
                        : DrawDecalCommand::DecalMode::Albedo; // Forward path always albedo.
        cmd->transparent = transparent ? u8{ 1 } : u8{ 0 };
        cmd->entityID = entityID;

        // Decal render state: blend on for albedo (soft edges), blend off for
        // normal/RMA (hard discard threshold — see shader comments). Depth
        // read-only, front-face culling in all cases.
        {
            PODRenderState decalState = CreateDefaultPODRenderState();
            const bool blendForThisMode = (cmd->mode == DrawDecalCommand::DecalMode::Albedo);
            decalState.blendEnabled = blendForThisMode;
            decalState.blendSrcFactor = GL_SRC_ALPHA;
            decalState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
            decalState.depthTestEnabled = true;
            decalState.depthFunction = GL_LEQUAL;
            decalState.depthWriteMask = false;
            decalState.cullingEnabled = true;
            decalState.cullFace = GL_FRONT;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(decalState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: in Forward/Forward+ decals are transparent overlays
        // rendered after opaque geometry; in Deferred they write into the
        // G-Buffer pre-lighting so they are opaque from the sorter's POV.
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = decalShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(decalTransform);
        metadata.m_SortKey = deferredPath
                                 ? DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth)
                                 : DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawFoliageLayer(
        RendererID vertexArrayID, u32 indexCount, u32 instanceCount,
        RendererID albedoTextureID,
        const glm::mat4& modelTransform,
        f32 time,
        f32 prevTime,
        f32 windStrength, f32 windSpeed,
        f32 viewDistance, f32 fadeStart, f32 alphaCutoff,
        const glm::vec4& baseColor,
        const BoundingBox& layerBounds,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.FoliagePass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawFoliageLayer: FoliagePass is null!");
            return nullptr;
        }

        if (!s_Data.FoliageShader)
        {
            return nullptr;
        }

        // Deferred: route through ScenePass (the G-Buffer FB) with the
        // G-Buffer variant shader so foliage participates in the deferred
        // lighting composite. Falls back to the forward FoliagePass route
        // when the variant is missing.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = deferredActive && s_Data.FoliageGBufferShader && s_Data.ScenePass;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.FoliageGBufferShader : s_Data.FoliageShader;

        // Frustum cull the entire layer using the precomputed bounding box
        if (s_Data.FrustumCullingEnabled)
        {
            BoundingBox worldBounds = layerBounds.Transform(modelTransform);
            if (!s_Data.ViewFrustum.IsBoundingBoxVisible(worldBounds))
            {
                return nullptr;
            }
        }

        CommandPacket* packet = useGBufferVariant
                                    ? CreateDrawCall<DrawFoliageLayerCommand>()
                                    : CreateFoliageDrawCall<DrawFoliageLayerCommand>();
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawFoliageLayer: Failed to allocate foliage command packet!");
            return nullptr;
        }
        auto* cmd = packet->GetCommandData<DrawFoliageLayerCommand>();
        cmd->header.type = CommandType::DrawFoliageLayer;

        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = indexCount;
        cmd->instanceCount = instanceCount;
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->modelTransform = modelTransform;
        cmd->normalMatrix = glm::transpose(glm::inverse(modelTransform));
        cmd->time = time;
        cmd->prevTime = prevTime;
        cmd->windStrength = windStrength;
        cmd->windSpeed = windSpeed;
        cmd->viewDistance = viewDistance;
        cmd->fadeStart = fadeStart;
        cmd->alphaCutoff = alphaCutoff;
        cmd->baseColor = baseColor;
        cmd->albedoTextureID = albedoTextureID;
        cmd->entityID = entityID;

        // Foliage render state: opaque alpha-tested, depth test + write, no blend
        {
            PODRenderState foliageState = CreateDefaultPODRenderState();
            foliageState.depthTestEnabled = true;
            foliageState.depthFunction = GL_LEQUAL;
            foliageState.depthWriteMask = true;
            foliageState.blendEnabled = false;
            foliageState.cullingEnabled = true;
            foliageState.cullFace = GL_BACK;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(foliageState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: opaque, sorted by shader then depth (front-to-back)
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(modelTransform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawWaterSurface(
        RendererID vertexArrayID, u32 indexCount,
        const glm::mat4& modelTransform,
        f32 time,
        f32 prevTime,
        const WaterDrawParams& params,
        const BoundingBox& bounds,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.WaterPass)
        {
            OLO_CORE_ERROR("Renderer3D::DrawWaterSurface: WaterPass is null!");
            return nullptr;
        }

        if (!s_Data.WaterShader)
        {
            return nullptr;
        }

        // Frustum cull the water surface
        if (s_Data.FrustumCullingEnabled)
        {
            BoundingBox worldBounds = bounds.Transform(modelTransform);
            if (!s_Data.ViewFrustum.IsBoundingBoxVisible(worldBounds))
            {
                return nullptr;
            }
        }

        CommandPacket* packet = CreateWaterDrawCall<DrawWaterCommand>();
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawWaterSurface: Failed to allocate water command packet!");
            return nullptr;
        }
        auto* cmd = packet->GetCommandData<DrawWaterCommand>();
        cmd->header.type = CommandType::DrawWater;

        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = indexCount;
        cmd->shaderRendererID = s_Data.WaterShader->GetRendererID();
        cmd->modelTransform = modelTransform;
        cmd->normalMatrix = glm::transpose(glm::inverse(modelTransform));

        // Pack time into waveParams.x
        glm::vec4 wp = params.waveParams;
        wp.x = time;
        cmd->waveParams = wp;
        cmd->waveDir0 = params.waveDir0;
        cmd->waveDir1 = params.waveDir1;
        cmd->waterColor = params.waterColor;
        cmd->waterDeepColor = params.waterDeepColor;
        cmd->visualParams = params.visualParams;
        cmd->normalMapScroll = params.normalMapScroll;
        cmd->normalMapSpeed = params.normalMapSpeed;
        // Pack previous-frame time into normalMapSpeed.z so the water shader can
        // re-evaluate the Gerstner sum at `t - dt` for per-fragment velocity
        // reprojection (closes the wave-animation gap in the RT3 motion vector).
        cmd->normalMapSpeed.z = prevTime;
        cmd->lightDirection = params.lightDirection;
        cmd->screenParams = params.screenParams;
        cmd->depthRefractionParams = params.depthRefractionParams;
        cmd->refractionColor = params.refractionColor;
        cmd->foamParams = params.foamParams;
        cmd->foamParams2 = params.foamParams2;
        cmd->sssColor = params.sssColor;
        cmd->ssrParams = params.ssrParams;
        cmd->tessParams = params.tessParams;
        cmd->normalMap0ID = params.normalMap0ID;
        cmd->normalMap1ID = params.normalMap1ID;
        cmd->noiseTextureID = params.noiseTextureID;
        cmd->foamTextureID = params.foamTextureID;
        cmd->refractionEnabled = params.refractionEnabled;
        cmd->ssrEnabled = params.ssrEnabled;
        cmd->entityID = entityID;

        // Water render state: translucent, depth test on, depth write off, alpha blend
        {
            PODRenderState waterState = CreateDefaultPODRenderState();
            waterState.depthTestEnabled = true;
            waterState.depthFunction = GL_LEQUAL;
            waterState.depthWriteMask = false;
            waterState.blendEnabled = true;
            waterState.blendSrcFactor = GL_SRC_ALPHA;
            waterState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
            waterState.cullingEnabled = true;
            waterState.cullFace = GL_BACK;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(waterState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: translucent, sorted back-to-front for correct blending
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = s_Data.WaterShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(modelTransform);
        metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = false;
        packet->SetMetadata(metadata);

        return packet;
    }

    void Renderer3D::EndScene()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("Renderer3D::EndScene: Render graph is null!");
            return;
        }

        // Phase F slice 35 — OITResolvePass and SSSPass are now self-resolving:
        // their Execute(RGCommandContext&) calls context.GetBlackboard() to look
        // up SceneColor directly, eliminating the per-frame side-channel. The
        // legacy raw SetInputFramebuffer() setters remain as headless / test
        // fallbacks.
        // (Previously slice 12 set typed handles here; that block has been
        // removed since those two passes resolve via the blackboard.)

        // Phase F slice 36 — ForwardOverlayPass, FoliagePass, WaterPass,
        // DecalPass and ParticlePass now self-resolve SceneColor (and
        // SceneDepth for Decal) directly from the render-graph blackboard
        // inside their Execute() implementations. No per-frame side-channel
        // setter calls are needed here.
        {
            // Phase F slice 41 — DeferredLightingPass now self-resolves all
            // G-Buffer and scene depth handles from the render-graph
            // blackboard. No SetXxx calls needed.
        }

        if (s_Data.SSAOPass)
        {
            s_Data.SSAOPass->SetSettings(s_Data.PostProcess);
            // Phase F slice 37 — SSAOPass now self-resolves SceneDepth and
            // SceneNormals from the blackboard; no per-frame handle setters needed.

            // Upload projection matrices for SSAO position reconstruction
            s_Data.SSAOGPUData.Projection = s_Data.ProjectionMatrix;
            s_Data.SSAOGPUData.InverseProjection = glm::inverse(s_Data.ProjectionMatrix);
            s_Data.SSAOGPUData.DebugView = s_Data.PostProcess.SSAODebugView ? 1 : 0;
        }
        if (s_Data.GTAOPass)
        {
            s_Data.GTAOPass->SetSettings(s_Data.PostProcess);
            s_Data.GTAOPass->SetProjectionMatrix(s_Data.ProjectionMatrix);
            // Phase F slice 37 — GTAOPass now self-resolves SceneDepth and
            // SceneNormals from the blackboard; no per-frame handle setters needed.

            // When GTAO is active, override the SSAO UBO debug/intensity fields
            // so the PostProcess_SSAOApply shader reads correct values for GTAO,
            // and upload the UBO since SSAORenderPass::Execute() is skipped.
            if (s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled)
            {
                s_Data.SSAOGPUData.DebugView = s_Data.PostProcess.GTAODebugView ? 1 : 0;
                // GTAO power is baked in compute; intensity=1 for the apply pass
                s_Data.SSAOGPUData.Intensity = 1.0f;
                s_Data.SSAOUBO->SetData(&s_Data.SSAOGPUData, SSAOUBOData::GetSize());
                s_Data.SSAOUBO->Bind();
            }
        }
        if (s_Data.SSSPass)
        {
            s_Data.SSSPass->SetSettings(s_Data.Snow);
        }
        // Phase F slice 24 — wire AOApplyPass before PostProcessPass.
        if (s_Data.AOApplyPass)
        {
            const bool ssaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO && s_Data.PostProcess.SSAOEnabled;
            const bool gtaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled;
            s_Data.AOApplyPass->SetEnabled(ssaoEnabled || gtaoEnabled);
            // Phase F slice 38 — SetAOTextureID removed; AOApplyPass::Execute()
            // self-resolves AO texture via the render-graph blackboard.
            s_Data.AOApplyPass->SetPostProcessUBO(s_Data.PostProcessUBO);
        }
        if (s_Data.PostProcessPass)
        {
            s_Data.PostProcessPass->SetSettings(s_Data.PostProcess);
            s_Data.PostProcessPass->SetPostProcessUBO(s_Data.PostProcessUBO, &s_Data.PostProcessGPUData);
            s_Data.PostProcessPass->SetFogEnabled(s_Data.Fog.Enabled);
            s_Data.PostProcessPass->SetPrecipitationScreenEffectsEnabled(
                s_Data.Precipitation.Enabled &&
                (s_Data.Precipitation.ScreenStreaksEnabled || s_Data.Precipitation.LensImpactsEnabled));
            // Phase F slice 39 — SetAOTextureID removed; PostProcessPass::Execute()
            // self-resolves AO texture via the render-graph blackboard.
            s_Data.PostProcessPass->SetBloomHandledExternally(s_Data.BloomPass != nullptr);
            s_Data.PostProcessPass->SetChromAbHandledExternally(s_Data.ChromAberrationPass != nullptr);
            s_Data.PostProcessPass->SetColorGradingHandledExternally(s_Data.ColorGradingPass != nullptr);
            s_Data.PostProcessPass->SetToneMapHandledExternally(s_Data.ToneMapPass != nullptr);
            s_Data.PostProcessPass->SetVignetteHandledExternally(s_Data.VignettePass != nullptr);
            s_Data.PostProcessPass->SetDOFHandledExternally(s_Data.DOFPass != nullptr);
            s_Data.PostProcessPass->SetMotionBlurHandledExternally(s_Data.MotionBlurPass != nullptr);
            s_Data.PostProcessPass->SetTAAHandledExternally(s_Data.TAAPass != nullptr);
            s_Data.PostProcessPass->SetPrecipitationHandledExternally(s_Data.PrecipitationPass != nullptr);
            // Phase F slice 24 — AO Apply is now handled by AOApplyRenderPass.
            s_Data.PostProcessPass->SetAOApplyHandledExternally(s_Data.AOApplyPass != nullptr);

            // Phase F slice 23 — wire BloomPass and tell PostProcess
            // to skip the inline Bloom section it now delegates.
            if (s_Data.BloomPass)
            {
                s_Data.BloomPass->SetEnabled(s_Data.PostProcess.BloomEnabled);
                // Phase F slice 43 — SetInputFramebufferHandle removed; self-resolved in Execute()
                s_Data.BloomPass->SetPostProcessUBO(s_Data.PostProcessUBO);
                s_Data.BloomPass->SetPostProcessGPUData(&s_Data.PostProcessGPUData);
            }

            // Phase F slice 22 — wire DOFPass and tell PostProcess
            // to skip the inline DOF section it now delegates.
            if (s_Data.DOFPass)
            {
                s_Data.DOFPass->SetEnabled(s_Data.PostProcess.DOFEnabled);
                // Phase F slice 40 — SetInputFramebufferHandle and
                // SetSceneDepthTextureHandle removed; self-resolved in Execute().
                s_Data.DOFPass->SetPostProcessUBO(s_Data.PostProcessUBO);
            }

            // Phase F slice 21 — wire MotionBlurPass and tell PostProcess
            // to skip the inline motion blur section it now delegates.
            if (s_Data.MotionBlurPass)
            {
                s_Data.MotionBlurPass->SetEnabled(s_Data.PostProcess.MotionBlurEnabled);
                // Phase F slice 40 — SetInputFramebufferHandle and
                // SetSceneDepthTextureHandle removed; self-resolved in Execute().
                s_Data.MotionBlurPass->SetMotionBlurUBO(s_Data.MotionBlurUBO);
            }

            // Phase F slice 19 — wire TAAPass and tell PostProcess to skip
            // the inline TAA section it now delegates.
            if (s_Data.TAAPass)
            {
                s_Data.TAAPass->SetEnabled(s_Data.PostProcess.TAAEnabled);
                s_Data.TAAPass->SetSettings(s_Data.PostProcess);
                // Phase F slice 40 — SetInputFramebufferHandle,
                // SetSceneDepthTextureHandle, SetVelocityTextureHandle removed;
                // self-resolved in Execute().
            }

            // Phase F slice 20 — wire PrecipitationPass and tell PostProcess to skip
            // the inline precipitation section it now delegates.
            if (s_Data.PrecipitationPass)
            {
                const bool precipEnabled = s_Data.Precipitation.Enabled &&
                                           (s_Data.Precipitation.ScreenStreaksEnabled ||
                                            s_Data.Precipitation.LensImpactsEnabled);
                s_Data.PrecipitationPass->SetEnabled(precipEnabled);
                // Phase F slice 44 — SetInputFramebufferHandle removed; self-resolved in Execute()
            }

            // Phase F slice 18 — wire FogPass and tell PostProcess to skip
            // the inline fog section it now delegates.
            s_Data.PostProcessPass->SetFogHandledExternally(s_Data.FogPass != nullptr);
            if (s_Data.FogPass)
            {
                s_Data.FogPass->SetEnabled(s_Data.Fog.Enabled);
                s_Data.FogPass->SetPostProcessUBO(s_Data.PostProcessUBO);
                // Phase F slice 42 — FogPass now self-resolves input framebuffer,
                // scene depth, and shadow map from the render-graph blackboard.
                // No SetXxx handle calls needed.
            }

            if (s_Data.ChromAberrationPass)
            {
                s_Data.ChromAberrationPass->SetEnabled(s_Data.PostProcess.ChromaticAberrationEnabled);
                s_Data.ChromAberrationPass->SetPostProcessUBO(s_Data.PostProcessUBO);
            }
            if (s_Data.ColorGradingPass)
            {
                s_Data.ColorGradingPass->SetEnabled(s_Data.PostProcess.ColorGradingEnabled);
                s_Data.ColorGradingPass->SetPostProcessUBO(s_Data.PostProcessUBO);
            }
            if (s_Data.ToneMapPass)
            {
                // ToneMap always runs; m_Enabled stays true.
                s_Data.ToneMapPass->SetPostProcessUBO(s_Data.PostProcessUBO);
            }
            if (s_Data.VignettePass)
            {
                s_Data.VignettePass->SetEnabled(s_Data.PostProcess.VignetteEnabled);
                s_Data.VignettePass->SetPostProcessUBO(s_Data.PostProcessUBO);
            }

            // Phase F slice 16 — wire FXAA pass.
            if (s_Data.FXAAPass)
            {
                s_Data.FXAAPass->SetEnabled(s_Data.PostProcess.FXAAEnabled);
                s_Data.FXAAPass->SetPostProcessUBO(s_Data.PostProcessUBO);
            }
            // Tell PostProcess to skip the inline FXAA stage so the standalone
            // pass is the single source of truth.
            s_Data.PostProcessPass->SetFXAAHandledExternally(s_Data.FXAAPass != nullptr);

            if (s_Data.EnableSelectionOutline && s_Data.SelectionOutlinePass)
            {
                // Phase F slice 44 — SetInputFramebufferHandle removed; self-resolved in Execute()
            }

            if (s_Data.UICompositePass)
            {
                // Phase F slice 44 — SetInputFramebufferHandle removed; self-resolved in Execute()
            }

            if (s_Data.FinalPass)
            {
                // Phase F slice 44 — SetInputFramebufferHandle removed; self-resolved in Execute()
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

        // Upload fog & atmospheric scattering settings to GPU
        if (s_Data.Fog.Enabled)
        {
            auto& fog = s_Data.Fog;
            auto& gpu = s_Data.FogGPUData;
            gpu.ColorAndDensity = glm::vec4(fog.Color, fog.Density);
            gpu.DistanceParams = glm::vec4(fog.Start, fog.End, fog.HeightFalloff, fog.HeightOffset);
            gpu.ScatterParams = glm::vec4(fog.RayleighStrength, fog.MieStrength, fog.MieDirectionality, fog.SunIntensity);
            gpu.RayleighColorAndMaxOpacity = glm::vec4(fog.RayleighColor, fog.MaxOpacity);
            // Derive sun direction from the scene's primary directional light
            // Guard against zero-length direction to prevent NaN from normalize
            glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
            f32 const dirLen2 = glm::dot(s_Data.SceneLight.Direction, s_Data.SceneLight.Direction);
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
            auto fogNow = std::chrono::steady_clock::now();
            f32 const fogDt = std::clamp(std::chrono::duration<f32>(fogNow - s_Data.FogLastTime).count(), 0.0f, 0.1f);
            s_Data.FogLastTime = fogNow;
            s_Data.FogTime += fogDt;

            f32 const effectiveNoiseIntensity = fog.EnableNoise ? fog.NoiseIntensity : 0.0f;
            gpu.NoiseParams = glm::vec4(fog.NoiseScale, fog.NoiseSpeed, effectiveNoiseIntensity, s_Data.FogTime);
            gpu.VolumetricParams = glm::vec4(static_cast<f32>(fog.VolumetricSamples), fog.AbsorptionCoefficient,
                                             fog.LightShaftIntensity, fog.EnableLightShafts ? 1.0f : 0.0f);
            s_Data.FogUBO->SetData(&gpu, FogUBOData::GetSize());
        }
        else
        {
            auto& gpu = s_Data.FogGPUData;
            gpu.Flags = glm::vec4(0.0f);
            s_Data.FogUBO->SetData(&gpu, FogUBOData::GetSize());
        }

        // Upload fog volumes (collected by the scene)
        s_Data.FogVolumesUBO->SetData(&s_Data.FogVolumesGPUData, FogVolumesUBOData::GetSize());

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
            auto& mb = s_Data.MotionBlurGPUData;
            mb.InverseViewProjection = s_Data.InverseViewProjectionMatrix;
            mb.PrevViewProjection = s_Data.PrevViewProjectionMatrix;
            s_Data.MotionBlurUBO->SetData(&mb, MotionBlurUBOData::GetSize());
        }

        // Wire deferred lighting pass inputs each frame so it reflects the
        // current G-Buffer / debug-channel selection. The pass no-ops when
        // the path is Forward / Forward+ (GBuffer is never created).
        if (s_Data.DeferredLightPass && s_Data.ScenePass)
        {
            const bool deferred = (s_Data.Settings.Path == RenderingPath::Deferred);
            s_Data.DeferredLightPass->SetGBuffer(deferred ? s_Data.ScenePass->GetGBuffer() : nullptr);
            s_Data.DeferredLightPass->SetDebugChannel(deferred ? s_Data.Settings.Deferred.DebugChannel : 0);
            s_Data.DeferredLightPass->SetPerSampleLighting(deferred && s_Data.Settings.Deferred.PerSampleLighting);
        }

        // Wire the opaque-decal graph shim: in Deferred mode it drains the
        // DecalRenderPass bucket into the G-Buffer between ScenePass and
        // DeferredLightingPass. Safe to update unconditionally — the pass
        // no-ops when it isn't registered in the graph (Forward paths).
        if (s_Data.OpaqueDecalPass && s_Data.DecalPass && s_Data.ScenePass)
        {
            const bool deferred = (s_Data.Settings.Path == RenderingPath::Deferred);
            s_Data.OpaqueDecalPass->SetDecalPass(s_Data.DecalPass);
            s_Data.OpaqueDecalPass->SetGBuffer(deferred ? s_Data.ScenePass->GetGBuffer() : nullptr);
            s_Data.OpaqueDecalPass->SetPerSampleLighting(deferred && s_Data.Settings.Deferred.PerSampleLighting);
        }

        // Phase 6: propagate OIT toggle to transparent passes + resolve
        // every frame so UI changes take effect immediately.
        {
            const bool oitEnabled = (s_Data.Settings.Path == RenderingPath::Deferred) && s_Data.Settings.Deferred.OITEnabled;
            if (s_Data.ParticlePass)
                s_Data.ParticlePass->SetOITEnabled(oitEnabled);
            if (s_Data.WaterPass)
                s_Data.WaterPass->SetOITEnabled(oitEnabled);
            if (s_Data.DecalPass)
                s_Data.DecalPass->SetOITEnabled(oitEnabled);
            if (s_Data.OITResolvePass)
                s_Data.OITResolvePass->SetEnabled(oitEnabled);
        }

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
                OLO_CORE_INFO("RenderGraph BuildFrameGraph stats: passes={}, reads={}, writes={}, derivedEdges={}",
                              buildStats.PassesVisited,
                              buildStats.DeclaredReads,
                              buildStats.DeclaredWrites,
                              buildStats.DerivedEdges);
                s_LastBuildStats = buildStats;
                s_HasLastBuildStats = true;
            }
        }

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
        s_Data.ScenePass->GetCommandBucket().SetAllocator(nullptr);
        if (s_Data.DecalPass)
            s_Data.DecalPass->GetCommandBucket().SetAllocator(nullptr);
        if (s_Data.FoliagePass)
            s_Data.FoliagePass->GetCommandBucket().SetAllocator(nullptr);
        if (s_Data.ForwardOverlayPass)
            s_Data.ForwardOverlayPass->GetCommandBucket().SetAllocator(nullptr);
        if (s_Data.WaterPass)
            s_Data.WaterPass->GetCommandBucket().SetAllocator(nullptr);

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

        // Worker allocators already reset in BeginFrame — no separate prepare needed

        // Prepare frame data buffer for parallel submission
        FrameDataBufferManager::Get().PrepareForParallelSubmission();

        s_Data.ParallelSubmissionActive = true;
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

        s_Data.ParallelSubmissionActive = false;
    }

    WorkerSubmitContext Renderer3D::GetWorkerContext(u32 workerIndex)
    {
        OLO_PROFILE_FUNCTION();

        WorkerSubmitContext ctx;

        // Get worker allocator directly from FrameResourceManager — zero overhead
        ctx.WorkerIndex = workerIndex;
        ctx.Allocator = FrameResourceManager::Get().GetWorkerAllocator(workerIndex);

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
                                                i32 entityID,
                                                const LODGroup* lodGroup,
                                                const glm::mat4* prevModelMatrix)
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

        // LOD selection
        Ref<Mesh> meshToUse;
        auto lodResult = SelectLODMesh(mesh, modelMatrix, ctx.SceneContext->ViewPosition, lodGroup, meshToUse);
        if (lodResult.SelectedLODIndex >= 0)
        {
            if (lodResult.SelectedLODIndex >= static_cast<i32>(ctx.ObjectsPerLODLevel.size()))
            {
                ctx.ObjectsPerLODLevel.resize(lodResult.SelectedLODIndex + 1, 0);
            }
            ctx.ObjectsPerLODLevel[lodResult.SelectedLODIndex]++;
            if (lodResult.Switched)
            {
                ctx.LODSwitches++;
            }
        }

        if (!meshToUse || !meshToUse->GetVertexArray())
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

        // Deferred-path gating: workers submit exclusively into ScenePass's
        // per-thread bucket, which is the G-Buffer producer. Non-PBR /
        // forward-only override shaders on this path would alias forward
        // outputs onto G-Buffer slots (breaking lighting for every
        // subsequent pixel). Instead of dropping the draw we reroute the
        // fully-assembled packet into ForwardOverlayPass's global bucket,
        // matching the serial `DrawMesh` overlay-reroute behaviour so
        // worker-submitted forward-only materials render as overlays
        // after DeferredLightingPass composes the G-Buffer.
        //
        // Note: `ForwardOverlayPass::SubmitPacket` is mutex-protected
        // (CommandBucket's internal lock) so calling it from a worker is
        // safe — the serialisation cost is acceptable for rare forward-
        // only materials on the Deferred path. The packet memory is still
        // owned by the worker's allocator (both allocators reset at end-
        // of-frame; the overlay bucket stores pointers, not copies).
        //
        // In Deferred `ctx.SceneContext->PBRShader` is already swapped to
        // `PBRGBufferShader` (see ParallelContext init), so an un-overridden
        // PBR material never triggers this reroute.
        const bool overlayReroute = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                                    !IsDeferredCapableShader(shaderToUse) &&
                                    s_Data.ForwardOverlayPass;

        const u32 vertexArrayID = meshToUse->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawMeshParallel", vertexArrayID, shaderRendererID))
            return nullptr;

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
        cmd->meshHandle = meshToUse->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = meshToUse->GetIndexCount();
        cmd->baseIndex = meshToUse->GetBaseIndex();
        cmd->transform = glm::mat4(modelMatrix);
        // When the caller has prev-frame history, use it; otherwise alias
        // current so motion vectors are zero for this draw. Parallel workers
        // cannot touch the main-thread entity motion-history map, so the
        // caller (typically via MeshSubmitDesc::PrevTransform) must supply
        // the history for per-object velocity to be correct.
        cmd->prevTransform = prevModelMatrix ? *prevModelMatrix : cmd->transform;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

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
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);

        // Compute depth using parallel context's view matrix
        u32 depthKey = ComputeDepthForSortKeyWithView(modelMatrix, ctx.SceneContext->ViewMatrix);

        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        if (overlayReroute)
        {
            // Hand the packet off to the overlay bucket directly and return
            // nullptr so the caller's follow-up SubmitPacketParallel becomes
            // a no-op (mirrors the serial DrawMesh overlay-reroute pattern).
            // The global bucket's mutex serialises worker submissions; the
            // volume of forward-only draws on Deferred is expected to be
            // small enough that this doesn't become a contention point.
            s_Data.ForwardOverlayPass->SubmitPacket(packet);
            return nullptr;
        }

        return packet;
    }

    CommandPacket* Renderer3D::DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                        const Ref<Mesh>& mesh,
                                                        const glm::mat4& modelMatrix,
                                                        const Material& material,
                                                        const std::vector<glm::mat4>& boneMatrices,
                                                        bool isStatic)
    {
        // Legacy entry point: no prev-pose information available. Alias current
        // bones and transform into the prev slot so motion-vector shaders see
        // zero per-bone and per-object motion for this draw.
        static const std::vector<glm::mat4> s_EmptyPrev;
        return DrawAnimatedMeshParallel(ctx, mesh, modelMatrix, material, boneMatrices,
                                        s_EmptyPrev, modelMatrix, /*hasPrevTransform*/ false, isStatic);
    }

    CommandPacket* Renderer3D::DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                        const Ref<Mesh>& mesh,
                                                        const glm::mat4& modelMatrix,
                                                        const Material& material,
                                                        const std::vector<glm::mat4>& boneMatrices,
                                                        const std::vector<glm::mat4>& prevBoneMatrices,
                                                        const glm::mat4& prevModelMatrix,
                                                        bool hasPrevTransform,
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

        // Same Deferred gating as DrawMeshParallel — forward-only skinned
        // shaders submitted from a worker would corrupt the G-Buffer.
        if (s_Data.Settings.Path == RenderingPath::Deferred &&
            !IsDeferredCapableShader(shaderToUse))
        {
            static std::atomic<u64> s_WarnCount{ 0 };
            if (s_WarnCount.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                OLO_CORE_WARN("Renderer3D::DrawAnimatedMeshParallel: forward-only skinned shader on Deferred path — draw dropped (use serial DrawAnimatedMesh for overlay reroute)");
            }
            return nullptr;
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

        // Previous-frame pose: allocate a second palette in the same worker
        // scratch so skinned shaders (PBR_MultiLight_Skinned, PBR_GBuffer_Skinned)
        // can emit per-bone velocity via binding 31. Only do this when the
        // caller supplied a matching-size prev palette; otherwise we leave the
        // sentinel UINT32_MAX in the command so CommandDispatch::UploadBoneMatrices
        // aliases current into prev (zero per-bone motion). Allocation failure
        // silently falls back to alias-current for this draw only.
        u32 localPrevBoneOffset = UINT32_MAX;
        const bool wantPrevBones = !prevBoneMatrices.empty() &&
                                   prevBoneMatrices.size() == boneMatrices.size();
        if (wantPrevBones)
        {
            u32 prevOffset = frameBuffer.AllocateBoneMatricesParallel(ctx.WorkerIndex, boneCount);
            if (prevOffset != UINT32_MAX)
            {
                frameBuffer.WriteBoneMatricesParallel(ctx.WorkerIndex, prevOffset, prevBoneMatrices.data(), boneCount);
                localPrevBoneOffset = prevOffset;
            }
        }

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

        const u32 vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawAnimatedMeshParallel", vertexArrayID, shaderRendererID))
            return nullptr;

        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = mesh->GetIndexCount();
        cmd->baseIndex = mesh->GetBaseIndex();
        cmd->transform = modelMatrix;
        // Use caller-supplied prev transform when available; otherwise alias
        // current so u_PrevModel - u_Model = 0 and shader velocity is 0.
        cmd->prevTransform = hasPrevTransform ? prevModelMatrix : modelMatrix;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        // Animation support - store worker-local offsets with remapping info.
        // Both current and (when present) prev offsets are worker-local and
        // must be remapped to global during EndParallelSubmission().
        cmd->isAnimatedMesh = true;
        cmd->boneBufferOffset = localBoneOffset;
        cmd->prevBoneBufferOffset = localPrevBoneOffset;
        cmd->boneCount = boneCount;
        cmd->workerIndex = static_cast<u8>(ctx.WorkerIndex);
        cmd->needsBoneOffsetRemap = true;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);

        u32 depthKey = ComputeDepthForSortKeyWithView(modelMatrix, ctx.SceneContext->ViewMatrix);

        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        else
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
                    // Route through the prev-aware DrawAnimatedMesh overload
                    // when the caller supplied prev-pose data; otherwise the
                    // legacy entry aliases current->prev (zero motion).
                    if (desc.PrevBoneMatrices)
                    {
                        packet = DrawAnimatedMesh(desc.Mesh, desc.Transform, desc.MaterialData,
                                                  *desc.BoneMatrices, *desc.PrevBoneMatrices,
                                                  desc.IsStatic, desc.EntityID);
                    }
                    else
                    {
                        packet = DrawAnimatedMesh(desc.Mesh, desc.Transform, desc.MaterialData,
                                                  *desc.BoneMatrices, desc.IsStatic, desc.EntityID);
                    }
                }
                else
                {
                    // DrawMesh internally records prev-transform via the shared
                    // per-entity cache (GetAndRecordPrevTransform) keyed on
                    // entityID, so object motion is preserved even without an
                    // explicit prev-aware overload on this path. When the
                    // caller has already computed a prev-transform (e.g. for
                    // animated-mesh fallback paths that store it in desc), seed
                    // the cache so DrawMesh's internal lookup returns the
                    // caller-authoritative value instead of potentially stale
                    // prior-frame history.
                    if (desc.HasPrevTransform && desc.EntityID >= 0)
                    {
                        s_Data.PrevEntityTransforms.insert_or_assign(desc.EntityID, desc.PrevTransform);
                    }
                    packet = DrawMesh(desc.Mesh, desc.Transform, desc.MaterialData, desc.IsStatic, desc.EntityID, desc.LODGroupPtr);
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
                    // Route through the prev-aware overload when the caller
                    // supplied prev-pose data; otherwise fall back to the
                    // legacy entry which aliases current->prev (zero motion).
                    if (desc.PrevBoneMatrices || desc.HasPrevTransform)
                    {
                        static const std::vector<glm::mat4> s_EmptyPrev;
                        const std::vector<glm::mat4>& prevBones =
                            desc.PrevBoneMatrices ? *desc.PrevBoneMatrices : s_EmptyPrev;
                        packet = Renderer3D::DrawAnimatedMeshParallel(
                            stats.Context,
                            desc.Mesh,
                            desc.Transform,
                            desc.MaterialData,
                            *desc.BoneMatrices,
                            prevBones,
                            desc.PrevTransform,
                            desc.HasPrevTransform,
                            desc.IsStatic);
                    }
                    else
                    {
                        packet = Renderer3D::DrawAnimatedMeshParallel(
                            stats.Context,
                            desc.Mesh,
                            desc.Transform,
                            desc.MaterialData,
                            *desc.BoneMatrices,
                            desc.IsStatic);
                    }
                }
                else
                {
                    const glm::mat4* prevXform = desc.HasPrevTransform ? &desc.PrevTransform : nullptr;
                    packet = Renderer3D::DrawMeshParallel(
                        stats.Context,
                        desc.Mesh,
                        desc.Transform,
                        desc.MaterialData,
                        desc.IsStatic,
                        desc.EntityID,
                        desc.LODGroupPtr,
                        prevXform);
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
            s_Data.Stats.LODSwitches += workerStats[i].Context.LODSwitches;
            for (sizet j = 0; j < workerStats[i].Context.ObjectsPerLODLevel.size(); ++j)
            {
                if (j >= s_Data.Stats.ObjectsPerLODLevel.size())
                {
                    s_Data.Stats.ObjectsPerLODLevel.resize(j + 1, 0);
                }
                s_Data.Stats.ObjectsPerLODLevel[j] += workerStats[i].Context.ObjectsPerLODLevel[j];
            }
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

    void Renderer3D::UploadMultiLightUBO(const UBOStructures::MultiLightUBO& data, i32 activeLightCount)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.MultiLightBuffer)
        {
            // Clamp to valid range to prevent buffer overrun
            activeLightCount = std::clamp(activeLightCount, 0, static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS));

            // Only upload header (16 bytes) + active lights to minimize CPU→GPU transfer
            constexpr u32 headerSize = 4 * sizeof(i32); // LightCount, MaxLights, ShadowCasterCount, DirectionalLightCount
            const u32 uploadSize = headerSize + static_cast<u32>(activeLightCount) * static_cast<u32>(sizeof(UBOStructures::MultiLightData));

            // Ensure the GPU header reflects the clamped count (the caller may
            // have set data.LightCount to a value exceeding MAX_LIGHTS).
            if (data.LightCount != activeLightCount)
            {
                UBOStructures::MultiLightUBO temp = data;
                temp.LightCount = activeLightCount;
                s_Data.MultiLightBuffer->SetData(&temp, uploadSize);
            }
            else
            {
                s_Data.MultiLightBuffer->SetData(&data, uploadSize);
            }
        }
    }

    void Renderer3D::UploadLightProbeData(const ShaderBindingLayout::LightProbeVolumeUBO& uboData,
                                          const void* shData, u32 shDataSize)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.LightProbeVolumeUBO)
        {
            s_Data.LightProbeVolumeUBO->SetData(&uboData, ShaderBindingLayout::LightProbeVolumeUBO::GetSize());
        }

        if (shData && shDataSize > 0)
        {
            if (!s_Data.LightProbeSHBuffer || s_Data.LightProbeSHBuffer->GetSize() < shDataSize)
            {
                s_Data.LightProbeSHBuffer = StorageBuffer::Create(shDataSize, ShaderBindingLayout::SSBO_LIGHT_PROBES);
            }
            s_Data.LightProbeSHBuffer->SetData(shData, shDataSize);
        }
        // When no SH data is provided, the UBO's Enabled field should already be 0,
        // causing the shader to early-out. The SSBO remains bound from init (zeroed).
    }

    void Renderer3D::SetGlobalIBL(RendererID irradianceMapID, RendererID prefilterMapID,
                                  RendererID brdfLutMapID, RendererID environmentMapID,
                                  f32 iblIntensity)
    {
        s_Data.GlobalIrradianceMapID = irradianceMapID;
        s_Data.GlobalPrefilterMapID = prefilterMapID;
        s_Data.GlobalBRDFLutMapID = brdfLutMapID;
        s_Data.GlobalEnvironmentMapID = environmentMapID;
        s_Data.GlobalIBLIntensity = iblIntensity;
    }

    void Renderer3D::ClearGlobalIBL()
    {
        s_Data.GlobalIrradianceMapID = 0;
        s_Data.GlobalPrefilterMapID = 0;
        s_Data.GlobalBRDFLutMapID = 0;
        s_Data.GlobalEnvironmentMapID = 0;
        s_Data.GlobalIBLIntensity = 1.0f;
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

    void Renderer3D::EnableDepthPrepass(bool enable)
    {
        OLO_PROFILE_FUNCTION();
        s_Data.DepthPrepassEnabled = enable;
    }

    bool Renderer3D::IsDepthPrepassEnabled()
    {
        return s_Data.DepthPrepassEnabled;
    }

    void Renderer3D::EnableOcclusionCulling(bool enable)
    {
        OLO_PROFILE_FUNCTION();
        s_Data.OcclusionCullingEnabled = enable;
        if (enable)
        {
            auto& queryPool = OcclusionQueryPool::GetInstance();
            if (!queryPool.IsInitialized())
            {
                queryPool.Initialize(1024);
            }
            auto& culler = OcclusionCuller::GetInstance();
            if (!culler.IsInitialized())
            {
                culler.Initialize();
            }
            OcclusionStateManager::GetInstance().SetMaxQueries(queryPool.GetMaxQueries());
        }
    }

    bool Renderer3D::IsOcclusionCullingEnabled()
    {
        return s_Data.OcclusionCullingEnabled;
    }

    Renderer3D::Statistics& Renderer3D::GetStats()
    {
        return s_Data.Stats;
    }

    void Renderer3D::ResetStats()
    {
        s_Data.Stats.Reset();
    }

    void Renderer3D::ApplyRendererSettings()
    {
        OLO_PROFILE_FUNCTION();
        auto& settings = s_Data.Settings;

        // Clamp MSAA sample count to what the driver advertises. The combo
        // box exposes 1/2/4/8 but older or mobile GPUs may cap at 4. Logs
        // on clamp so users notice rather than silently dropping samples.
        if (const u32 maxSamples = GetMaxMSAASamples(); maxSamples > 0)
        {
            const u32 requested = settings.Deferred.MSAASampleCount;
            if (requested > maxSamples)
            {
                OLO_CORE_WARN("Renderer3D: MSAASampleCount={} exceeds driver cap {}. Clamping.",
                              requested, maxSamples);
                settings.Deferred.MSAASampleCount = maxSamples;
            }
        }

        // Detect a RenderingPath switch and rebuild the graph topology
        // BEFORE touching the Forward+ mode / culling toggles below so that
        // downstream code always observes a graph whose registered pass
        // list matches the active path. RGraph must exist — if we're called
        // pre-Init (defensive), skip the rebuild and let SetupRenderGraph
        // do the first configure.
        // Phase F slice 34: also detect ActiveAOTechnique changes so that
        // the conditional AO-pass registration in ConfigureRenderGraph
        // reflects the newly selected technique without waiting for a path
        // switch.
        const bool pathChanged = settings.Path != s_Data.ActiveGraphPath;
        const bool aoTechniqueChanged =
            s_Data.PostProcess.ActiveAOTechnique != s_Data.ActiveGraphAOTechnique;
        if (s_Data.RGraph && (pathChanged || aoTechniqueChanged))
        {
            ConfigureRenderGraph(settings.Path);
        }

        // Sync culling toggles
        EnableFrustumCulling(settings.FrustumCullingEnabled);
        EnableOcclusionCulling(settings.OcclusionCullingEnabled);

        // Sync Forward+ settings
        auto& fplus = s_Data.ForwardPlus;
        switch (settings.Path)
        {
            case RenderingPath::Forward:
                if (settings.ForwardPlusAutoSwitch)
                {
                    fplus.SetMode(ForwardPlusMode::Auto);
                    fplus.SetLightCountThreshold(settings.ForwardPlusLightThreshold);
                    fplus.SetLightCountThresholdDown(settings.ForwardPlusLightThresholdDown);
                }
                else
                {
                    fplus.SetMode(ForwardPlusMode::Never);
                }
                break;
            case RenderingPath::ForwardPlus:
                fplus.SetMode(ForwardPlusMode::Always);
                break;
            case RenderingPath::Deferred:
                // Deferred reuses the Forward+ tile-culling compute to build
                // per-tile light lists; the G-Buffer lighting shader samples
                // those same SSBOs. Forcing ForwardPlusMode::Always here
                // guarantees the tile classification runs every frame while
                // the scene pipeline is operating in Deferred mode.
                fplus.SetMode(ForwardPlusMode::Always);
                break;
        }

        // Forward+ compute culling requires the depth pre-pass.
        // Include the Auto case: when Forward+ can dynamically activate,
        // the depth buffer must already be available for the culling dispatch.
        // Deferred likewise needs the depth buffer before the lighting pass —
        // the G-Buffer depth attachment is populated by the scene pass MRT
        // writes, and the depth-prepass additionally supports Forward+ tile
        // culling reused by DeferredLightingPass.
        bool effectiveDepthPrepass = settings.DepthPrepassEnabled || (settings.Path == RenderingPath::ForwardPlus) || (settings.Path == RenderingPath::Deferred) || (settings.Path == RenderingPath::Forward && settings.ForwardPlusAutoSwitch);
        EnableDepthPrepass(effectiveDepthPrepass);

        fplus.SetTileSize(settings.ForwardPlusTileSize);
        fplus.SetDebugVisualization(settings.ForwardPlusDebugHeatmap);
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

    CommandPacket* Renderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic, i32 entityID, const LODGroup* lodGroup)
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
            if (mesh && !IsVisibleInFrustum(mesh, modelMatrix))
            {
                s_Data.Stats.CulledMeshes++;
                return nullptr;
            }
        }

        // Temporal occlusion culling: skip objects that were occluded last frame,
        // and submit proxy bounding boxes for re-testing.
        if (s_Data.OcclusionCullingEnabled && s_Data.OcclusionResultsAvailable && entityID >= 0 && mesh)
        {
            auto& stateMgr = OcclusionStateManager::GetInstance();
            auto& state = stateMgr.GetOrCreate(static_cast<u64>(entityID));

            // Allocate query index if this is a new object
            if (state.QueryIndex == UINT32_MAX)
            {
                state.QueryIndex = stateMgr.AllocateQueryIndex();
            }

            // Read back previous frame's result
            if (state.QueryIndex != UINT32_MAX)
            {
                auto& queryPool = OcclusionQueryPool::GetInstance();
                bool visible = queryPool.WasVisible(state.QueryIndex);
                state.WasVisible = visible;

                if (!visible)
                {
                    state.InvisibleFrameCount++;
                    // Re-test periodically to detect when occluded objects become visible
                    if (state.InvisibleFrameCount % kOcclusionRetestInterval == 0)
                    {
                        BoundingSphere bs = mesh->GetTransformedBoundingSphere(modelMatrix);
                        BoundingBox worldBounds;
                        worldBounds.Min = bs.Center - glm::vec3(bs.Radius);
                        worldBounds.Max = bs.Center + glm::vec3(bs.Radius);
                        OcclusionCuller::GetInstance().QueueBoundingBox(state.QueryIndex, worldBounds);
                    }
                    s_Data.Stats.CulledMeshes++;
                    return nullptr;
                }
                else
                {
                    state.InvisibleFrameCount = 0;
                    // Queue visible objects for occlusion testing so they can
                    // transition to occluded when something moves in front of them
                    BoundingSphere bs = mesh->GetTransformedBoundingSphere(modelMatrix);
                    BoundingBox worldBounds;
                    worldBounds.Min = bs.Center - glm::vec3(bs.Radius);
                    worldBounds.Max = bs.Center + glm::vec3(bs.Radius);
                    OcclusionCuller::GetInstance().QueueBoundingBox(state.QueryIndex, worldBounds);
                }
            }
        }

        // LOD selection
        Ref<Mesh> meshToUse;
        auto lodResult = SelectLODMesh(mesh, modelMatrix, s_Data.ViewPos, lodGroup, meshToUse);
        if (lodResult.SelectedLODIndex >= 0)
        {
            if (lodResult.SelectedLODIndex >= static_cast<i32>(s_Data.Stats.ObjectsPerLODLevel.size()))
            {
                s_Data.Stats.ObjectsPerLODLevel.resize(lodResult.SelectedLODIndex + 1, 0);
            }
            s_Data.Stats.ObjectsPerLODLevel[lodResult.SelectedLODIndex]++;
            if (lodResult.Switched)
            {
                s_Data.Stats.LODSwitches++;
            }
        }

        if (!meshToUse || !meshToUse->GetVertexArray())
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: Invalid mesh or vertex array!");
            return nullptr;
        }

        Ref<Shader> shaderToUse;
        // Deferred mode demands that every ScenePass draw write the 4-RT
        // G-Buffer layout (Albedo/Metallic, Normal/Roughness/AO, Emissive/
        // Flags, Velocity). Non-PBR materials selecting s_Data.LightingShader
        // would instead write the legacy forward outputs (o_Color / o_EntityID
        // / o_ViewNormal / o_Velocity), which alias onto the G-Buffer slots
        // and corrupt lighting for every subsequent pixel.
        //
        // Until a Lighting3D_GBuffer variant lands, reroute such draws to the
        // ForwardOverlayPass — which binds the scene framebuffer (matching
        // MRT layout) and runs *after* DeferredLightingPass composites the
        // G-Buffer, so the non-PBR surface shades itself and blits over the
        // lit deferred image unscathed. Mirrors the same pattern DrawSkybox
        // uses for the skybox-on-deferred fallback.
        bool overlayRoute = false;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
            // Forward-only override on the Deferred path would alias its
            // output locations onto G-Buffer slots. Reroute to
            // ForwardOverlayPass so the override gets the forward FB layout
            // it was authored against.
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.ForwardOverlayPass &&
                !IsDeferredCapableShader(shaderToUse))
            {
                overlayRoute = true;
            }
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            // Route PBR default shader to the G-Buffer write variant when the
            // deferred path is active. Material overrides still win (so
            // custom shaders, e.g. terrain/foliage, keep their forward
            // pipeline until their own G-Buffer variants land in later phases).
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.PBRGBufferShader)
                shaderToUse = s_Data.PBRGBufferShader;
            else
                shaderToUse = s_Data.PBRShader;
        }
        else
        {
            shaderToUse = s_Data.LightingShader;
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.ForwardOverlayPass)
                overlayRoute = true;
        }

        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: No shader available!");
            return nullptr;
        }

        const u32 vertexArrayID = meshToUse->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawMesh", vertexArrayID, shaderRendererID))
            return nullptr;

        // Create POD command using asset handles and renderer IDs
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawMeshCommand>()
                                    : CreateDrawCall<DrawMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = meshToUse->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = meshToUse->GetIndexCount();
        cmd->baseIndex = meshToUse->GetBaseIndex();
        cmd->transform = glm::mat4(modelMatrix);
        // Prev-transform is recorded for every path — forward PBR shaders
        // now emit screen-space velocity into scene FB RT3 alongside the
        // deferred G-Buffer variant, so TAA consumes per-object motion in
        // Forward / Forward+ too. Static meshes self-alias (prev == curr)
        // so their velocity reads zero.
        cmd->prevTransform = GetAndRecordPrevTransform(entityID, cmd->transform);
        cmd->entityID = entityID;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        // No bone matrices for non-animated mesh
        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        // Store occlusion query index for conditional rendering in dispatch
        if (s_Data.OcclusionCullingEnabled && entityID >= 0)
        {
            auto& stateMgr = OcclusionStateManager::GetInstance();
            if (stateMgr.Has(static_cast<u64>(entityID)))
            {
                u32 queryIdx = stateMgr.GetOrCreate(static_cast<u64>(entityID)).QueryIndex;
                if (queryIdx != UINT32_MAX)
                {
                    cmd->occlusionQueryIndex = queryIdx;
                }
            }
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for optimal command sorting
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID & 0xFFFF; // 16-bit shader ID
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            // Submit to the overlay bucket directly; return nullptr so the
            // caller's follow-up SubmitPacket(packet) becomes a safe no-op
            // (same pattern DrawSkybox/DrawInfiniteGrid use).
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

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
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreateDefaultPODRenderState());

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

    CommandPacket* Renderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic, u64 ownerKey)
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

        const std::vector<glm::mat4>* activeTransforms = &transforms;
        std::vector<glm::mat4> filteredTransforms;
        // Index map from post-cull visible slot -> pre-cull stable instance
        // index. Passed to GetAndRecordPrevInstanceTransforms so history
        // lookup uses the full pre-cull array for identity stability, then
        // projects prev onto the visible subset. Empty when no culling ran.
        std::vector<u32> visibleIndices;

        if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
        {
            // Extract the local bounding sphere once and transform per-instance
            // instead of recomputing from mesh source each time
            BoundingSphere localSphere = mesh->GetBoundingSphere();
            localSphere.Radius *= 1.3f; // Match expansion factor from IsVisibleInFrustum
            filteredTransforms.reserve(transforms.size());
            visibleIndices.reserve(transforms.size());
            for (sizet i = 0; i < transforms.size(); ++i)
            {
                const auto& t = transforms[i];
                BoundingSphere worldSphere = localSphere.Transform(t);
                if (s_Data.ViewFrustum.IsBoundingSphereVisible(worldSphere))
                {
                    filteredTransforms.push_back(t);
                    visibleIndices.push_back(static_cast<u32>(i));
                }
            }
            s_Data.Stats.CulledMeshes += static_cast<u32>(transforms.size() - filteredTransforms.size());
            if (filteredTransforms.empty())
            {
                return nullptr;
            }
            activeTransforms = &filteredTransforms;
        }

        // Allocate space in FrameDataBuffer for instance transforms
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 transformCount = static_cast<u32>(activeTransforms->size());
        u32 transformOffset = frameBuffer.AllocateTransforms(transformCount);
        if (transformOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: Failed to allocate transform buffer space");
            return nullptr;
        }
        frameBuffer.WriteTransforms(transformOffset, activeTransforms->data(), transformCount);

        // Previous-frame transforms (Deferred per-instance velocity). Cache keyed by
        // (mesh, ownerKey) so two submission sources that render the same mesh with
        // independent instance arrays don't overwrite each other's history;
        // caller-ordering stability within an owner is still required. First frame
        // / count mismatches alias the current data -> zero velocity.
        // Record per-instance transform history on every render path, not
        // just Deferred: in Forward/Forward+ the PBR shader writes velocity
        // into scene-FB RT3 via the CameraMatrices UBO, and instanced draws
        // without `prevTransformBufferOffset` set fall back to the
        // static-geometry path (prev == curr) — which silently zeroes
        // per-object motion for TAA when the velocity source is scene FB RT3
        // (Forward/Forward+). See EndScene velocity-source selection: the
        // scene FB path is now taken whenever RenderingPath != Deferred.
        u32 prevTransformOffset = UINT32_MAX;
        if (mesh)
        {
            const u64 meshKey = static_cast<u64>(mesh->GetHandle());
            bool usedFallback = false;
            // Pass the **full pre-cull** transform list so history is keyed by
            // stable per-instance identity. When culling dropped instances,
            // `visibleIndices` projects the prev array onto the visible subset
            // so slot i in prevTransforms lines up with slot i in
            // activeTransforms. Without this, a different frustum-visible
            // subset next frame would silently alias unrelated instances.
            const std::vector<u32>* idxPtr = visibleIndices.empty() ? nullptr : &visibleIndices;
            std::vector<glm::mat4> prevTransforms = GetAndRecordPrevInstanceTransforms(meshKey, ownerKey, transforms, idxPtr, &usedFallback);
            // Use the explicit flag rather than pointer identity — the function
            // returns a projected vector by value in the fallback path, so
            // prevTransforms.data() is always a distinct buffer.
            if (!usedFallback && prevTransforms.size() == activeTransforms->size())
            {
                u32 prevOffset = frameBuffer.AllocateTransforms(transformCount);
                if (prevOffset != UINT32_MAX)
                {
                    frameBuffer.WriteTransforms(prevOffset, prevTransforms.data(), transformCount);
                    prevTransformOffset = prevOffset;
                }
            }
        }

        Ref<Shader> shaderToUse = material.GetShader() ? material.GetShader() : s_Data.LightingShader;

        // Create POD command
        CommandPacket* packet = CreateDrawCall<DrawMeshInstancedCommand>();
        auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();
        cmd->header.type = CommandType::DrawMeshInstanced;

        const u32 vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawMeshInstanced", vertexArrayID, shaderRendererID))
            return nullptr;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = mesh->GetIndexCount();
        cmd->baseIndex = mesh->GetBaseIndex();
        cmd->instanceCount = transformCount;
        cmd->transformBufferOffset = transformOffset;
        cmd->prevTransformBufferOffset = prevTransformOffset;
        cmd->transformCount = transformCount;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCountPerInstance = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for instanced mesh commands (use first transform for depth)
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = activeTransforms->empty() ? 0 : ComputeDepthForSortKey((*activeTransforms)[0]);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
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

        // In Deferred mode route into the ScenePass (which binds the 4-RT
        // G-Buffer) and substitute the LightCube_GBuffer variant shader so
        // the cube writes a full MRT payload with `emissive.a = 1.0` (unlit
        // flag). `ComputeDeferredLit` then short-circuits and outputs the
        // raw emissive colour — matching the forward debug look. Falls back
        // to the ForwardOverlayPass if the G-Buffer variant failed to load.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = deferredActive && s_Data.LightCubeGBufferShader;
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.ForwardOverlayPass;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.LightCubeGBufferShader : s_Data.LightCubeShader;

        // Create POD command on the appropriate bucket.
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawMeshCommand>()
                                    : CreateDrawCall<DrawMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        const u32 vertexArrayID = s_Data.CubeMesh->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = activeShader ? activeShader->GetRendererID() : 0u;
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawLightCube", vertexArrayID, shaderRendererID))
            return nullptr;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = s_Data.CubeMesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = s_Data.CubeMesh->GetIndexCount();
        cmd->transform = modelMatrix;
        cmd->prevTransform = modelMatrix; // debug viz — no motion history
        cmd->shaderHandle = activeShader->GetHandle();

        // Light cube material data — simple default material
        {
            PODMaterialData matData{};
            matData.shaderRendererID = shaderRendererID;
            matData.ambient = glm::vec3(1.0f);
            matData.diffuse = glm::vec3(1.0f);
            matData.specular = glm::vec3(1.0f);
            matData.shininess = 32.0f;
            cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(matData);
        }

        // Render state via table
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreateDefaultPODRenderState());

        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for light cube
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

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
        // Previous-frame view-projection is maintained by BeginSceneCommon
        // in `s_Data.PrevViewProjectionMatrix`. Forward PBR shaders consume
        // this through the CameraMatrices UBO (binding 0) to emit screen-
        // space velocity into scene FB RT3 — mirroring what the deferred
        // G-Buffer PBR shader does through u_PrevViewProjection.
        cameraData.PrevViewProjection = s_Data.PrevViewProjectionMatrix;

        constexpr u32 expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
        static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch");

        s_Data.CameraUBO->SetData(&cameraData, expectedSize);

        // Re-bind to ensure this UBO is active at binding point 0.
        // Other subsystems (e.g. ShadowMap) create their own camera UBOs at the
        // same binding point, which can overwrite the persistent binding.
        s_Data.CameraUBO->Bind();
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

        // Phase D: enable runtime materialization of compiled transient plan
        // entries. Unit tests keep this path disabled by default; production
        // renderer setup opts in explicitly.
        s_Data.RGraph->SetTransientMaterializationEnabled(true);

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
            FramebufferTextureFormat::RG16F,       // [3] Screen-space velocity (forward-path TAA input; unused in Deferred, which reads G-Buffer RT3)
            FramebufferTextureFormat::Depth
        };

        FramebufferSpecification finalPassSpec;
        finalPassSpec.Width = width;
        finalPassSpec.Height = height;

        s_Data.ScenePass = Ref<SceneRenderPass>::Create();
        s_Data.ScenePass->SetName("ScenePass");
        s_Data.ScenePass->Init(scenePassSpec);

        // Deferred lighting composition — no-op when Settings.Path is
        // Forward / Forward+ (no G-Buffer supplied). Writes into
        // ScenePass's colour[0] so downstream passes stay path-agnostic.
        s_Data.DeferredLightPass = Ref<DeferredLightingPass>::Create();
        s_Data.DeferredLightPass->SetName("DeferredLightingPass");
        s_Data.DeferredLightPass->Init(scenePassSpec);

        // Graph-scheduled opaque-decal shim. Pulls the decal bucket into
        // the G-Buffer between ScenePass and DeferredLightingPass (was
        // previously a synchronous call inside SceneRenderPass::Execute,
        // now a proper graph node with declared resource edges).
        s_Data.OpaqueDecalPass = Ref<DeferredOpaqueDecalPass>::Create();
        s_Data.OpaqueDecalPass->SetName("DeferredOpaqueDecalPass");
        s_Data.OpaqueDecalPass->Init(scenePassSpec);

        // Forward overlay pass — runs after DeferredLightingPass in Deferred
        // mode to render skybox / terrain / voxel terrain / infinite grid /
        // light-cube geometry that cannot participate in the G-Buffer MRT
        // write. No-ops in Forward / Forward+.
        s_Data.ForwardOverlayPass = Ref<ForwardOverlayRenderPass>::Create();
        s_Data.ForwardOverlayPass->SetName("ForwardOverlayPass");
        s_Data.ForwardOverlayPass->Init(finalPassSpec);

        s_Data.ParticlePass = Ref<ParticleRenderPass>::Create();
        s_Data.ParticlePass->SetName("ParticlePass");
        s_Data.ParticlePass->Init(finalPassSpec);

        s_Data.FoliagePass = Ref<FoliageRenderPass>::Create();
        s_Data.FoliagePass->SetName("FoliagePass");
        s_Data.FoliagePass->Init(finalPassSpec);

        s_Data.WaterPass = Ref<WaterRenderPass>::Create();
        s_Data.WaterPass->SetName("WaterPass");
        s_Data.WaterPass->Init(finalPassSpec);

        s_Data.DecalPass = Ref<DecalRenderPass>::Create();
        s_Data.DecalPass->SetName("DecalPass");
        s_Data.DecalPass->Init(finalPassSpec);

        s_Data.SSAOPass = Ref<SSAORenderPass>::Create();
        s_Data.SSAOPass->SetName("SSAOPass");
        s_Data.SSAOPass->Init(scenePassSpec);
        // Input binding deferred to per-frame handoff in EndScene() — Phase F side-channel removal.
        s_Data.SSAOPass->SetSSAOUBO(s_Data.SSAOUBO, &s_Data.SSAOGPUData);

        s_Data.GTAOPass = Ref<GTAORenderPass>::Create();
        s_Data.GTAOPass->SetName("GTAOPass");
        s_Data.GTAOPass->Init(scenePassSpec);
        // Input binding deferred to per-frame handoff in EndScene() — Phase F side-channel removal.
        s_Data.GTAOPass->SetGTAOUBO(s_Data.GTAOUBO, &s_Data.GTAOGPUData);

        s_Data.SSSPass = Ref<SSSRenderPass>::Create();
        s_Data.SSSPass->SetName("SSSPass");
        s_Data.SSSPass->Init(finalPassSpec);
        // Input binding deferred to per-frame handoff in EndScene() — Phase F side-channel removal.
        s_Data.SSSPass->SetSSSUBO(s_Data.SSSUBO, &s_Data.SSSGPUData);

        // Phase 6: OIT resolve pass. Composites weighted-blended transparent
        // accumulation (produced by ParticlePass when OITEnabled) over the
        // scene FB, then acts as a passthrough for downstream piping.
        s_Data.OITResolvePass = Ref<OITResolveRenderPass>::Create();
        s_Data.OITResolvePass->SetName("OITResolvePass");
        s_Data.OITResolvePass->Init(finalPassSpec);
        // Input binding deferred to per-frame handoff in EndScene() — Phase F side-channel removal.

        // Wire OIT buffer provider + accumulation marker through to
        // ParticlePass. Phase F slice 15 — the buffer is allocated lazily
        // in OITResolveRenderPass; installing a provider callback (rather
        // than caching a Ref) lets paths that never enable OIT skip the
        // ~2x screen-sized RGBA16F+RG16F GPU memory cost.
        {
            Ref<OITResolveRenderPass> oitResolvePassRef = s_Data.OITResolvePass;
            s_Data.ParticlePass->SetOITBufferProvider([oitResolvePassRef]() mutable -> Ref<OITBuffer>
                                                      { return oitResolvePassRef ? oitResolvePassRef->GetOrCreateOITBuffer() : nullptr; });
            s_Data.ParticlePass->SetOITAccumulationMarker([oitResolvePassRef]() mutable
                                                          {
                if (oitResolvePassRef)
                    oitResolvePassRef->MarkAccumulationWritten(); });
        }

        // WaterPass WB-OIT hookup: shader override + provider + marker. The
        // OIT toggle itself is flipped each frame in ApplyRendererSettings;
        // attach the infrastructure here so enabling it has immediate effect.
        if (s_Data.WaterPass)
        {
            Ref<OITResolveRenderPass> oitResolvePassRef = s_Data.OITResolvePass;
            s_Data.WaterPass->SetOITBufferProvider([oitResolvePassRef]() mutable -> Ref<OITBuffer>
                                                   { return oitResolvePassRef ? oitResolvePassRef->GetOrCreateOITBuffer() : nullptr; });
            s_Data.WaterPass->SetOITShader(m_ShaderLibrary.Get("Water_OIT"));
            s_Data.WaterPass->SetOITAccumulationMarker([oitResolvePassRef]() mutable
                                                       {
                if (oitResolvePassRef)
                    oitResolvePassRef->MarkAccumulationWritten(); });
        }

        // DecalPass WB-OIT hookup (forward path only — deferred uses
        // G-Buffer variants via ExecuteOnGBuffer which is already order-
        // independent).
        if (s_Data.DecalPass)
        {
            Ref<OITResolveRenderPass> oitResolvePassRef = s_Data.OITResolvePass;
            s_Data.DecalPass->SetOITBufferProvider([oitResolvePassRef]() mutable -> Ref<OITBuffer>
                                                   { return oitResolvePassRef ? oitResolvePassRef->GetOrCreateOITBuffer() : nullptr; });
            s_Data.DecalPass->SetOITShader(m_ShaderLibrary.Get("Decal_OIT"));
            s_Data.DecalPass->SetOITAccumulationMarker([oitResolvePassRef]() mutable
                                                       {
                if (oitResolvePassRef)
                    oitResolvePassRef->MarkAccumulationWritten(); });
        }

        s_Data.PostProcessPass = Ref<PostProcessRenderPass>::Create();
        s_Data.PostProcessPass->SetName("PostProcessPass");
        s_Data.PostProcessPass->Init(finalPassSpec);

        // Phase F slice 24 — AO Apply extracted from PostProcessRenderPass.
        // Sits between SSSPass (or SceneColor) and PostProcessPass.
        s_Data.AOApplyPass = Ref<AOApplyRenderPass>::Create();
        s_Data.AOApplyPass->SetName("AOApplyPass");
        s_Data.AOApplyPass->Init(finalPassSpec);

        // Phase F slice 23 — Bloom extracted from PostProcessRenderPass.
        // Sits between PostProcess and DOF.
        s_Data.BloomPass = Ref<BloomRenderPass>::Create();
        s_Data.BloomPass->SetName("BloomPass");
        s_Data.BloomPass->Init(finalPassSpec);

        // Phase F slice 22 — DOF extracted from PostProcessRenderPass.
        // Sits between Bloom and MotionBlur.
        s_Data.DOFPass = Ref<DOFRenderPass>::Create();
        s_Data.DOFPass->SetName("DOFPass");
        s_Data.DOFPass->Init(finalPassSpec);

        // Phase F slice 21 — MotionBlur extracted from PostProcessRenderPass.
        // Sits between DOF and TAA.
        s_Data.MotionBlurPass = Ref<MotionBlurRenderPass>::Create();
        s_Data.MotionBlurPass->SetName("MotionBlurPass");
        s_Data.MotionBlurPass->Init(finalPassSpec);

        // Phase F slice 19 — TAA extracted from PostProcessRenderPass.
        // Sits between PostProcess and Fog.
        s_Data.TAAPass = Ref<TAARenderPass>::Create();
        s_Data.TAAPass->SetName("TAAPass");
        s_Data.TAAPass->Init(finalPassSpec);

        // Phase F slice 20 — screen-space precipitation extracted from PostProcessRenderPass.
        // Sits between TAA and Fog.
        s_Data.PrecipitationPass = Ref<PrecipitationRenderPass>::Create();
        s_Data.PrecipitationPass->SetName("PrecipitationPass");
        s_Data.PrecipitationPass->Init(finalPassSpec);

        // Phase F slice 18 — volumetric fog extracted from PostProcessRenderPass.
        // Sits between Precipitation and the slice-17 sub-chain.
        s_Data.FogPass = Ref<FogRenderPass>::Create();
        s_Data.FogPass->SetName("FogPass");
        s_Data.FogPass->Init(finalPassSpec);

        // Phase F slice 17 — four effects extracted from PostProcessRenderPass
        // in chain order. Each pass self-skips when its effect is disabled;
        // the graph topology stays constant regardless of settings.
        s_Data.ChromAberrationPass = Ref<ChromaticAberrationRenderPass>::Create();
        s_Data.ChromAberrationPass->SetName("ChromAberrationPass");
        s_Data.ChromAberrationPass->Init(finalPassSpec);

        s_Data.ColorGradingPass = Ref<ColorGradingRenderPass>::Create();
        s_Data.ColorGradingPass->SetName("ColorGradingPass");
        s_Data.ColorGradingPass->Init(finalPassSpec);

        s_Data.ToneMapPass = Ref<ToneMapRenderPass>::Create();
        s_Data.ToneMapPass->SetName("ToneMapPass");
        s_Data.ToneMapPass->Init(finalPassSpec);

        s_Data.VignettePass = Ref<VignetteRenderPass>::Create();
        s_Data.VignettePass->SetName("VignettePass");
        s_Data.VignettePass->Init(finalPassSpec);

        // Phase F slice 16 — FXAA extracted into its own graph pass.
        // Always created so the graph topology can stay constant; the
        // pass self-skips when `Settings.FXAAEnabled` is false and the
        // blackboard import is gated on the same flag.
        s_Data.FXAAPass = Ref<FXAARenderPass>::Create();
        s_Data.FXAAPass->SetName("FXAAPass");
        s_Data.FXAAPass->Init(finalPassSpec);

        if (s_Data.EnableSelectionOutline)
        {
            s_Data.SelectionOutlinePass = Ref<SelectionOutlineRenderPass>::Create();
            s_Data.SelectionOutlinePass->SetName("SelectionOutlinePass");
            s_Data.SelectionOutlinePass->Init(finalPassSpec);
        }

        s_Data.UICompositePass = Ref<UICompositeRenderPass>::Create();
        s_Data.UICompositePass->SetName("UICompositePass");
        s_Data.UICompositePass->Init(finalPassSpec);

        s_Data.FinalPass = Ref<FinalRenderPass>::Create();
        s_Data.FinalPass->SetName("FinalPass");
        s_Data.FinalPass->Init(finalPassSpec);

        // All passes are now constructed. Build the initial graph topology
        // for the currently-configured rendering path. Runtime switches
        // between Forward / Forward+ / Deferred re-run ConfigureRenderGraph
        // from ApplyRendererSettings so the graph only ever contains the
        // passes that are relevant for the active path.
        ConfigureRenderGraph(s_Data.Settings.Path);
    }

    void Renderer3D::ConfigureRenderGraph(RenderingPath path)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Renderer3D: Configuring RenderGraph for path = {}",
                      path == RenderingPath::Forward       ? "Forward"
                      : path == RenderingPath::ForwardPlus ? "Forward+"
                                                           : "Deferred");

        // Wipe any prior topology. Passes themselves are owned by s_Data
        // as Ref<>s and survive the reset — only the graph's bookkeeping
        // (pass lookup, edges, framebuffer piping, cached execution order)
        // is cleared.
        s_Data.RGraph->ResetTopology();
        s_Data.RGraph->ClearGraphPasses();

        const bool deferred = (path == RenderingPath::Deferred);

        // Core passes shared by every path
        s_Data.RGraph->AddPass(s_Data.ShadowPass);
        s_Data.RGraph->AddPass(s_Data.ScenePass);

        // Deferred-only passes: the G-Buffer lighting composition + the
        // forward overlay that renders skybox/terrain/voxel/grid/debug
        // geometry on top of the lit result. In Forward / Forward+ these
        // are simply NOT registered, so the graph executor never dispatches
        // them and the execution edges that reference them are skipped.
        if (deferred)
        {
            // Graph-scheduled opaque decal drain — sits between ScenePass
            // and DeferredLightingPass so the "opaque decals composite
            // into the G-Buffer before lighting" contract is visible in
            // the render graph rather than being an implicit side-effect
            // of SceneRenderPass::Execute calling ExecuteOnGBuffer().
            if (s_Data.OpaqueDecalPass)
                s_Data.RGraph->AddPass(s_Data.OpaqueDecalPass);
            s_Data.RGraph->AddPass(s_Data.DeferredLightPass);
            s_Data.RGraph->AddPass(s_Data.ForwardOverlayPass);
        }

        s_Data.RGraph->AddPass(s_Data.FoliagePass);
        // Keep insertion order aligned with the explicit baseline dependency
        // chain below (Foliage -> Decal -> Water). BuildFrameGraph derives
        // WAW edges in insertion order; inserting Water before Decal can
        // derive Water -> Decal and create a Decal <-> Water cycle.
        s_Data.RGraph->AddPass(s_Data.DecalPass);
        s_Data.RGraph->AddPass(s_Data.WaterPass);
        // Phase F slice 34 — register only the active AO pass so there is
        // never a WAW on AOBuffer to resolve with an explicit edge.
        switch (s_Data.PostProcess.ActiveAOTechnique)
        {
            case AOTechnique::SSAO:
                s_Data.RGraph->AddPass(s_Data.SSAOPass);
                break;
            case AOTechnique::GTAO:
                s_Data.RGraph->AddPass(s_Data.GTAOPass);
                break;
            case AOTechnique::None:
                break;
        }
        s_Data.RGraph->AddPass(s_Data.ParticlePass);
        s_Data.RGraph->AddPass(s_Data.OITResolvePass);
        s_Data.RGraph->AddPass(s_Data.SSSPass);
        s_Data.RGraph->AddPass(s_Data.AOApplyPass);
        s_Data.RGraph->AddPass(s_Data.PostProcessPass);
        s_Data.RGraph->AddPass(s_Data.BloomPass);
        s_Data.RGraph->AddPass(s_Data.DOFPass);
        s_Data.RGraph->AddPass(s_Data.MotionBlurPass);
        s_Data.RGraph->AddPass(s_Data.TAAPass);
        s_Data.RGraph->AddPass(s_Data.PrecipitationPass);
        s_Data.RGraph->AddPass(s_Data.FogPass);
        s_Data.RGraph->AddPass(s_Data.ChromAberrationPass);
        s_Data.RGraph->AddPass(s_Data.ColorGradingPass);
        s_Data.RGraph->AddPass(s_Data.ToneMapPass);
        s_Data.RGraph->AddPass(s_Data.VignettePass);
        if (s_Data.FXAAPass)
        {
            s_Data.RGraph->AddPass(s_Data.FXAAPass);
        }
        if (s_Data.EnableSelectionOutline)
        {
            s_Data.RGraph->AddPass(s_Data.SelectionOutlinePass);
        }
        s_Data.RGraph->AddPass(s_Data.UICompositePass);
        s_Data.RGraph->AddPass(s_Data.FinalPass);

        // Baseline ordering edges required by ConfigureRenderGraph-time
        // hazard validation. BuildFrameGraph() still derives additional edges
        // from setup declarations at runtime.
        //
        // Phase F slice 27 — ShadowPass→ScenePass has been removed from the
        // explicit baseline.  ShadowRenderPass::Init DeclareWrite(ShadowMapCSM/
        // Spot/Point); SceneRenderPass::Init DeclareRead(ShadowMapCSM).
        // ValidateResourceHazards now derives the RAW edge from those
        // declarations, so no explicit AddExecutionDependency call is needed.
        // BuildFrameGraph() also derives the edge at runtime from RegisterGraphPass
        // builder declarations.

        // Phase F slice 34 — SSAOPass→GTAOPass explicit edge removed.
        // The WAW on AOBuffer is eliminated at the source: only the pass for
        // the active ActiveAOTechnique (SSAO/GTAO/None) is registered above,
        // so at most one AOBuffer writer is ever in the graph.
        // ApplyRendererSettings detects ActiveAOTechnique changes and calls
        // ConfigureRenderGraph to rebuild the topology with the new pass set.
        //
        // Phase F slice 33 — all five deferred-path explicit edges are now
        // derived from declaration pairs:
        //   ScenePass              DeclareWrite(SceneDepth)
        //   DeferredOpaqueDecalPass DeclareRead(SceneDepth)              → RAW (ScenePass→DeferredOpaqueDecalPass)
        //   DeferredOpaqueDecalPass DeclareWrite(SceneColor)
        //   DeferredLightingPass   DeclareRead(SceneColor)               → RAW (DeferredOpaqueDecalPass→DeferredLightingPass)
        //   DeferredLightingPass   DeclareRead(SceneDepth/SceneNormals)  → RAW (ScenePass→DeferredLightingPass fallback)
        //   DeferredLightingPass   DeclareWrite(SceneColor)
        //   ForwardOverlayPass     DeclareRead(SceneColor)               → RAW (DeferredLightingPass→ForwardOverlayPass)
        //   ForwardOverlayPass     DeclareWrite(SceneColor)
        //   FoliagePass            DeclareRead(SceneColor)               → RAW (ForwardOverlayPass→FoliagePass)
        //
        // Phase F slice 32 — ScenePass→FoliagePass, FoliagePass→DecalPass,
        // DecalPass→WaterPass, and WaterPass→ParticlePass are now derived from
        // SceneColor read-modify-write declarations on each pass:
        //   ScenePass       DeclareWrite(SceneColor)
        //   FoliagePass     DeclareRead(SceneColor)   → RAW edge (ScenePass→FoliagePass)
        //   FoliagePass     DeclareWrite(SceneColor)
        //   DecalPass       DeclareRead(SceneColor)   → RAW edge (FoliagePass→DecalPass)
        //   DecalPass       DeclareWrite(SceneColor)
        //   WaterPass       DeclareRead(SceneColor)   → RAW edge (DecalPass→WaterPass)
        //   WaterPass       DeclareWrite(SceneColor)
        //   ParticlePass    DeclareRead(SceneColor)   → RAW edge (WaterPass→ParticlePass)
        //   ParticlePass    DeclareWrite(SceneColor)

        // Phase F slice 31 — WaterPass→SSAOPass and WaterPass→GTAOPass are
        // now removed from the explicit baseline. ScenePass declares
        // SceneDepth writes, and SSAO/GTAO declare SceneDepth reads, so
        // ValidateResourceHazards derives ScenePass→SSAOPass / ScenePass→GTAOPass
        // as RAW edges automatically.

        // Phase F slice 30 — SSAOPass/GTAOPass now declare AOBuffer writes and
        // AOApplyPass declares AOBuffer reads, so AO->AOApply RAW edges are
        // derived and explicit SSAO->Particle / GTAO->Particle edges are no
        // longer needed. Keep an explicit SSAOPass->GTAOPass edge because both
        // passes are registered in the static topology and both declare writes
        // to AOBuffer; this serialises dual writers (WAW).

        // Phase F slice 29 — Particle→OITResolve, OITResolve→SSS, SSS→AOApply
        // are all derived from DeclareRead/DeclareWrite pairs:
        //   ParticlePass  DeclareWrite(OITAccum, OITRevealage)
        //   OITResolvePass DeclareRead(OITAccum, OITRevealage)   → RAW edge
        //   OITResolvePass DeclareWrite(SceneColor)
        //   SSSPass        DeclareRead(SceneColor)                → RAW edge
        //   SSSPass        DeclareWrite(SSSColor)
        //   AOApplyPass    DeclareRead(SSSColor)                  → RAW edge
        // Phase F slice 28 — AOApplyPass→PostProcessPass derived from:
        //   AOApplyPass  DeclareWrite(AOApplyColor)
        //   PostProcessPass DeclareRead(AOApplyColor)
        // Slice 28 — entire post-process linear sub-chain is now derived from
        // matching DeclareWrite/DeclareRead pairs on adjacent passes:
        //   PostProcess  → Bloom    (PostProcessColor)
        //   Bloom        → DOF      (BloomColor)
        //   DOF          → MBlur    (DOFColor)
        //   MBlur        → TAA      (MotionBlurColor)
        //   TAA          → Precip   (TAAColor)
        //   Precip       → Fog      (PrecipitationColor)
        //   Fog          → ChromAb  (FogColor)
        //   ChromAb      → CG       (ChromAbColor)
        //   CG           → ToneMap  (ColorGradingColor)
        //   ToneMap      → Vignette (ToneMapColor)
        //   Vignette     → FXAA/SelectionOutline/UIComposite (VignetteColor)
        //   FXAA         → SelectionOutline/UIComposite       (FXAAColor)
        //   SelectOutline→ UIComposite                        (SelectionOutlineColor)
        // ValidateResourceHazards synthesises all of these RAW edges.

        // Phase F slice 27 — UICompositePass→FinalPass has been removed from
        // the explicit baseline.  UICompositeRenderPass::Init DeclareWrite(
        // UIComposite); FinalRenderPass::Init DeclareRead(UIComposite).
        // ValidateResourceHazards derives the RAW edge from those declarations.
        // BuildFrameGraph() also derives the edge at runtime.
        // FinalPass is still marked as the final/sink pass via SetFinalPass(),
        // which is the real guarantee that it executes last.

        // Phase C bridge: register setup declarations for selected production
        // passes so BuildFrameGraph() can derive ordering edges from resource
        // access contracts. Execute remains legacy RenderPass::Execute for now.
        s_Data.RGraph->RegisterGraphPass(
            "ScenePass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();

                if (board.ShadowMapCSM.IsValid())
                {
                    [[maybe_unused]] const auto shadowCSMRead = builder.Read(board.ShadowMapCSM, RGReadUsage::ShaderSample);
                }
                if (board.ShadowMapSpot.IsValid())
                {
                    [[maybe_unused]] const auto shadowSpotRead = builder.Read(board.ShadowMapSpot, RGReadUsage::ShaderSample);
                }
                // Phase F slice 26 — per-light point shadow cubemaps (up to 4, bindings 14-17).
                for (const auto& pointHandle : board.ShadowMapPoint)
                {
                    if (pointHandle.IsValid())
                    {
                        [[maybe_unused]] const auto pointRead = builder.Read(pointHandle, RGReadUsage::ShaderSample);
                    }
                }

                if (board.SceneDepth.IsValid())
                    builder.Write(board.SceneDepth, RGWriteUsage::DepthStencil);
                if (board.Velocity.IsValid())
                    builder.Write(board.Velocity, RGWriteUsage::RenderTarget);

                if (s_Data.Settings.Path == RenderingPath::Deferred)
                {
                    if (board.GBufferAlbedo.IsValid())
                        builder.Write(board.GBufferAlbedo, RGWriteUsage::RenderTarget);
                    if (board.GBufferNormal.IsValid())
                        builder.Write(board.GBufferNormal, RGWriteUsage::RenderTarget);
                    if (board.GBufferEmissive.IsValid())
                        builder.Write(board.GBufferEmissive, RGWriteUsage::RenderTarget);
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "SSAOPass",
            [](RGBuilder& builder)
            {
                if (s_Data.PostProcess.ActiveAOTechnique != AOTechnique::SSAO ||
                    !s_Data.PostProcess.SSAOEnabled)
                {
                    return;
                }

                const auto& board = builder.UseBlackboard();
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.SceneNormals.IsValid())
                {
                    [[maybe_unused]] const auto sceneNormalsRead = builder.Read(board.SceneNormals, RGReadUsage::ShaderSample);
                }
                if (board.AOBuffer.IsValid())
                    builder.Write(board.AOBuffer, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "GTAOPass",
            [](RGBuilder& builder)
            {
                if (s_Data.PostProcess.ActiveAOTechnique != AOTechnique::GTAO ||
                    !s_Data.PostProcess.GTAOEnabled)
                {
                    return;
                }

                const auto& board = builder.UseBlackboard();
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.SceneNormals.IsValid())
                {
                    [[maybe_unused]] const auto sceneNormalsRead = builder.Read(board.SceneNormals, RGReadUsage::ShaderSample);
                }
                if (board.AOBuffer.IsValid())
                    builder.Write(board.AOBuffer, RGWriteUsage::ShaderImage);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "AOApplyPass",
            [](RGBuilder& builder)
            {
                const bool aoApplyEnabled =
                    (s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO && s_Data.PostProcess.SSAOEnabled) ||
                    (s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled);
                if (!aoApplyEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.SSSColor.IsValid())
                {
                    [[maybe_unused]] const auto sssColorRead = builder.Read(board.SSSColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.SceneColor.IsValid())
                {
                    [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
                }
                if (board.AOBuffer.IsValid())
                {
                    [[maybe_unused]] const auto aoBufferRead = builder.Read(board.AOBuffer, RGReadUsage::ShaderSample);
                }
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.AOApplyColor.IsValid())
                    builder.Write(board.AOApplyColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "PostProcessPass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();

                // Phase F slice 24 — prefer AOApplyColor (AO already applied),
                // then fall back through SSSColor and SceneColor.
                if (board.AOApplyColor.IsValid())
                {
                    [[maybe_unused]] const auto aoApplyRead = builder.Read(board.AOApplyColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.SSSColor.IsValid())
                {
                    [[maybe_unused]] const auto sssColorRead = builder.Read(board.SSSColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.SceneColor.IsValid())
                {
                    [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
                }
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.Velocity.IsValid())
                {
                    [[maybe_unused]] const auto velocityRead = builder.Read(board.Velocity, RGReadUsage::ShaderSample);
                }
                // Phase F slice 24 — AOBuffer is now read by AOApplyPass; only read
                // here when AOApply is not handled externally (inline fallback path).
                const bool aoApplyEnabled =
                    (s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO && s_Data.PostProcess.SSAOEnabled) ||
                    (s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled);
                if (!s_Data.AOApplyPass && aoApplyEnabled && board.AOBuffer.IsValid())
                {
                    [[maybe_unused]] const auto aoBufferRead = builder.Read(board.AOBuffer, RGReadUsage::ShaderSample);
                }
                if (board.FogHistory.IsValid())
                {
                    [[maybe_unused]] const auto fogHistoryRead = builder.Read(board.FogHistory, RGReadUsage::ShaderSample);
                }

                if (board.PostProcessColor.IsValid())
                    builder.Write(board.PostProcessColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "BloomPass",
            [](RGBuilder& builder)
            {
                if (!s_Data.PostProcess.BloomEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.BloomColor.IsValid())
                    builder.Write(board.BloomColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "DOFPass",
            [](RGBuilder& builder)
            {
                if (!s_Data.PostProcess.DOFEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.DOFColor.IsValid())
                    builder.Write(board.DOFColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "MotionBlurPass",
            [](RGBuilder& builder)
            {
                if (!s_Data.PostProcess.MotionBlurEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.MotionBlurColor.IsValid())
                    builder.Write(board.MotionBlurColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "TAAPass",
            [](RGBuilder& builder)
            {
                if (!s_Data.PostProcess.TAAEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.Velocity.IsValid())
                {
                    [[maybe_unused]] const auto velocityRead = builder.Read(board.Velocity, RGReadUsage::ShaderSample);
                }
                if (board.TAAHistory.IsValid())
                {
                    [[maybe_unused]] const auto taaHistoryRead = builder.Read(board.TAAHistory, RGReadUsage::ShaderSample);
                }
                if (board.TAAColor.IsValid())
                    builder.Write(board.TAAColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "PrecipitationPass",
            [](RGBuilder& builder)
            {
                // Phase F slice 20 — gated by precipitation screen FX flag.
                const bool precipEnabled = s_Data.Precipitation.Enabled &&
                                           (s_Data.Precipitation.ScreenStreaksEnabled ||
                                            s_Data.Precipitation.LensImpactsEnabled);
                if (!precipEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.PrecipitationColor.IsValid())
                    builder.Write(board.PrecipitationColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "FogPass",
            [](RGBuilder& builder)
            {
                // Phase F slice 18 — gated by fog enabled flag.
                if (!s_Data.Fog.Enabled)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.FogColor.IsValid())
                    builder.Write(board.FogColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "ChromAberrationPass",
            [](RGBuilder& builder)
            {
                // Phase F slice 17 — gated by settings flag so the pass
                // produces zero graph edges when ChromAb is disabled.
                if (!s_Data.PostProcess.ChromaticAberrationEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                // Phase F slices 18-20 — prefer FogColor, then PrecipitationColor, then TAAColor.
                if (board.FogColor.IsValid())
                {
                    [[maybe_unused]] const auto fogRead = builder.Read(board.FogColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.ChromAbColor.IsValid())
                    builder.Write(board.ChromAbColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "ColorGradingPass",
            [](RGBuilder& builder)
            {
                if (!s_Data.PostProcess.ColorGradingEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                // Prefer most recent valid handle in the sub-chain.
                if (board.ChromAbColor.IsValid())
                {
                    [[maybe_unused]] const auto chromAbRead = builder.Read(board.ChromAbColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.FogColor.IsValid())
                {
                    [[maybe_unused]] const auto fogRead = builder.Read(board.FogColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.ColorGradingColor.IsValid())
                    builder.Write(board.ColorGradingColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "ToneMapPass",
            [](RGBuilder& builder)
            {
                // Tone mapping always runs — no settings gate.
                const auto& board = builder.UseBlackboard();

                if (board.ColorGradingColor.IsValid())
                {
                    [[maybe_unused]] const auto colorGradingRead = builder.Read(board.ColorGradingColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ChromAbColor.IsValid())
                {
                    [[maybe_unused]] const auto chromAbRead = builder.Read(board.ChromAbColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.FogColor.IsValid())
                {
                    [[maybe_unused]] const auto fogRead = builder.Read(board.FogColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.ToneMapColor.IsValid())
                    builder.Write(board.ToneMapColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "VignettePass",
            [](RGBuilder& builder)
            {
                if (!s_Data.PostProcess.VignetteEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                // Prefer most recent valid handle in the sub-chain.
                if (board.ToneMapColor.IsValid())
                {
                    [[maybe_unused]] const auto toneMapRead = builder.Read(board.ToneMapColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ColorGradingColor.IsValid())
                {
                    [[maybe_unused]] const auto colorGradingRead = builder.Read(board.ColorGradingColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ChromAbColor.IsValid())
                {
                    [[maybe_unused]] const auto chromAbRead = builder.Read(board.ChromAbColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.FogColor.IsValid())
                {
                    [[maybe_unused]] const auto fogRead = builder.Read(board.FogColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
                if (board.VignetteColor.IsValid())
                    builder.Write(board.VignetteColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "FXAAPass",
            [](RGBuilder& builder)
            {
                // Phase F slice 16 — read/write only declared when FXAA is
                // enabled. Renderer3D gates the blackboard import on the
                // same flag so `board.FXAAColor.IsValid()` is the canonical
                // signal for downstream consumers.
                if (!s_Data.PostProcess.FXAAEnabled)
                    return;

                const auto& board = builder.UseBlackboard();

                // Phase F slices 16-21 — prefer the most recent valid
                // handle in the extracted sub-chain as FXAA input.
                if (board.VignetteColor.IsValid())
                {
                    [[maybe_unused]] const auto vignetteRead = builder.Read(board.VignetteColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ToneMapColor.IsValid())
                {
                    [[maybe_unused]] const auto toneMapRead = builder.Read(board.ToneMapColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ColorGradingColor.IsValid())
                {
                    [[maybe_unused]] const auto colorGradingRead = builder.Read(board.ColorGradingColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ChromAbColor.IsValid())
                {
                    [[maybe_unused]] const auto chromAbRead = builder.Read(board.ChromAbColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.FogColor.IsValid())
                {
                    [[maybe_unused]] const auto fogRead = builder.Read(board.FogColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }

                if (board.FXAAColor.IsValid())
                    builder.Write(board.FXAAColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "SelectionOutlinePass",
            [](RGBuilder& builder)
            {
                if (!s_Data.EnableSelectionOutline)
                    return;

                const auto& board = builder.UseBlackboard();

                // Phase F slices 16-21 — prefer the most recent valid
                // handle in the extracted post-process sub-chain.
                if (board.FXAAColor.IsValid())
                {
                    [[maybe_unused]] const auto fxaaRead = builder.Read(board.FXAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.VignetteColor.IsValid())
                {
                    [[maybe_unused]] const auto vignetteRead = builder.Read(board.VignetteColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ToneMapColor.IsValid())
                {
                    [[maybe_unused]] const auto toneMapRead = builder.Read(board.ToneMapColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.FogColor.IsValid())
                {
                    [[maybe_unused]] const auto fogRead = builder.Read(board.FogColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }

                if (board.SelectionOutlineColor.IsValid())
                    builder.Write(board.SelectionOutlineColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "UICompositePass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();

                if (board.UIComposite.IsValid())
                    builder.Write(board.UIComposite, RGWriteUsage::RenderTarget);

                if (s_Data.EnableSelectionOutline && board.SelectionOutlineColor.IsValid())
                {
                    [[maybe_unused]] const auto selectionOutlineRead = builder.Read(board.SelectionOutlineColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.FXAAColor.IsValid())
                {
                    [[maybe_unused]] const auto fxaaRead = builder.Read(board.FXAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.VignetteColor.IsValid())
                {
                    [[maybe_unused]] const auto vignetteRead = builder.Read(board.VignetteColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.ToneMapColor.IsValid())
                {
                    [[maybe_unused]] const auto toneMapRead = builder.Read(board.ToneMapColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.FogColor.IsValid())
                {
                    [[maybe_unused]] const auto fogRead = builder.Read(board.FogColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PrecipitationColor.IsValid())
                {
                    [[maybe_unused]] const auto precipRead = builder.Read(board.PrecipitationColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.TAAColor.IsValid())
                {
                    [[maybe_unused]] const auto taaRead = builder.Read(board.TAAColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.MotionBlurColor.IsValid())
                {
                    [[maybe_unused]] const auto motionBlurRead = builder.Read(board.MotionBlurColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.DOFColor.IsValid())
                {
                    [[maybe_unused]] const auto dofRead = builder.Read(board.DOFColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.BloomColor.IsValid())
                {
                    [[maybe_unused]] const auto bloomRead = builder.Read(board.BloomColor, RGReadUsage::RenderTargetRead);
                }
                else if (board.PostProcessColor.IsValid())
                {
                    [[maybe_unused]] const auto postProcessColorRead = builder.Read(board.PostProcessColor, RGReadUsage::RenderTargetRead);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "FinalPass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();

                if (board.UIComposite.IsValid())
                {
                    [[maybe_unused]] const auto uiCompositeRead = builder.Read(board.UIComposite, RGReadUsage::RenderTargetRead);
                }

                if (board.Backbuffer.IsValid())
                    builder.Write(board.Backbuffer, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "ParticlePass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();
                const bool oitEnabled = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                                        s_Data.Settings.Deferred.OITEnabled;

                if (oitEnabled)
                {
                    if (board.OITAccum.IsValid())
                        builder.Write(board.OITAccum, RGWriteUsage::RenderTarget);
                    if (board.OITRevealage.IsValid())
                        builder.Write(board.OITRevealage, RGWriteUsage::RenderTarget);
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "WaterPass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();
                const bool oitEnabled = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                                        s_Data.Settings.Deferred.OITEnabled;

                if (oitEnabled)
                {
                    if (board.OITAccum.IsValid())
                        builder.Write(board.OITAccum, RGWriteUsage::RenderTarget);
                    if (board.OITRevealage.IsValid())
                        builder.Write(board.OITRevealage, RGWriteUsage::RenderTarget);
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "DecalPass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();
                const bool oitEnabled = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                                        s_Data.Settings.Deferred.OITEnabled;

                if (oitEnabled)
                {
                    if (board.OITAccum.IsValid())
                        builder.Write(board.OITAccum, RGWriteUsage::RenderTarget);
                    if (board.OITRevealage.IsValid())
                        builder.Write(board.OITRevealage, RGWriteUsage::RenderTarget);
                }
                else if (board.SceneColor.IsValid())
                {
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "OITResolvePass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();

                if (board.OITAccum.IsValid())
                {
                    [[maybe_unused]] const auto oitAccumRead = builder.Read(board.OITAccum, RGReadUsage::RenderTargetRead);
                }
                if (board.OITRevealage.IsValid())
                {
                    [[maybe_unused]] const auto oitRevealageRead = builder.Read(board.OITRevealage, RGReadUsage::RenderTargetRead);
                }

                if (board.SceneColor.IsValid())
                {
                    [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "SSSPass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();
                if (!s_Data.Snow.SSSBlurEnabled || !s_Data.SSSPass)
                    return;

                if (board.SceneColor.IsValid())
                {
                    [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
                }
                if (board.SSSColor.IsValid())
                {
                    builder.Write(board.SSSColor, RGWriteUsage::RenderTarget);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "ShadowPass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();

                if (board.ShadowMapCSM.IsValid())
                    builder.Write(board.ShadowMapCSM, RGWriteUsage::DepthStencil);
                if (board.ShadowMapSpot.IsValid())
                    builder.Write(board.ShadowMapSpot, RGWriteUsage::DepthStencil);
                // Phase F slice 26 — per-light point shadow cubemaps (up to 4).
                for (const auto& pointHandle : board.ShadowMapPoint)
                {
                    if (pointHandle.IsValid())
                        builder.Write(pointHandle, RGWriteUsage::DepthStencil);
                }
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "DeferredLightingPass",
            [](RGBuilder& builder)
            {
                if (s_Data.Settings.Path != RenderingPath::Deferred)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.GBufferAlbedo.IsValid())
                {
                    [[maybe_unused]] const auto gbufferAlbedoRead = builder.Read(board.GBufferAlbedo, RGReadUsage::ShaderSample);
                }
                if (board.GBufferNormal.IsValid())
                {
                    [[maybe_unused]] const auto gbufferNormalRead = builder.Read(board.GBufferNormal, RGReadUsage::ShaderSample);
                }
                if (board.GBufferEmissive.IsValid())
                {
                    [[maybe_unused]] const auto gbufferEmissiveRead = builder.Read(board.GBufferEmissive, RGReadUsage::ShaderSample);
                }
                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }
                if (board.ShadowMapCSM.IsValid())
                {
                    [[maybe_unused]] const auto shadowCSMRead = builder.Read(board.ShadowMapCSM, RGReadUsage::ShaderSample);
                }
                if (board.ShadowMapSpot.IsValid())
                {
                    [[maybe_unused]] const auto shadowSpotRead = builder.Read(board.ShadowMapSpot, RGReadUsage::ShaderSample);
                }
                // Phase F slice 26 — per-light point shadow cubemaps (up to 4, bindings 14-17).
                for (const auto& pointHandle : board.ShadowMapPoint)
                {
                    if (pointHandle.IsValid())
                    {
                        [[maybe_unused]] const auto pointRead = builder.Read(pointHandle, RGReadUsage::ShaderSample);
                    }
                }
                if (board.AOBuffer.IsValid())
                {
                    [[maybe_unused]] const auto aoRead = builder.Read(board.AOBuffer, RGReadUsage::ShaderSample);
                }
                if (board.IrradianceMap.IsValid())
                {
                    [[maybe_unused]] const auto irradianceRead = builder.Read(board.IrradianceMap, RGReadUsage::ShaderSample);
                }
                if (board.PrefilterMap.IsValid())
                {
                    [[maybe_unused]] const auto prefilterRead = builder.Read(board.PrefilterMap, RGReadUsage::ShaderSample);
                }
                if (board.BrdfLut.IsValid())
                {
                    [[maybe_unused]] const auto brdfRead = builder.Read(board.BrdfLut, RGReadUsage::ShaderSample);
                }

                if (board.SceneColor.IsValid())
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "DeferredOpaqueDecalPass",
            [](RGBuilder& builder)
            {
                if (s_Data.Settings.Path != RenderingPath::Deferred)
                    return;

                const auto& board = builder.UseBlackboard();

                if (board.SceneDepth.IsValid())
                {
                    [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
                }

                if (board.GBufferAlbedo.IsValid())
                    builder.Write(board.GBufferAlbedo, RGWriteUsage::RenderTarget);
                if (board.GBufferNormal.IsValid())
                    builder.Write(board.GBufferNormal, RGWriteUsage::RenderTarget);
                if (board.GBufferEmissive.IsValid())
                    builder.Write(board.GBufferEmissive, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "ForwardOverlayPass",
            [](RGBuilder& builder)
            {
                if (s_Data.Settings.Path != RenderingPath::Deferred)
                    return;

                const auto& board = builder.UseBlackboard();
                if (board.SceneColor.IsValid())
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        s_Data.RGraph->RegisterGraphPass(
            "FoliagePass",
            [](RGBuilder& builder)
            {
                const auto& board = builder.UseBlackboard();
                if (board.SceneColor.IsValid())
                    builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            },
            [](RGCommandContext& context)
            {
                (void)context;
            });

        // PostProcessPass input binding is handled per-frame in EndScene() — Phase F side-channel removal.
        // Depth / AO / velocity / shadow inputs are now passed as typed graph handles per frame.

        if (deferred)
        {
            OLO_CORE_INFO("Renderer3D: Render graph (Deferred): Shadow -> Scene -> DeferredOpaqueDecal -> DeferredLighting -> ForwardOverlay -> Foliage -> Decal -> Water -> SSAO/GTAO -> Particle -> OITResolve -> SSS -> PostProcess{} -> UIComposite -> Final",
                          s_Data.EnableSelectionOutline ? " -> SelectionOutline" : "");
        }
        else
        {
            OLO_CORE_INFO("Renderer3D: Render graph ({}): Shadow -> Scene -> Foliage -> Decal -> Water -> SSAO/GTAO -> Particle -> OITResolve -> SSS -> PostProcess{} -> UIComposite -> Final",
                          path == RenderingPath::ForwardPlus ? "Forward+" : "Forward",
                          s_Data.EnableSelectionOutline ? " -> SelectionOutline" : "");
        }

        s_Data.RGraph->SetFinalPass("FinalPass");

        // Validate the resource-aware RDG contract: every pass's declared
        // reads must have a transitive execution dependency on their
        // producer. Hazards are logged to the engine logger; in debug
        // builds we additionally assert so regressions surface immediately.
        {
            const auto hazards = s_Data.RGraph->ValidateResourceHazards();
            if (!hazards.empty())
            {
                // Any Cycle entry means validation couldn't even run;
                // surface that distinctly from genuine resource hazards.
                const bool cycle = std::any_of(hazards.begin(), hazards.end(),
                                               [](const auto& h)
                                               { return h.Kind == RenderGraph::HazardKind::Cycle; });
                if (cycle)
                {
                    OLO_CORE_ERROR("Renderer3D: RenderGraph dependency cycle detected — resource hazard validation aborted.");
                    OLO_CORE_ASSERT(!cycle, "RenderGraph dependency cycle detected. Break the cycle and retry.");
                }
                else
                {
                    OLO_CORE_ERROR("Renderer3D: RenderGraph has {} resource hazards — see previous log entries for details.", hazards.size());
                    OLO_CORE_ASSERT(hazards.empty(), "RenderGraph resource hazard detected (see log). Add ConnectPass / AddExecutionDependency for the reported producer -> consumer edge.");
                }
            }
            else
            {
                OLO_CORE_INFO("Renderer3D: RenderGraph resource hazard validation passed.");
            }
        }

        s_Data.ActiveGraphPath = path;
        s_Data.ActiveGraphAOTechnique = s_Data.PostProcess.ActiveAOTechnique;
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
        if (s_Data.RGraph && !s_Data.ScenePass)
        {
            OLO_CORE_INFO("Renderer3D::OnWindowResize: ScenePass missing — running deferred SetupRenderGraph");
            SetupRenderGraph(width, height);
            s_Data.ForwardPlus.Initialize(width, height);
            return; // Initialize already configured for width x height
        }
        else if (s_Data.RGraph)
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

    CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic, i32 entityID)
    {
        // Delegate to the variant that accepts previous-frame bone matrices; pass an empty
        // vector so the callee treats prev as "same as current" (zero per-bone motion).
        static const std::vector<glm::mat4> s_EmptyPrev;
        return DrawAnimatedMesh(mesh, modelMatrix, material, boneMatrices, s_EmptyPrev, isStatic, entityID);
    }

    CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, const std::vector<glm::mat4>& prevBoneMatrices, bool isStatic, i32 entityID)
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
        // See DrawMesh() for the non-PBR-deferred → ForwardOverlayPass
        // rerouting rationale. The same reasoning applies here: non-PBR
        // skinned draws would otherwise alias their MRT outputs onto the
        // G-Buffer slots and corrupt every subsequent pixel.
        bool overlayRoute = false;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
            // Same deferred-capability guard as DrawMesh — a forward-only
            // override on the Deferred path must be rerouted to
            // ForwardOverlayPass so it doesn't alias forward outputs onto
            // G-Buffer slots.
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.ForwardOverlayPass &&
                !IsDeferredCapableShader(shaderToUse))
            {
                overlayRoute = true;
            }
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.PBRGBufferSkinnedShader)
                shaderToUse = s_Data.PBRGBufferSkinnedShader;
            else
                shaderToUse = s_Data.PBRSkinnedShader;
        }
        else
        {
            shaderToUse = s_Data.SkinnedLightingShader;
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.ForwardOverlayPass)
                overlayRoute = true;
        }

        if (!shaderToUse)
        {
            OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: Preferred shader not available, falling back to Lighting3D");
            shaderToUse = s_Data.LightingShader;
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.ForwardOverlayPass)
                overlayRoute = true;
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

        // Previous-frame bones: the skinned shader variants (PBR_GBuffer_Skinned
        // in Deferred, PBR_MultiLight_Skinned in Forward/Forward+) bind a
        // parallel PrevBoneMatrices UBO at binding 31 and use it to emit per-
        // bone velocity alongside u_PrevModel. When the caller doesn't provide
        // a prev pose, CommandDispatch::UploadBoneMatrices aliases the current
        // palette into the prev UBO so the shader always reads valid data and
        // the resulting bone-motion term is zero.
        u32 prevBoneBufferOffset = UINT32_MAX;
        const bool wantPrevStream = !prevBoneMatrices.empty() &&
                                    prevBoneMatrices.size() == boneMatrices.size();
        if (wantPrevStream)
        {
            u32 prevOffset = frameBuffer.AllocateBoneMatrices(boneCount);
            if (prevOffset != UINT32_MAX)
            {
                frameBuffer.WriteBoneMatrices(prevOffset, prevBoneMatrices.data(), boneCount);
                prevBoneBufferOffset = prevOffset;
            }
            // Else: fall back to aliasing current (no spare FDB space this frame).
        }

        // Create POD command
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawMeshCommand>()
                                    : CreateDrawCall<DrawMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        const u32 vertexArrayID = vertexArray->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawAnimatedMesh", vertexArrayID, shaderRendererID))
            return nullptr;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = mesh->GetIndexCount();
        cmd->baseIndex = mesh->GetBaseIndex();
        cmd->transform = modelMatrix;
        // Prev-transform applies to all paths — see DrawMesh() comment. Both
        // the forward PBR_MultiLight_Skinned and deferred PBR_GBuffer_Skinned
        // variants consume u_PrevModel + the prev-bone palette (binding 31)
        // to emit per-bone velocity into their respective velocity targets
        // (scene FB RT3 in Forward/Forward+, G-Buffer RT3 in Deferred).
        cmd->prevTransform = GetAndRecordPrevTransform(entityID, cmd->transform);
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        // Animation support - store offset/count into FrameDataBuffer
        cmd->isAnimatedMesh = true;
        cmd->boneBufferOffset = boneBufferOffset;
        cmd->prevBoneBufferOffset = prevBoneBufferOffset;
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
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            // Route the packet to the overlay bucket so it renders after
            // DeferredLightingPass composites the G-Buffer. Return nullptr
            // so the caller's SubmitPacket(packet) is a no-op.
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

    void Renderer3D::BindSceneUBOs()
    {
        OLO_PROFILE_FUNCTION();
        if (s_Data.CameraUBO)
            s_Data.CameraUBO->Bind();
        if (s_Data.LightPropertiesUBO)
            s_Data.LightPropertiesUBO->Bind();

        // Ensure Forward+ UBO is always bound (with Enabled=0 when inactive)
        // so fragment shaders can always read fplus_Params.z
        s_Data.ForwardPlus.UploadDisabledUBO();
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
            const auto& prevBoneMatrices = skeletonComp.m_Skeleton->m_PrevFinalBoneMatrices;
            const i32 pickEntityID = static_cast<i32>(static_cast<u32>(entityID));

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

                            // Populate prev-world-transform from the shared
                            // per-entity cache on the main thread before we hand
                            // the desc to the parallel worker. Without this the
                            // parallel path drops object motion and TAA /
                            // MotionBlur see zero velocity for moving skinned
                            // meshes. The cache is maintained in Deferred paths
                            // via GetAndRecordPrevTransform; when no history
                            // exists yet it returns current, giving zero motion.
                            const glm::mat4 prevWorldTransform =
                                GetAndRecordPrevTransform(pickEntityID, worldTransform);
                            MeshSubmitDesc desc{};
                            desc.Mesh = submeshComponent.m_Mesh;
                            desc.Transform = worldTransform;
                            desc.MaterialData = submeshMaterial;
                            desc.IsStatic = false;
                            desc.EntityID = pickEntityID;
                            desc.IsAnimated = true;
                            desc.BoneMatrices = &boneMatrices;
                            desc.PrevBoneMatrices = &prevBoneMatrices;
                            desc.PrevTransform = prevWorldTransform;
                            desc.HasPrevTransform = true;
                            meshDescriptors.push_back(std::move(desc));
                            foundSubmeshes = true;
                        }
                    }
                }
            }

            // Fallback: if no submesh entities found, use first submesh from MeshSource
            if (!foundSubmeshes && meshComp.m_MeshSource->GetSubmeshes().Num() > 0)
            {
                auto mesh = Ref<Mesh>::Create(meshComp.m_MeshSource, 0);
                const glm::mat4 prevWorldTransform =
                    GetAndRecordPrevTransform(pickEntityID, worldTransform);
                MeshSubmitDesc desc{};
                desc.Mesh = mesh;
                desc.Transform = worldTransform;
                desc.MaterialData = material;
                desc.IsStatic = false;
                desc.EntityID = pickEntityID;
                desc.IsAnimated = true;
                desc.BoneMatrices = &boneMatrices;
                desc.PrevBoneMatrices = &prevBoneMatrices;
                desc.PrevTransform = prevWorldTransform;
                desc.HasPrevTransform = true;
                meshDescriptors.push_back(std::move(desc));
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

        // Get current + previous bone matrices from the skeleton. The prev
        // pose feeds motion-vector computation in animated PBR shaders so
        // TAA / MotionBlur get correct per-bone velocity rather than a
        // stale-identity fallback.
        const auto& boneMatrices = skeletonComp.m_Skeleton->m_FinalBoneMatrices;
        const auto& prevBoneMatrices = skeletonComp.m_Skeleton->m_PrevFinalBoneMatrices;

        // Convert entt entity id to the i32 picking ID used by the editor.
        const i32 entityID = static_cast<i32>(static_cast<u32>(entity));

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

                    auto* packet = DrawAnimatedMesh(
                        submeshComponent.m_Mesh,
                        worldTransform,
                        submeshMaterial,
                        boneMatrices,
                        prevBoneMatrices,
                        false,
                        entityID);

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
                    prevBoneMatrices,
                    false,
                    entityID);

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

        // In Deferred mode, swap to the Skybox_GBuffer variant and submit the
        // packet to ScenePass (which binds the 4-RT G-Buffer). The shader
        // writes the cubemap sample into RT2.rgb with `emissive.a = 1.0`
        // (unlit flag) so `ComputeDeferredLit` short-circuits and passes the
        // colour through unshaded. Falls back to ForwardOverlayPass when the
        // variant failed to load.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = deferredActive && s_Data.SkyboxGBufferShader;
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.ForwardOverlayPass;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.SkyboxGBufferShader : s_Data.SkyboxShader;

        // Create POD command on the appropriate bucket
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawSkyboxCommand>()
                                    : CreateDrawCall<DrawSkyboxCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawSkyboxCommand>();
        cmd->header.type = CommandType::DrawSkybox;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = s_Data.SkyboxMesh->GetHandle();
        cmd->vertexArrayID = s_Data.SkyboxMesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = s_Data.SkyboxMesh->GetIndexCount();
        cmd->transform = glm::mat4(1.0f); // Identity matrix for skybox
        cmd->shaderHandle = activeShader->GetHandle();
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->skyboxTextureID = skyboxTexture->GetRendererID();

        // Skybox-specific POD render state
        {
            PODRenderState skyboxState = CreateDefaultPODRenderState();
            skyboxState.depthTestEnabled = true;
            skyboxState.depthFunction = GL_LEQUAL;
            skyboxState.depthWriteMask = false;
            skyboxState.cullingEnabled = false;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(skyboxState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for skybox (rendered last in skybox layer with max depth)
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        // Skybox always renders at maximum depth (far plane)
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::Skybox, shaderID, 0, 0xFFFFFF);
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            // Submit directly to the overlay bucket; return nullptr so the
            // caller's follow-up SubmitPacket(packet) is a safe no-op.
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

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

        // Modify render state and sort key to ensure skeleton visibility through geometry
        if (packet)
        {
            auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
            if (drawCmd)
            {
                // Depth off so lines always pass depth test
                PODRenderState skelState = FrameDataBufferManager::Get().GetRenderState(drawCmd->renderStateIndex);
                skelState.depthTestEnabled = false;
                // Only write to color attachment (0); skip entity-ID (1) and normals (2)
                skelState.colorAttachmentWriteMask = 0x01;
                drawCmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(skelState);
            }

            // Move to UI layer so these draw AFTER all 3D geometry
            PacketMetadata meta = packet->GetMetadata();
            meta.m_SortKey.SetViewLayer(ViewLayerType::UI);
            packet->SetMetadata(meta);
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

        // Modify render state and sort key to ensure joint visibility through geometry
        if (packet)
        {
            auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
            if (drawCmd)
            {
                // Depth off so joints always pass depth test
                PODRenderState jointState = FrameDataBufferManager::Get().GetRenderState(drawCmd->renderStateIndex);
                jointState.depthTestEnabled = false;
                // Only write to color attachment (0); skip entity-ID (1) and normals (2)
                jointState.colorAttachmentWriteMask = 0x01;
                drawCmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(jointState);
            }

            // Move to UI layer so these draw AFTER all 3D geometry
            PacketMetadata meta = packet->GetMetadata();
            meta.m_SortKey.SetViewLayer(ViewLayerType::UI);
            packet->SetMetadata(meta);
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

        // Deferred path: the G-Buffer variant used to be preferred so the
        // grid participates in deferred lighting via emissive+`gl_FragDepth`.
        // But grid depth in the scene FB breaks water's SSR — the ray-march
        // finds a "hit" at the infinite plane and samples the refraction
        // texture there (which contains the grid itself), making water
        // appear to reflect/refract the grid and look transparent.
        // Route the grid through the forward overlay pass instead; its
        // `depthWriteMask=false` state keeps the grid out of the scene
        // depth buffer so downstream SSR behaves like Forward+.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = false;
        const bool overlayRoute = deferredActive && s_Data.ForwardOverlayPass;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.InfiniteGridGBufferShader : s_Data.InfiniteGridShader;

        // Create POD command packet
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawInfiniteGridCommand>()
                                    : CreateDrawCall<DrawInfiniteGridCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawInfiniteGridCommand>();
        cmd->header.type = CommandType::DrawInfiniteGrid;

        // Store renderer IDs (POD)
        cmd->shaderHandle = activeShader->GetHandle();
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->quadVAOID = s_Data.FullscreenQuadVAO->GetRendererID();
        cmd->gridScale = gridScale;

        // Grid-specific render state. The G-Buffer variant writes gl_FragDepth
        // and premultiplies alpha into emissive.rgb, so blending is disabled
        // (destination G-Buffer is opaque). Forward path still alpha-blends.
        {
            PODRenderState gridState = CreateDefaultPODRenderState();
            if (useGBufferVariant)
            {
                gridState.blendEnabled = false;
                gridState.depthTestEnabled = true;
                gridState.depthWriteMask = true; // gl_FragDepth path
            }
            else
            {
                gridState.blendEnabled = true;
                gridState.blendSrcFactor = GL_SRC_ALPHA;
                gridState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
                gridState.depthTestEnabled = true;
                gridState.depthWriteMask = false;
            }
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(gridState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: G-Buffer variant sits with opaques (depth-write on); forward
        // variant stays in the transparent layer.
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        if (useGBufferVariant)
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, 0x800000);
        else
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, 0x800000);
        packet->SetMetadata(metadata);

        // Submit packet to the appropriate bucket. Despite both `CreateForwardOverlayDrawCall`
        // / `CreateDrawCall` being named "CreateDrawCall", they ONLY allocate via
        // `CommandBucket::CreateDrawCall<T>()` (which does not push into the bucket's
        // packet list). The actual submission happens here — exactly once. We return
        // nullptr afterwards to match the other overlay helpers and prevent callers
        // from accidentally double-submitting the packet.
        if (overlayRoute)
            SubmitForwardOverlayPacket(packet);
        else
            SubmitPacket(packet);
        return nullptr;
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

        // In the deferred path, substitute the forward terrain shader with the
        // G-Buffer variant so terrain participates in the deferred composite
        // (full PBR evaluated once in DeferredLightingPass).
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        Ref<Shader> activeShader = shader;
        if (deferredActive)
        {
            if (shader == s_Data.TerrainPBRShader && s_Data.TerrainGBufferShader)
                activeShader = s_Data.TerrainGBufferShader;
            else if (shader == s_Data.VoxelPBRShader && s_Data.VoxelGBufferShader)
                activeShader = s_Data.VoxelGBufferShader;
        }
        const bool useGBufferVariant = deferredActive && (activeShader != shader);
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.ForwardOverlayPass;

        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawTerrainPatchCommand>()
                                    : CreateDrawCall<DrawTerrainPatchCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawTerrainPatchCommand>();
        cmd->header.type = CommandType::DrawTerrainPatch;

        cmd->vertexArrayID = vaoID;
        cmd->indexCount = indexCount;
        cmd->patchVertexCount = patchVertexCount;
        cmd->shaderRendererID = activeShader->GetRendererID();
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
        {
            PODRenderState terrainState = CreateDefaultPODRenderState();
            terrainState.blendEnabled = false;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(terrainState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: group by shader for state efficiency
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(transform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = true;
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

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

        // Substitute voxel G-Buffer shader in deferred path.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        Ref<Shader> activeShader = shader;
        if (deferredActive && shader == s_Data.VoxelPBRShader && s_Data.VoxelGBufferShader)
            activeShader = s_Data.VoxelGBufferShader;
        const bool useGBufferVariant = deferredActive && (activeShader != shader);
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.ForwardOverlayPass;

        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawVoxelMeshCommand>()
                                    : CreateDrawCall<DrawVoxelMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawVoxelMeshCommand>();
        cmd->header.type = CommandType::DrawVoxelMesh;

        cmd->vertexArrayID = vaoID;
        cmd->indexCount = indexCount;
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->albedoArrayTextureID = albedoArrayID;
        cmd->normalArrayTextureID = normalArrayID;
        cmd->armArrayTextureID = armArrayID;
        cmd->transform = transform;
        cmd->entityID = entityID;

        {
            PODRenderState voxelState = CreateDefaultPODRenderState();
            voxelState.blendEnabled = false;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(voxelState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(transform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = true;
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

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

        // Compute world-space joint positions
        std::vector<glm::vec3> worldPositions(skeleton.m_GlobalTransforms.size());
        for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
        {
            worldPositions[i] = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[i] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        // Compute average bone length to auto-scale visualization.
        // jointSize and boneThickness are treated as fractions of this extent
        // so the skeleton is visible regardless of the model's unit system.
        f32 totalBoneLength = 0.0f;
        i32 boneCount = 0;
        for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
        {
            i32 parentIndex = skeleton.m_ParentIndices[i];
            if (parentIndex >= 0 && parentIndex < static_cast<i32>(skeleton.m_GlobalTransforms.size()))
            {
                f32 len = glm::length(worldPositions[i] - worldPositions[parentIndex]);
                if (len > 0.001f)
                {
                    totalBoneLength += len;
                    ++boneCount;
                }
            }
        }
        f32 avgBoneLength = (boneCount > 0) ? (totalBoneLength / static_cast<f32>(boneCount)) : 1.0f;

        // Scale factors: jointSize=1.0 means joint radius = 10% of average bone length
        f32 scaledJointRadius = jointSize * avgBoneLength * 0.1f;
        // DrawLine internally multiplies thickness by 0.005, so divide to compensate
        f32 scaledBoneThickness = (boneThickness * avgBoneLength * 0.02f) / 0.005f;

        // Colors for visualization
        const glm::vec3 boneColor(1.0f, 0.5f, 0.0f);  // Bright orange for bones
        const glm::vec3 jointColor(0.0f, 1.0f, 0.0f); // Bright green for joints

        // Draw joints
        if (showJoints)
        {
            for (sizet i = 0; i < worldPositions.size(); ++i)
            {
                auto* spherePacket = DrawSphere(worldPositions[i], scaledJointRadius, jointColor);
                if (spherePacket)
                {
                    SubmitPacket(spherePacket);
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
                    f32 boneLength = glm::length(worldPositions[i] - worldPositions[parentIndex]);
                    if (boneLength > 0.001f)
                    {
                        auto* linePacket = DrawLine(worldPositions[parentIndex], worldPositions[i], boneColor, scaledBoneThickness);
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
