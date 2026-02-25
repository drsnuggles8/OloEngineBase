#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Passes/PostProcessRenderPass.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
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
        static CommandPacket* DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic = false, i32 entityID = -1);
        static CommandPacket* DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
        static CommandPacket* DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic = true);
        static CommandPacket* DrawLightCube(const glm::mat4& modelMatrix);
        static CommandPacket* DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
        static CommandPacket* DrawSkybox(const Ref<TextureCubemap>& skyboxTexture);

        // Grid rendering (returns command packet for deferred execution)
        static CommandPacket* DrawInfiniteGrid(f32 gridScale = 1.0f);

        // Skeleton visualization
        static void DrawSkeleton(const Skeleton& skeleton, const glm::mat4& modelMatrix,
                                 bool showBones = true, bool showJoints = true,
                                 f32 jointSize = 0.02f, f32 boneThickness = 2.0f);
        static CommandPacket* DrawLine(const glm::vec3& start, const glm::vec3& end,
                                       const glm::vec3& color = glm::vec3(1.0f), f32 thickness = 1.0f);
        static CommandPacket* DrawSphere(const glm::vec3& position, f32 radius,
                                         const glm::vec3& color = glm::vec3(1.0f));

        // Gizmo visualization for scene cameras
        /**
         * @brief Draw a wireframe frustum visualization for a scene camera
         *
         * Draws the near plane, far plane, and connecting edges of a camera's view frustum.
         * Also draws a small camera icon indicator at the camera position.
         *
         * @param cameraTransform World transform of the camera entity
         * @param fov Vertical field of view in radians (for perspective cameras)
         * @param aspectRatio Camera aspect ratio (width/height)
         * @param nearClip Near clipping plane distance
         * @param farClip Far clipping plane distance (clamped for visualization)
         * @param color Frustum line color
         * @param isPerspective True for perspective, false for orthographic
         * @param orthoSize Orthographic size (half-height, used when !isPerspective)
         */
        static void DrawCameraFrustum(const glm::mat4& cameraTransform,
                                      f32 fov, f32 aspectRatio,
                                      f32 nearClip, f32 farClip,
                                      const glm::vec3& color = glm::vec3(0.9f, 0.9f, 0.3f),
                                      bool isPerspective = true,
                                      f32 orthoSize = 10.0f);

        // Light gizmo visualization
        /**
         * @brief Draw a directional light gizmo (arrow indicating direction)
         * @param position World position of the light
         * @param direction Normalized direction vector the light points
         * @param color Light color for the gizmo
         * @param intensity Light intensity (affects gizmo brightness)
         */
        static void DrawDirectionalLightGizmo(const glm::vec3& position,
                                              const glm::vec3& direction,
                                              const glm::vec3& color = glm::vec3(1.0f, 0.9f, 0.3f),
                                              f32 intensity = 1.0f);

        /**
         * @brief Draw a point light gizmo (sphere showing range)
         * @param position World position of the light
         * @param range Light falloff range
         * @param color Light color for the gizmo
         * @param showRangeSphere Whether to draw the range sphere
         */
        static void DrawPointLightGizmo(const glm::vec3& position,
                                        f32 range,
                                        const glm::vec3& color = glm::vec3(1.0f, 0.8f, 0.2f),
                                        bool showRangeSphere = true);

        /**
         * @brief Draw a spot light gizmo (cone showing direction and angle)
         * @param position World position of the light
         * @param direction Normalized direction vector
         * @param range Light range
         * @param innerCutoff Inner cone angle in degrees
         * @param outerCutoff Outer cone angle in degrees
         * @param color Light color for the gizmo
         */
        static void DrawSpotLightGizmo(const glm::vec3& position,
                                       const glm::vec3& direction,
                                       f32 range,
                                       f32 innerCutoff,
                                       f32 outerCutoff,
                                       const glm::vec3& color = glm::vec3(1.0f, 0.8f, 0.2f));

        /**
         * @brief Draw an audio source range gizmo (spheres showing min/max distance)
         * @param position World position of the audio source
         * @param minDistance Minimum attenuation distance
         * @param maxDistance Maximum attenuation distance
         * @param color Gizmo color
         */
        static void DrawAudioSourceGizmo(const glm::vec3& position,
                                         f32 minDistance,
                                         f32 maxDistance,
                                         const glm::vec3& color = glm::vec3(0.2f, 0.6f, 1.0f));

        /**
         * @brief Draw world axis helper at origin
         * @param axisLength Length of each axis line
         */
        static void DrawWorldAxisHelper(f32 axisLength = 5.0f);

        // ====================================================================
        // 3D Collider Gizmo Visualization
        // ====================================================================

        /**
         * @brief Draw a wireframe box collider gizmo
         * @param position World position (center of the box)
         * @param halfExtents Half-size of the box in each axis
         * @param rotation Rotation quaternion
         * @param color Wireframe color
         */
        static void DrawBoxColliderGizmo(const glm::vec3& position,
                                         const glm::vec3& halfExtents,
                                         const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                                         const glm::vec3& color = glm::vec3(0.0f, 1.0f, 0.0f));

        /**
         * @brief Draw a wireframe sphere collider gizmo
         * @param position World position (center of the sphere)
         * @param radius Sphere radius
         * @param color Wireframe color
         */
        static void DrawSphereColliderGizmo(const glm::vec3& position,
                                            f32 radius,
                                            const glm::vec3& color = glm::vec3(0.0f, 1.0f, 0.0f));

        /**
         * @brief Draw a wireframe capsule collider gizmo
         * @param position World position (center of the capsule)
         * @param radius Capsule radius
         * @param halfHeight Half-height of the cylindrical section
         * @param rotation Rotation quaternion (capsule aligned with local Y axis)
         * @param color Wireframe color
         */
        static void DrawCapsuleColliderGizmo(const glm::vec3& position,
                                             f32 radius,
                                             f32 halfHeight,
                                             const glm::quat& rotation = glm::quat(1, 0, 0, 0),
                                             const glm::vec3& color = glm::vec3(0.0f, 1.0f, 0.0f));

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
         * @brief Get worker context for an explicit worker index
         *
         * Uses the provided worker index directly without thread ID lookup.
         * This is the optimized path when contextIndex is already known
         * from ParallelForWithTaskContext.
         *
         * @param workerIndex The worker index (typically from ParallelFor contextIndex)
         * @return Worker submission context with allocator and bucket access
         */
        static WorkerSubmitContext GetWorkerContext(u32 workerIndex);

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
            i32 EntityID = -1; // Entity ID for picking (-1 = no entity)
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

        // Upload multi-light UBO data for the current frame
        static void UploadMultiLightUBO(const UBOStructures::MultiLightUBO& data);

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

        static const Ref<ParticleRenderPass>& GetParticlePass()
        {
            return s_Data.ParticlePass;
        }

        static const Ref<ShadowRenderPass>& GetShadowPass()
        {
            return s_Data.ShadowPass;
        }

        static const Ref<PostProcessRenderPass>& GetPostProcessPass()
        {
            return s_Data.PostProcessPass;
        }

        static ShadowMap& GetShadowMap()
        {
            return s_Data.Shadow;
        }

        static Ref<Shader> GetTerrainPBRShader() { return s_Data.TerrainPBRShader; }
        static Ref<Shader> GetTerrainDepthShader() { return s_Data.TerrainDepthShader; }
        static Ref<Shader> GetVoxelPBRShader() { return s_Data.VoxelPBRShader; }
        static Ref<Shader> GetVoxelDepthShader() { return s_Data.VoxelDepthShader; }
        static Ref<Shader> GetFoliageShader() { return s_Data.FoliageShader; }
        static Ref<Shader> GetFoliageDepthShader() { return s_Data.FoliageDepthShader; }
        static Ref<UniformBuffer> GetTerrainUBO() { return s_Data.TerrainUBO; }
        static Ref<UniformBuffer> GetFoliageUBO() { return s_Data.FoliageUBO; }
        static Ref<UniformBuffer> GetModelMatrixUBO() { return s_Data.ModelMatrixUBO; }

        static PostProcessSettings& GetPostProcessSettings()
        {
            return s_Data.PostProcess;
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
        static void BeginSceneCommon();
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
            Ref<VertexArray> FullscreenQuadVAO; // Fullscreen quad for grid and post-processing
            Ref<UniformBuffer> CameraUBO;
            Ref<UniformBuffer> MaterialUBO;
            Ref<UniformBuffer> LightPropertiesUBO;
            Ref<UniformBuffer> MultiLightBuffer;
            Ref<UniformBuffer> BoneMatricesUBO;
            Ref<UniformBuffer> ModelMatrixUBO;
            Ref<UniformBuffer> PostProcessUBO;
            Ref<UniformBuffer> MotionBlurUBO;
            Ref<UniformBuffer> SSAOUBO;
            Ref<UniformBuffer> TerrainUBO;
            Ref<UniformBuffer> FoliageUBO;

            glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
            glm::mat4 ViewMatrix = glm::mat4(1.0f);
            glm::mat4 ProjectionMatrix = glm::mat4(1.0f);

            Frustum ViewFrustum;
            bool FrustumCullingEnabled = true;
            bool DynamicCullingEnabled = true;

            Light SceneLight;
            glm::vec3 ViewPos;
            f32 CameraNearClip = 0.1f;
            f32 CameraFarClip = 1000.0f;

            Statistics Stats;
            u32 CommandCounter = 0;

            // Global resource registry for scene-wide resources like environment maps, shadows, etc.
            ShaderResourceRegistry GlobalResourceRegistry;

            // Shader registry management
            std::unordered_map<u32, ShaderResourceRegistry*> ShaderRegistries;

            Ref<RenderGraph> RGraph;
            Ref<ShadowRenderPass> ShadowPass;
            Ref<SceneRenderPass> ScenePass;
            Ref<SSAORenderPass> SSAOPass;
            Ref<ParticleRenderPass> ParticlePass;
            Ref<PostProcessRenderPass> PostProcessPass;
            Ref<FinalRenderPass> FinalPass;

            // Shadow mapping
            ShadowMap Shadow;
            Ref<Shader> ShadowDepthShader;
            Ref<Shader> ShadowDepthSkinnedShader;

            // Terrain
            Ref<Shader> TerrainPBRShader;
            Ref<Shader> TerrainDepthShader;
            Ref<Shader> VoxelPBRShader;
            Ref<Shader> VoxelDepthShader;
            Ref<Shader> FoliageShader;
            Ref<Shader> FoliageDepthShader;

            // Post-processing
            PostProcessSettings PostProcess;
            PostProcessUBOData PostProcessGPUData;
            MotionBlurUBOData MotionBlurGPUData;
            SSAOUBOData SSAOGPUData;
            glm::mat4 PrevViewProjectionMatrix = glm::mat4(1.0f);

            // Parallel submission state
            ParallelSceneContext ParallelContext;
            bool ParallelSubmissionActive = false;
        };

        static Renderer3DData s_Data;
        static ShaderLibrary m_ShaderLibrary;
    };
} // namespace OloEngine
