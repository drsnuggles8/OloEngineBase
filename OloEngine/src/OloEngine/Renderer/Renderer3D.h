#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"

// Forward declarations
namespace OloEngine
{
    class Texture2D;
    class RenderCommand;
    class UniformBuffer;
    class CommandBucket;
    class Scene;
    class Entity;
    class CommandAllocator;
    class EditorCamera;
} // namespace OloEngine

namespace OloEngine
{
    // ========================================================================
    // Parallel Rendering Context
    // ========================================================================

    /**
     * @brief Thread-safe scene context for parallel command generation
     *
     * This struct contains all the immutable data needed by worker threads
     * to generate draw commands. It's created by BeginScene() and remains
     * valid until EndScene().
     */
    struct ParallelSceneContext
    {
        glm::mat4 ViewMatrix = glm::mat4(1.0f);
        glm::mat4 ProjectionMatrix = glm::mat4(1.0f);
        glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
        glm::vec3 ViewPosition = glm::vec3(0.0f);
        Frustum ViewFrustum;
        bool FrustumCullingEnabled = true;
        bool DynamicCullingEnabled = true;

        // Shader references (immutable during frame)
        Ref<Shader> LightingShader;
        Ref<Shader> SkinnedLightingShader;
        Ref<Shader> PBRShader;
        Ref<Shader> PBRSkinnedShader;
        Ref<Shader> LightCubeShader;
        Ref<Shader> SkyboxShader;
        Ref<Shader> QuadShader;
    };

    /**
     * @brief Per-worker submission context
     *
     * Contains worker-specific resources for parallel command generation.
     */
    struct WorkerSubmitContext
    {
        u32 WorkerIndex = 0;
        CommandAllocator* Allocator = nullptr;
        CommandBucket* Bucket = nullptr;
        const ParallelSceneContext* SceneContext = nullptr;

        // Statistics
        u32 CommandsSubmitted = 0;
        u32 MeshesCulled = 0;
    };

    // @brief High-level 3D rendering API with scene and material management
    //
    // Thread Safety:
    // - BeginScene() / EndScene() must be called from the main thread only
    // - BeginParallelSubmission() / EndParallelSubmission() bracket parallel regions
    // - DrawMeshParallel() and related methods can be called from worker threads
    // - All other Draw* methods are NOT thread-safe unless noted
    class ShaderLibrary;

    class Renderer3D
    {
      public:
        struct Statistics
        {
            u32 TotalMeshes = 0;
            u32 CulledMeshes = 0;
            u32 DrawCalls = 0;
            u32 ShaderBinds = 0;
            u32 TextureBinds = 0;
            u32 TotalAnimatedMeshes = 0;
            u32 RenderedAnimatedMeshes = 0;
            u32 SkippedAnimatedMeshes = 0;

            void Reset()
            {
                TotalMeshes = 0;
                CulledMeshes = 0;
                DrawCalls = 0;
                ShaderBinds = 0;
                TextureBinds = 0;
                TotalAnimatedMeshes = 0;
                RenderedAnimatedMeshes = 0;
                SkippedAnimatedMeshes = 0;
            }
        };

      public:
        static void Init();
        static void Shutdown();
        static bool IsInitialized();

        static void BeginScene(const PerspectiveCamera& camera);
        static void BeginScene(const EditorCamera& camera);
        static void BeginScene(const Camera& camera, const glm::mat4& transform);
        static void EndScene();
        static CommandPacket* DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic = true, i32 entityID = -1);
        // Animated drawing commands
        static CommandPacket* DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic = false);
        static CommandPacket* DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
        static CommandPacket* DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic = true);
        static CommandPacket* DrawLightCube(const glm::mat4& modelMatrix);
        static CommandPacket* DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
        static CommandPacket* DrawSkybox(const Ref<TextureCubemap>& skyboxTexture);
        
        // Grid rendering
        static void DrawInfiniteGrid(f32 gridScale = 1.0f);

        // Skeleton visualization
        static void DrawSkeleton(const Skeleton& skeleton, const glm::mat4& modelMatrix,
                                 bool showBones = true, bool showJoints = true,
                                 f32 jointSize = 0.02f, f32 boneThickness = 2.0f);
        static CommandPacket* DrawLine(const glm::vec3& start, const glm::vec3& end,
                                       const glm::vec3& color = glm::vec3(1.0f), f32 thickness = 1.0f);
        static CommandPacket* DrawSphere(const glm::vec3& position, f32 radius,
                                         const glm::vec3& color = glm::vec3(1.0f));

        // ECS Animated Mesh Rendering
        static void RenderAnimatedMeshes(const Ref<Scene>& scene, const Material& defaultMaterial);
        static void RenderAnimatedMesh(const Ref<Scene>& scene, Entity entity, const Material& defaultMaterial);

        // ====================================================================
        // Parallel Command Generation API
        // ====================================================================

        /**
         * @brief Begin parallel command submission mode
         *
         * Prepares internal state for parallel command generation.
         * Must be called after BeginScene() and before any parallel submission.
         */
        static void BeginParallelSubmission();

        /**
         * @brief End parallel command submission mode
         *
         * Merges all worker commands and prepares for sorting/dispatch.
         * Must be called before EndScene() and after all workers complete.
         */
        static void EndParallelSubmission();

        /**
         * @brief Get worker context for the current thread
         *
         * Registers the calling thread as a worker and returns its context.
         * Should be called once per thread at the start of parallel work.
         *
         * @return Worker submission context with allocator and bucket access
         */
        static WorkerSubmitContext GetWorkerContext();

        /**
         * @brief Get the current parallel scene context
         *
         * Returns the immutable scene data for the current frame.
         * Valid only between BeginScene() and EndScene().
         *
         * @return Pointer to the parallel scene context (nullptr if not in scene)
         */
        static const ParallelSceneContext* GetParallelSceneContext();

        /**
         * @brief Thread-safe mesh drawing for parallel submission
         *
         * Can be called from worker threads during parallel submission.
         * Uses worker-local allocator and bucket for lock-free operation.
         *
         * @param ctx Worker context from GetWorkerContext()
         * @param mesh The mesh to draw
         * @param modelMatrix Transform matrix
         * @param material Material properties
         * @param isStatic Whether the object is static (for culling optimization)
         * @return Command packet pointer (caller should submit via ctx)
         */
        static CommandPacket* DrawMeshParallel(WorkerSubmitContext& ctx,
                                               const Ref<Mesh>& mesh,
                                               const glm::mat4& modelMatrix,
                                               const Material& material,
                                               bool isStatic = true,
                                               i32 entityID = -1);

        /**
         * @brief Thread-safe animated mesh drawing for parallel submission
         */
        static CommandPacket* DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                       const Ref<Mesh>& mesh,
                                                       const glm::mat4& modelMatrix,
                                                       const Material& material,
                                                       const std::vector<glm::mat4>& boneMatrices,
                                                       bool isStatic = false);

        /**
         * @brief Submit a packet to the worker's bucket (thread-safe)
         */
        static void SubmitPacketParallel(WorkerSubmitContext& ctx, CommandPacket* packet);

        /**
         * @brief Check if currently in parallel submission mode
         */
        static bool IsParallelSubmissionActive();

        /**
         * @brief Descriptor for a single mesh to be submitted in parallel
         */
        struct MeshSubmitDesc
        {
            Ref<Mesh> Mesh;
            glm::mat4 Transform = glm::mat4(1.0f);
            Material MaterialData;
            bool IsStatic = true;
            i32 EntityID = -1;  // Entity ID for picking (-1 = no entity)
            // For animated meshes
            bool IsAnimated = false;
            const std::vector<glm::mat4>* BoneMatrices = nullptr;
        };

        /**
         * @brief Submit multiple meshes in parallel using the task system
         *
         * This function uses ParallelFor internally to distribute mesh submission
         * across available worker threads. It handles BeginParallelSubmission/
         * EndParallelSubmission automatically.
         *
         * @param meshes Array of mesh descriptors to submit
         * @param minBatchSize Minimum number of meshes per batch (default: 16)
         * @return Total number of commands submitted
         */
        static u32 SubmitMeshesParallel(const std::vector<MeshSubmitDesc>& meshes,
                                        i32 minBatchSize = 16);

        static void SetLight(const Light& light);
        static void SetViewPosition(const glm::vec3& position);

        // Scene light collection (collects light components from scene)
        static void SetSceneLights(const Ref<Scene>& scene);

        // Culling methods
        static void EnableFrustumCulling(bool enable);
        static bool IsFrustumCullingEnabled();
        static void EnableDynamicCulling(bool enable);
        static bool IsDynamicCullingEnabled();
        static const Frustum& GetViewFrustum();
        static bool IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform);
        static bool IsVisibleInFrustum(const BoundingSphere& sphere);
        static bool IsVisibleInFrustum(const BoundingBox& box);

        // Debug culling methods
        static void SetForceDisableCulling(bool disable);
        static bool IsForceDisableCulling();

        // Statistics and debug methods
        static Statistics GetStats();
        static void ResetStats();

        // Global resource management for scene-wide resources
        static ShaderResourceRegistry& GetGlobalResourceRegistry()
        {
            return s_Data.GlobalResourceRegistry;
        }
        template<typename T>
        static bool SetGlobalResource(const std::string& name, const Ref<T>& resource)
        {
            return s_Data.GlobalResourceRegistry.SetResource(name, resource);
        }
        static void ApplyGlobalResources();

        // Shader registry management
        static ShaderResourceRegistry* GetShaderRegistry(u32 shaderID);
        static void RegisterShaderRegistry(u32 shaderID, ShaderResourceRegistry* registry);
        static void UnregisterShaderRegistry(u32 shaderID);
        static const std::unordered_map<u32, ShaderResourceRegistry*>& GetShaderRegistries();

        // High-level resource setting methods
        template<typename T>
        static bool SetShaderResource(u32 shaderID, const std::string& name, const Ref<T>& resource)
        {
            auto* registry = GetShaderRegistry(shaderID);
            if (registry)
            {
                return registry->SetResource(name, resource);
            }
            return false;
        }
        static void ApplyResourceBindings(u32 shaderID);

        // Debug access to command bucket for debugging tools
        static const CommandBucket* GetCommandBucket()
        {
            return s_Data.ScenePass ? &s_Data.ScenePass->GetCommandBucket() : nullptr;
        }

        // Entity ID picking support
        /**
         * @brief Read entity ID from the scene framebuffer at the given pixel coordinates
         * @param x Pixel x coordinate
         * @param y Pixel y coordinate
         * @return Entity ID at the given position (0 if no entity)
         */
        static int ReadEntityIDFromFramebuffer(int x, int y)
        {
            if (!s_Data.ScenePass)
            {
                return 0;
            }
            auto framebuffer = s_Data.ScenePass->GetTarget();
            if (!framebuffer)
            {
                return 0;
            }
            // Entity ID is stored in attachment index 1 (RED_INTEGER format)
            return framebuffer->ReadPixel(1, x, y);
        }

        // Window resize handling
        static void OnWindowResize(u32 width, u32 height);
        static const Ref<RenderGraph>& GetRenderGraph()
        {
            return s_Data.RGraph;
        }

        static const Ref<SceneRenderPass>& GetScenePass()
        {
            return s_Data.ScenePass;
        }

        // Shader library access for PBR material shader selection
        static ShaderLibrary& GetShaderLibrary();

        template<typename T>
        static CommandPacket* CreateDrawCall()
        {
            OLO_PROFILE_FUNCTION();
            return s_Data.ScenePass->GetCommandBucket().CreateDrawCall<T>();
        }

        static void SubmitPacket(CommandPacket* packet)
        {
            OLO_PROFILE_FUNCTION();
            if (!packet)
            {
                OLO_CORE_WARN("Renderer3D::SubmitPacket: Attempted to submit a null CommandPacket pointer!");
                return;
            }
            s_Data.ScenePass->SubmitPacket(packet);
        }

      private:
        static void UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection);
        static void UpdateLightPropertiesUBO();
        static void SetupRenderGraph(u32 width, u32 height);

      private:
        struct Renderer3DData
        {
            Ref<Mesh> CubeMesh;
            Ref<Mesh> QuadMesh;
            Ref<Mesh> SkyboxMesh;
            Ref<Mesh> LineQuadMesh; // Cached unit-length quad for debug lines
            Ref<Shader> LightCubeShader;
            Ref<Shader> LightingShader;
            Ref<Shader> SkinnedLightingShader;
            Ref<Shader> QuadShader;
            Ref<Shader> PBRShader;
            Ref<Shader> PBRSkinnedShader;
            Ref<Shader> PBRMultiLightShader;
            Ref<Shader> PBRMultiLightSkinnedShader;
            Ref<Shader> SkyboxShader;
            Ref<Shader> InfiniteGridShader;
            Ref<VertexArray> FullscreenQuadVAO;  // Fullscreen quad for grid and post-processing
            Ref<UniformBuffer> CameraUBO;
            Ref<UniformBuffer> MaterialUBO;
            Ref<UniformBuffer> LightPropertiesUBO;
            Ref<UniformBuffer> MultiLightBuffer;
            Ref<UniformBuffer> BoneMatricesUBO;
            Ref<UniformBuffer> ModelMatrixUBO;

            glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
            glm::mat4 ViewMatrix = glm::mat4(1.0f);
            glm::mat4 ProjectionMatrix = glm::mat4(1.0f);

            Frustum ViewFrustum;
            bool FrustumCullingEnabled = true;
            bool DynamicCullingEnabled = true;

            Light SceneLight;
            glm::vec3 ViewPos;

            Statistics Stats;
            u32 CommandCounter = 0;

            // Global resource registry for scene-wide resources like environment maps, shadows, etc.
            ShaderResourceRegistry GlobalResourceRegistry;

            // Shader registry management
            std::unordered_map<u32, ShaderResourceRegistry*> ShaderRegistries;

            Ref<RenderGraph> RGraph;
            Ref<SceneRenderPass> ScenePass;
            Ref<FinalRenderPass> FinalPass;

            // Parallel submission state
            ParallelSceneContext ParallelContext;
            bool ParallelSubmissionActive = false;
        };

        static Renderer3DData s_Data;
        static ShaderLibrary m_ShaderLibrary;
    };
} // namespace OloEngine
