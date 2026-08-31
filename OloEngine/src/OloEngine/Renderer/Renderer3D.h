#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/FluidRenderData.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"
#include "OloEngine/Renderer/HZBGenerator.h"
#include "OloEngine/Wind/WindSystem.h"
#include "OloEngine/Snow/SnowAccumulationSystem.h"
#include "OloEngine/Snow/SnowEjectaSystem.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
#include "OloEngine/Renderer/LightCulling/TiledForwardPlus.h"
#include "OloEngine/Renderer/ReflectionProbeArray.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/TerrainVTBindings.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/Texture3D.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <span>
#include <vector>

// Forward declarations
namespace OloEngine
{
    class Texture2D;
    class TextureCubemap;
    class Shader;
    class VertexArray;
    class Framebuffer;
    class GPUDrivenOcclusionPass;
    class DeferredGPUOcclusionPass;
    class DDGIProbeUpdatePass;
    struct DDGIVolumeDesc;
    struct DDGIMeshCaster;
    class RenderCommand;
    class UniformBuffer;
    class CommandBucket;
    class Scene;
    class Entity;
    class CommandAllocator;
    class EditorCamera;
    class AssetReloadedEvent;
    class CommandPacket;
    class FoliageRenderer;
    class Window;
    struct FramebufferSpecification;
    struct PODMaterialData;
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
        // Culling camera position (issue #726). Equal to ViewPosition unless
        // the culling camera is frozen. LOD selection reads THIS, not
        // ViewPosition — a frozen cut must keep the LOD levels it was cut with,
        // or flying closer silently swaps in a different mesh and the "frozen"
        // picture stops being the one the culler saw.
        glm::vec3 CullViewPosition = glm::vec3(0.0f);
        // Pixel-error LOD inputs for the CULLING camera (issue #711) — position,
        // FOV, render height and threshold, no view matrix. Mirrors
        // Renderer3DData::LODView; see the comment there.
        LODViewParams LODView;
        // Frustum of the CULLING camera (frozen when the observer is active).
        Frustum ViewFrustum;
        bool FrustumCullingEnabled = true;
        bool DynamicCullingEnabled = true;

        // Shader references (immutable during frame)
        Ref<Shader> DefaultForwardShader;
        Ref<Shader> DefaultForwardSkinnedShader;
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
        u32 LODSwitches = 0;
        std::vector<u32> ObjectsPerLODLevel;
    };

    // @brief High-level 3D rendering API with scene and material management
    //
    // Thread Safety:
    // - BeginScene() / EndScene() must be called from the main thread only
    // - BeginParallelSubmission() / EndParallelSubmission() bracket parallel regions
    // - DrawMeshParallel() and related methods can be called from worker threads
    // - All other Draw* methods are NOT thread-safe unless noted
    class ShaderLibrary;

    namespace Tests
    {
        // Test seam: grants RenderGraphFingerprintTest access to the private
        // RenderPipeline / Renderer3DData so it can verify that every
        // topology-affecting setting changes ComputeBlackboardFingerprint()
        // (the invariant the SSR-enable bug violated). See
        // RenderGraphFingerprintTest.cpp.
        struct RenderPipelineFingerprintAccess;
    } // namespace Tests

    class Renderer3D
    {
        friend struct ::OloEngine::Tests::RenderPipelineFingerprintAccess;

      public:
        using RenderCallback = std::function<void()>;

        enum class RenderStreamType : u8
        {
            Geometry = 0,
            ForwardOverlay,
            Foliage,
            Water,
            Decal,
            // GPU-driven two-phase occlusion cull for dense instanced statics
            // (#431). Routed to GPUDrivenOcclusionPass, which draws after
            // ScenePass in Forward / Forward+.
            GPUOcclusion,
        };

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
            u32 LODSwitches = 0;
            u32 TotalEmitters = 0;
            u32 CulledEmitters = 0;
            std::vector<u32> ObjectsPerLODLevel;

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
                LODSwitches = 0;
                TotalEmitters = 0;
                CulledEmitters = 0;
                ObjectsPerLODLevel.clear();
            }
        };

      public:
        // @param loadingWindow  Optional window used to draw the shader loading
        //                       progress bar. Pass nullptr for headless init
        //                       (tests, offline tools); async shader links are
        //                       flushed synchronously without UI.
        static void Init(Window* loadingWindow = nullptr);
        static void Shutdown();
        // True once the render graph is fully built (Scene pass exists) and the
        // renderer is ready to draw. May be false immediately after Init() when
        // Init ran against a 0x0 framebuffer — see HasInitialized() for the
        // "Init() has run at all" query that init/shutdown guards want.
        static bool IsInitialized();
        // True once Init() has run (and allocated its one-shot singletons),
        // regardless of whether the deferred render-graph build has completed.
        // Use this — not IsInitialized() — to guard against double-Init and to
        // decide whether Shutdown() must run; IsInitialized() is for "ready to
        // render" gates only.
        static bool HasInitialized();

        // Diagnostics: which GPU-resource-holding members of s_Data are still alive?
        //
        // Shutdown() must release every one of them. A Ref<> it forgets survives into
        // STATIC destruction at process exit, where its destructor frees GPU buffers
        // through FrameResourceManager / RendererMemoryTracker / GPUResourceInspector —
        // Meyer's singletons that are already gone — and segfaults the process on the way
        // out, AFTER the test binary has printed a clean pass. Ref<GPUFrustumCuller> hid
        // there for exactly that reason. Pinned by RendererShutdownTest.
        //
        // Returns the names of the members still holding resources; empty after a correct
        // Shutdown(). Add any new GPU-owning s_Data member here AND to Shutdown().
        [[nodiscard]] static std::vector<std::string> DebugLiveGpuOwningStatics();

        // Asset hot-reload handler — ensures next frame picks up new RendererIDs
        static void OnAssetReloaded(const AssetReloadedEvent& e);

        static void BeginScene(const PerspectiveCamera& camera);
        static void BeginScene(const EditorCamera& camera);
        static void BeginScene(const Camera& camera, const glm::mat4& transform);
        static void EndScene();
        static CommandPacket* DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic = true, i32 entityID = -1, const LODGroup* lodGroup = nullptr);
        // Virtualized geometry (issue #629): queues a cluster-LOD-DAG instance
        // for the GPU-driven VirtualGeometryPass. Builds + registers the DAG for
        // the mesh source on first sight (synchronous for now; the cook slice
        // moves this offline). Rendered by the Deferred path only.
        // overrideMaterial: an explicit MaterialComponent, or nullptr to shade each submesh
        // with the material it was imported with. defaultMaterial is the last resort for a
        // submesh that carries none.
        // Returns true when the mesh was actually queued. It is dropped — silently, from the
        // caller's point of view — when the cluster DAG will not build, and this function is
        // the only place that knows that. Scene's submission loop needs the answer to report
        // a scene that draws no virtual geometry at all (issue #864).
        [[nodiscard]] static bool SubmitVirtualMesh(AssetHandle meshHandle, const Ref<MeshSource>& meshSource,
                                                    const glm::mat4& modelMatrix, const Material* overrideMaterial,
                                                    const Material& defaultMaterial, i32 entityID,
                                                    f32 errorThresholdPixels, bool castShadows);
        // Flatten a Material into the exact POD record the frame material table
        // uploads for a draw (factors, alpha mode/cutoff, and the resolved GL
        // texture id per slot, incl. the global-IBL fallback). Public because it
        // is the single source of truth for "what the GPU was actually given" —
        // olo_material_get (MCP, issue #607) reports it verbatim rather than
        // re-deriving the resolution and risking a confidently wrong answer.
        static auto CreatePODMaterialDataForMaterial(const Material& material, RHI::ResourceHandle shaderRendererID) -> PODMaterialData;
        // Animated drawing commands
        static CommandPacket* DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic = false, i32 entityID = -1);
        // Same as DrawAnimatedMesh but also carries the previous-frame bone matrices used by the
        // Deferred G-Buffer path to compute per-bone motion vectors. Pass empty prevBoneMatrices
        // (or the same data as boneMatrices) to indicate zero per-bone motion.
        static CommandPacket* DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, const std::vector<glm::mat4>& prevBoneMatrices, bool isStatic = false, i32 entityID = -1);
        static CommandPacket* DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
        // `ownerKey` lets callers produce stable per-instance motion vectors
        // when multiple submission sources (entities / emitters / foliage
        // chunks) render the same mesh with independent instance arrays --
        // see GetAndRecordPrevInstanceTransforms. Leaving it at 0 preserves
        // the legacy mesh-handle-only cache key.
        static CommandPacket* DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic = true, u64 ownerKey = 0);

        // InstanceData overload — propagates per-instance Color, Custom, and
        // EntityID through FrameDataBuffer's parallel streams so shaders that
        // read `instances[gl_InstanceIndex].Color` etc. (see
        // InstanceBlock_Vertex.glsl) get the values the caller supplied.
        // Used by Scene's InstancedMeshComponent loop to plumb authored
        // per-instance tints + free-float data end-to-end. PrevTransform is
        // aliased from Transform per-instance (per-instance motion vectors
        // for explicit InstancedMeshComponent placements are a future
        // extension).
        static CommandPacket* DrawMeshInstanced(const Ref<Mesh>& mesh, std::span<const InstanceData> instances, const Material& material, bool isStatic = true, u64 ownerKey = 0);
        static CommandPacket* DrawLightCube(const glm::mat4& modelMatrix);
        static CommandPacket* DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
        static CommandPacket* DrawSkybox(const Ref<TextureCubemap>& skyboxTexture);

        // Grid rendering (returns command packet for deferred execution)
        static CommandPacket* DrawInfiniteGrid(f32 gridScale = 1.0f);

        // Terrain/Voxel rendering (returns command packets for sorted execution)
        static CommandPacket* DrawTerrainPatch(
            RHI::ResourceHandle vaoID, u32 indexCount, u32 patchVertexCount,
            const Ref<Shader>& shader,
            RHI::ResourceHandle heightmapID, RHI::ResourceHandle splatmapID, RHI::ResourceHandle splatmap1ID,
            RHI::ResourceHandle albedoArrayID, RHI::ResourceHandle normalArrayID, RHI::ResourceHandle armArrayID,
            const glm::mat4& transform,
            const ShaderBindingLayout::TerrainUBO& terrainUBO,
            i32 entityID = -1,
            // GPU-driven LOD (issue #714). Supply both to draw the shared unit
            // patch grid instanced from the GPU-built visible-node list; leave
            // both null for a chunk-geometry draw.
            RHI::ResourceHandle terrainIndirectArgsID = {},
            RHI::ResourceHandle terrainVisibleNodesID = {},
            // Terrain virtual texturing (issue #715). Default-constructed means
            // "VT off for this draw"; a partially-filled set is rejected.
            const TerrainVTBindings& virtualTexture = {});

        static CommandPacket* DrawVoxelMesh(
            RHI::ResourceHandle vaoID, u32 indexCount,
            const Ref<Shader>& shader,
            RHI::ResourceHandle albedoArrayID, RHI::ResourceHandle normalArrayID, RHI::ResourceHandle armArrayID,
            const glm::mat4& transform,
            i32 entityID = -1,
            // Binary greedy meshing (issue #727). Non-zero draws `indexCount`
            // indices of the shared unit quad once per merged packed quad;
            // zero keeps the marching-cubes indexed triangle-soup draw.
            u32 instanceCount = 0);

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
         * @brief Draw a sphere area light gizmo (emissive sphere at radius + wireframe range sphere)
         * @param position World position of the light
         * @param radius Emitter sphere radius
         * @param range Falloff range
         * @param color Light color
         * @param intensity Light intensity (drives emissive brightness on the icon)
         */
        static void DrawSphereAreaLightGizmo(const glm::vec3& position,
                                             f32 radius,
                                             f32 range,
                                             const glm::vec3& color = glm::vec3(1.0f, 0.8f, 0.2f),
                                             f32 intensity = 1.0f);

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
                                               i32 entityID = -1,
                                               const LODGroup* lodGroup = nullptr,
                                               // Previous-frame transform for this mesh. When null the
                                               // worker aliases current->prev (zero object motion) --
                                               // parallel workers cannot touch the main-thread entity
                                               // motion-history map, so the caller is responsible for
                                               // supplying prev data when per-object velocity matters.
                                               const glm::mat4* prevModelMatrix = nullptr);

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
         * @brief Thread-safe animated mesh drawing with previous-frame pose
         *
         * Same as DrawAnimatedMeshParallel above but also carries the prev-
         * frame pose so the worker can upload the prev-bone palette alongside
         * the current one and populate `prevTransform` for correct TAA /
         * motion-blur velocity. `prevBoneMatrices` must either be empty or
         * have the same size as `boneMatrices`; `hasPrevTransform` lets the
         * caller distinguish "identity" from "explicitly unchanged".
         */
        static CommandPacket* DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                       const Ref<Mesh>& mesh,
                                                       const glm::mat4& modelMatrix,
                                                       const Material& material,
                                                       const std::vector<glm::mat4>& boneMatrices,
                                                       const std::vector<glm::mat4>& prevBoneMatrices,
                                                       const glm::mat4& prevModelMatrix,
                                                       bool hasPrevTransform,
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
            // Optional previous-frame pose for motion-vector generation. When
            // null (or empty / size-mismatched) the consumer aliases current
            // bones / transform into the prev slot -- zero per-bone and per-
            // object motion. Lifetime: caller must keep the referenced vector
            // alive until SubmitMeshesParallel returns.
            const std::vector<glm::mat4>* PrevBoneMatrices = nullptr;
            glm::mat4 PrevTransform = glm::mat4(1.0f);
            bool HasPrevTransform = false; // False => prev == current (zero object motion).
            // For LOD selection
            const LODGroup* LODGroupPtr = nullptr;
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

        static void SetViewPosition(const glm::vec3& position);
        // Direction of the scene's primary directional light, consumed by the
        // fog / atmospheric sun-direction derivation. Set once per frame from
        // Scene::ProcessScene3DSharedLogic; replaces the retired single-light
        // SceneLight as the sun-direction source.
        static void SetPrimaryDirectionalLightDirection(const glm::vec3& direction);
        // The direction set above (travel direction of the sun's light). Consumed
        // by the underwater caustics term to fade caustics as the sun drops toward
        // the horizon (§7.1). Defaults to straight down before any light is seen.
        [[nodiscard]] static const glm::vec3& GetPrimaryDirectionalLightDirection()
        {
            return s_Data.PrimaryDirectionalLightDir;
        }
        static void SetCameraClipPlanes(f32 nearClip, f32 farClip);

        // Upload multi-light UBO data for the current frame (partial: only header + activeLightCount lights)
        static void UploadMultiLightUBO(const UBOStructures::MultiLightUBO& data, i32 activeLightCount);

        // Upload light probe volume parameters and SH coefficient data
        static void UploadLightProbeData(const ShaderBindingLayout::LightProbeVolumeUBO& uboData,
                                         const void* shData, u32 shDataSize);

        // Upload the scene lightmap parameters (UBO_LIGHTMAP) and publish the
        // atlas at TEX_LIGHTMAP (issue #439). The atlas is a Texture2DArray with
        // one layer per page since issue #868. A null atlas publishes the 1x1x1
        // white placeholder array — the sampler stays valid but uboData.Enabled
        // must be 0 then, which is the caller's staleness/absence kill switch.
        static void UploadLightmapData(const ShaderBindingLayout::LightmapUBO& uboData,
                                       const Ref<Texture2DArray>& atlas);

        // Set global IBL textures from the scene's EnvironmentMap.
        // These are used as fallbacks when individual materials don't have IBL configured.
        // Takes BOTH currencies for the three graph-imported maps, from the one
        // call site that has the Ref<Texture> in hand — deriving one from the
        // other later is impossible in Renderer/ (native -> handle is not
        // recoverable, and handle -> native may only happen in Platform/).
        // One currency. The native trio this used to carry alongside the
        // identities existed only so RenderPipeline could ImportTexture() the
        // maps into the frame graph; ImportTextureHandle does that now
        // (issue #691).
        static void SetGlobalIBL(RHI::ResourceHandle irradianceMap, RHI::ResourceHandle prefilterMap,
                                 RHI::ResourceHandle brdfLutMap, RHI::ResourceHandle environmentMap,
                                 f32 iblIntensity = 1.0f);
        static void ClearGlobalIBL();
        // Replace only the diffuse irradiance source (+ intensity), keeping
        // the sky's prefilter / BRDF LUT / environment intact. The dominant
        // reflection probe's diffuse override (issue #705): its specular half
        // moved to the per-pixel probe arrays, and overriding the prefilter
        // map too would turn the probe-miss sky fallback into the dominant
        // probe's own radiance.
        static void OverrideGlobalIrradiance(RHI::ResourceHandle irradianceMap, f32 iblIntensity);
        // Identity forms — what the command layer's bind cache consumes.
        [[nodiscard]] static RHI::ResourceHandle GetGlobalIrradianceMapHandle()
        {
            return s_Data.GlobalIrradianceMapID;
        }
        [[nodiscard]] static RHI::ResourceHandle GetGlobalPrefilterMapHandle()
        {
            return s_Data.GlobalPrefilterMapID;
        }
        [[nodiscard]] static RHI::ResourceHandle GetGlobalBRDFLutMapHandle()
        {
            return s_Data.GlobalBRDFLutMapID;
        }
        [[nodiscard]] static RHI::ResourceHandle GetGlobalEnvironmentMapHandle()
        {
            return s_Data.GlobalEnvironmentMapID;
        }

        // Native forms — what RenderPipeline's graph IMPORT consumes, and only
        // that. These cannot migrate with the rest: DeferredLightingPass reads
        // the imported IBL resources back through context.ResolveTexture, which
        // answers 0 for anything imported by handle (see
        // docs/agent-rules/rhi-abstraction-boundary.md). Moving the import
        // without moving that reader would silently drop IBL from the deferred
        // path — lit scene, no ambient, no error.
        // Nearest wavy water-surface depth captured by WaterRenderPass this frame
        // (0 when no water rendered). Consumed by the underwater-fog stage in the
        // ToneMap pass to find the per-pixel water boundary. See §7.2.
        static void SetWaterSurfaceDepthTextureID(RHI::ResourceHandle id)
        {
            s_Data.WaterSurfaceDepthTextureID = id;
        }
        [[nodiscard]] static RHI::ResourceHandle GetWaterSurfaceDepthTextureID()
        {
            return s_Data.WaterSurfaceDepthTextureID;
        }
        // Planar-reflection colour texture published by PlanarReflectionRenderPass
        // each frame (0 when reflection is disabled / unavailable). Sampled by
        // WaterRenderPass at TEX_WATER_PLANAR_REFLECTION.
        static void SetPlanarReflectionTextureID(RHI::ResourceHandle id)
        {
            s_Data.PlanarReflectionTextureID = id;
        }
        [[nodiscard]] static RHI::ResourceHandle GetPlanarReflectionTextureID()
        {
            return s_Data.PlanarReflectionTextureID;
        }
        // Per-frame planar-reflection state, pushed from Scene.cpp during water
        // submission and forwarded to PlanarReflectionRenderPass at EndScene.
        // `plane` is the world-space reflection plane vec4(n.xyz, d); `enabled`
        // is the combined gate (a reflective water surface exists AND the active
        // path supports the replay).
        static void SetPlanarReflectionState(const glm::vec4& plane, bool enabled, f32 intensity, f32 distortion)
        {
            s_Data.PlanarReflectionPlane = plane;
            s_Data.PlanarReflectionEnabled = enabled;
            s_Data.PlanarReflectionIntensity = intensity;
            s_Data.PlanarReflectionDistortion = distortion;
        }
        [[nodiscard]] static f32 GetGlobalIBLIntensity()
        {
            return s_Data.GlobalIBLIntensity;
        }

        // (The ephemeral MCP sun-direction override from #316 was
        // retired by issue #633: olo_scene_set_time_of_day / set_sun_angle now
        // write the scene's TimeOfDayComponent — a real, serialized clock —
        // and TimeOfDaySystem drives the directional light + sky from it.)

        // Culling methods
        static void EnableFrustumCulling(bool enable);
        static bool IsFrustumCullingEnabled();
        static void EnableDynamicCulling(bool enable);
        static bool IsDynamicCullingEnabled();
        static const Frustum& GetViewFrustum();
        static bool IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform);
        static bool IsVisibleInFrustum(const BoundingSphere& sphere);
        static bool IsVisibleInFrustum(const BoundingBox& box);

        // ---- Observer camera / frozen culling camera (issue #726) ----------
        //
        // THE ONE RULE: everything that DECIDES WHAT IS DRAWN reads the
        // *culling* camera; everything that decides HOW IT LOOKS ON SCREEN
        // reads the *render* camera. They are the same matrices until the
        // freeze is toggled on, at which point the culling camera stops
        // following the render camera and the viewport becomes a free-flying
        // observer looking at the frozen cut from outside.
        //
        // The property that makes this a diagnostic instead of a curiosity:
        // anything the frozen camera culled STAYS culled, so it is visibly
        // missing when you fly around it. Every site that quietly reads "the
        // camera" without saying which one is therefore a latent lie — the
        // enumeration of what is frozen and what deliberately is not lives in
        // docs/agent-rules/observer-camera.md.
        static void SetCullingCameraFrozen(bool frozen);
        [[nodiscard]] static bool IsCullingCameraFrozen();
        // World-space culling camera. Equal to the render camera's matrices
        // while unfrozen; pinned to the freeze-time values while frozen.
        [[nodiscard]] static const glm::mat4& GetCullViewMatrix();
        [[nodiscard]] static const glm::mat4& GetCullProjectionMatrix();
        [[nodiscard]] static const glm::mat4& GetCullViewProjectionMatrix();
        [[nodiscard]] static const glm::vec3& GetCullViewPosition();
        [[nodiscard]] static f32 GetCullNearClip();
        [[nodiscard]] static f32 GetCullFarClip();
        // Render-origin-relative forms, for the GPU cull dispatches (which read
        // render-relative instance transforms). Valid from PrepareFrame onward.
        [[nodiscard]] static const glm::mat4& GetCullViewProjectionRelative();
        [[nodiscard]] static const glm::vec3& GetCullViewPositionRelative();
        // x = |cull projection[1][1]|, y = cull near-plane distance.
        [[nodiscard]] static const glm::vec2& GetCullProjParams();
        // The eight world-space corners of a view-projection's frustum, indexed
        // by the ShaderDebugDrawBox corner convention (bit0 = +x, bit1 = +y,
        // bit2 = +z in NDC) so the debug-draw expansion recovers the 12 edges as
        // "every pair differing in exactly one bit". Unprojects the GL-convention
        // NDC cube through inverse(viewProjection), so it is exact for
        // perspective and orthographic alike rather than re-deriving from a
        // fov/aspect the projection may not have come from.
        [[nodiscard]] static std::array<glm::vec3, 8> ComputeFrustumCorners(const glm::mat4& viewProjection);
        // Draw a view-projection's frustum as a wireframe box through the
        // shader-debug-draw channel (issue #725). Requires
        // RendererSettings::ShaderDebugDrawEnabled — the pass that consumes the
        // channel is not declared otherwise.
        static void DrawFrustumWireframe(const glm::mat4& viewProjection,
                                         const glm::vec3& color = glm::vec3(1.0f, 0.85f, 0.2f));

        // Debug culling methods
        static void SetForceDisableCulling(bool disable);
        static bool IsForceDisableCulling();

        // Depth prepass control
        static void EnableDepthPrepass(bool enable);
        static bool IsDepthPrepassEnabled();
        // The depth-prepass value ApplyRendererSettings derives from the current
        // RendererSettings (the "auto" state): the user toggle OR'd with the
        // paths that require the prepass depth for tile culling (Forward+,
        // Deferred, Forward with auto-switch). Exposed so the MCP depthprepass
        // lever's 'auto' token resolves through the same single derivation.
        static bool ComputeSettingsDerivedDepthPrepass();

        // Occlusion culling control (legacy CPU hardware-query path).
        static void EnableOcclusionCulling(bool enable);
        static bool IsOcclusionCullingEnabled();

        // GPU Hi-Z occlusion culling for instanced static geometry (#431).
        // Independent of the legacy query path above: this drives the compute
        // cull's HZB occlusion test against the previous frame's retained depth
        // pyramid. Off by default. SetHZBOcclusionDepthBias tunes the device-Z
        // slack (larger = more conservative, fewer false culls).
        static void EnableHZBOcclusionCulling(bool enable);
        static bool IsHZBOcclusionCullingEnabled();
        static void SetHZBOcclusionDepthBias(f32 bias);

        // Forward+ light culling control
        static TiledForwardPlus& GetForwardPlus()
        {
            return s_Data.ForwardPlus;
        }
        // Distance-impostor reflection probe arrays (issue #705)
        static ReflectionProbeArray& GetReflectionProbes()
        {
            return s_Data.ReflectionProbes;
        }
        static bool IsForwardPlusActive()
        {
            return s_Data.ForwardPlus.IsActive();
        }
        static Ref<VertexArray> GetFullscreenQuadVAO()
        {
            return s_Data.FullscreenQuadVAO;
        }
        static Ref<Shader> GetForwardPlusDebugShader()
        {
            return s_Data.ForwardPlusDebugShader;
        }
        static const glm::mat4& GetViewMatrix()
        {
            return s_Data.ViewMatrix;
        }
        static const glm::mat4& GetProjectionMatrix()
        {
            return s_Data.ProjectionMatrix;
        }

        // Per-frame camera-relative render origin (issue #429). Every world
        // position/matrix is uploaded to the GPU relative to this point so the
        // GPU works with small coordinates far from the world origin. It is the
        // camera position snapped to a coarse grid (see ComputeRenderOrigin),
        // so it is exactly (0,0,0) within the first cell — making the whole
        // feature a no-op near origin — and only changes on cell crossings.
        // Shadow passes, light uploads, decals, etc. read this to stay in the
        // same relative space as the main camera. Set once per frame in
        // RenderPipeline::PrepareFrame.
        static const glm::vec3& GetRenderOrigin()
        {
            return s_Data.RenderOrigin;
        }

        // Debug / bisect lever for camera-relative rendering (issue #429). When
        // disabled, the render origin is forced to (0,0,0) every frame, which
        // makes every origin-relative upload site fall back to the exact
        // pre-#429 world-space behaviour (large coordinates jitter far from
        // origin). Default on. Mirrors OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL: it
        // exists to A/B the feature (e.g. capture before/after visual evidence)
        // and to isolate a regression, not as a user-facing setting.
        static void SetCameraRelativeEnabled(bool enabled)
        {
            s_Data.CameraRelativeEnabled = enabled;
        }
        static bool IsCameraRelativeEnabled()
        {
            return s_Data.CameraRelativeEnabled;
        }

        // Global renderer settings (rendering path, culling, debug overlays)
        static RendererSettings& GetRendererSettings()
        {
            return s_Data.Settings;
        }
        static void ApplyRendererSettings();

        // Statistics and debug methods
        static Statistics& GetStats();
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

        static bool IsShadowPassAvailable();

        // Window resize handling
        static void OnWindowResize(u32 width, u32 height);

        static Ref<Framebuffer> ResolveFrameGraphFramebuffer(std::string_view resourceName)
        {
            if (!s_Data.RGraph)
            {
                return nullptr;
            }

            return s_Data.RGraph->ResolveFramebuffer(s_Data.RGraph->GetFramebufferHandle(resourceName));
        }

        static u32 ResolveFrameGraphTexture(std::string_view resourceName)
        {
            if (!s_Data.RGraph)
            {
                return 0;
            }

            return s_Data.RGraph->ResolveTexture(s_Data.RGraph->GetTextureHandle(resourceName));
        }

        // Identity sibling of the above (issue #691). NOT interchangeable
        // with it: `textureID` and `identity` are ALTERNATIVES on a
        // PhysicalTexture, so a resource imported through ImportTextureHandle
        // has an identity and NO native id — ResolveTexture answers 0 for it —
        // while a natively-imported one is the other way round. A caller that
        // wants "whichever this resource happens to carry" must try both; the
        // MCP diagnostics do exactly that, and must, or migrating an import
        // silently deletes the resource from olo_render_capture_target.
        static RHI::ResourceHandle ResolveFrameGraphTextureHandle(std::string_view resourceName)
        {
            if (!s_Data.RGraph)
            {
                return RHI::NullResource;
            }

            return s_Data.RGraph->ResolveTextureHandle(s_Data.RGraph->GetTextureHandle(resourceName));
        }

        // Dynamic Resolution Scaling.
        // scale is clamped to [0.25, 1.0]; use 1.0 to disable DRS.
        // The render graph forwards the scale to all registered render passes
        // via ApplyRenderViewport, and the DRS UBO (binding 33) is updated
        // each frame during `RenderPipeline::PrepareFrame(...)` so shaders can
        // clamp screen-space UVs.
        static void SetRenderScale(f32 scale);
        static f32 GetRenderScale()
        {
            return s_Data.RGraph ? s_Data.RGraph->GetRenderScale() : 1.0f;
        }

        // Driver-advertised max MSAA samples (min of colour/depth texture
        // caps). Zero until Renderer3D::Init has queried the GL limits.
        // The editor settings panel uses this to disable combo entries
        // the GPU can't support.
        static u32 GetMaxMSAASamples()
        {
            if (s_Data.MaxMSAASamplesColor == 0 || s_Data.MaxMSAASamplesDepth == 0)
                return 0;
            return std::min(s_Data.MaxMSAASamplesColor, s_Data.MaxMSAASamplesDepth);
        }

        static void SetParticleRenderCallback(RenderCallback callback);

        /// Queue one fluid domain's screen-space draw for this frame (issue
        /// #630). Consumed by FluidIntermediatesPass/FluidCompositePass; the
        /// list clears every frame like the particle render callback.
        static void SubmitFluidDraw(const FluidRenderData& draw);

        static void SetUICompositeRenderCallback(RenderCallback callback);

        static void SetSelectionOutlineEnabled(bool enabled)
        {
            s_Data.EnableSelectionOutline = enabled;
        }

        static void SetSelectionOutlineEntityIDs(const std::vector<i32>& ids);

        static bool IsSelectionOutlineEnabled()
        {
            return s_Data.EnableSelectionOutline;
        }

        static ShadowMap& GetShadowMap()
        {
            return s_Data.Shadow;
        }

        static void AddMeshShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                        RHI::ResourceHandle shadowVaoID = {}, const BoundingBox& worldBounds = NoBounds,
                                        bool twoSided = false);

        static void AddSkinnedShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                           u32 boneBufferOffset, u32 boneCount, const BoundingBox& worldBounds = NoBounds);

        static void AddTerrainShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 patchVertexCount,
                                           const glm::mat4& transform, RHI::ResourceHandle heightmapTextureID,
                                           const ShaderBindingLayout::TerrainUBO& terrainUBO);

        // instanceCount > 0 selects the packed-quad depth shader + instanced
        // draw (issue #727); 0 keeps the marching-cubes triangle-soup path.
        static void AddVoxelShadowCaster(RHI::ResourceHandle vaoID, u32 indexCount, const glm::mat4& transform,
                                         u32 instanceCount = 0);

        static void AddFoliageShadowCaster(FoliageRenderer* renderer, const Ref<Shader>& depthShader, f32 time);

        // --- Realtime DDGI (#632) ---
        // Scene-side per-frame submission for the active Realtime/Hybrid
        // LightProbeVolumeComponent (called from ProcessScene3DSharedLogic's
        // probe block, before the mesh traversal). No-op when the DDGI pass
        // doesn't exist (renderer not initialized for 3D).
        static void SubmitDDGIVolume(const DDGIVolumeDesc& desc);
        // True while the DDGI pass wants capture geometry this frame — the
        // mesh submission sites gate their AddDDGICaster calls on this so the
        // caster list costs nothing when DDGI is idle.
        [[nodiscard]] static bool IsDDGICollectingCasters();
        static void AddDDGICaster(const DDGIMeshCaster& caster);
        // The pass itself, for the editor debug viz + MCP capture accessors
        // (may be null before Init / after Shutdown).
        [[nodiscard]] static DDGIProbeUpdatePass* GetDDGIPass();

        // Auxiliary mesh-caster sink (issue #705). While set, the scene's
        // SubmitDDGICasterIfCollecting sites ALSO append to this vector, so a
        // bake path (ReflectionProbeBaker's distance capture) can borrow the
        // DDGI caster enumeration for one RenderScene3D without a DDGI volume
        // being active. Caller owns the vector and MUST clear the sink
        // (nullptr) before it goes out of scope.
        static void SetAuxCasterSink(std::vector<DDGIMeshCaster>* sink);

        // @brief Record this frame's transform for an entity and return the
        // previous frame's transform (or the current one if no history exists
        // yet, producing zero velocity). Only called from Deferred submission
        // paths so the cache stays empty when Deferred is not active.
        //
        // Returns by value so the early-out path (entityID < 0) does not hand
        // back a reference to the caller's `currTransform` argument — which
        // would dangle once the submission helper's stack frame exited — and
        // the map-miss branch does not rely on the lifetime of the node
        // returned by `insert_or_assign`.
        static glm::mat4 GetAndRecordPrevTransform(i32 entityID, const glm::mat4& currTransform)
        {
            if (entityID < 0)
                return currTransform;
            s_Data.CurrEntityTransforms.insert_or_assign(entityID, currTransform);
            auto prevIt = s_Data.PrevEntityTransforms.find(entityID);
            return prevIt != s_Data.PrevEntityTransforms.end() ? prevIt->second : currTransform;
        }

        // @brief Instanced variant: record this frame's full transform array for a
        // mesh and return the previous frame's array (or a copy of current if no
        // history exists yet, producing zero velocity). Called only from Deferred
        // submission of DrawMeshInstanced so the cache stays empty in Forward/+.
        // Instance ordering is assumed stable frame-to-frame (foliage / particle /
        // constant-count emitters).
        //
        // `ownerKey` identifies the submission source (entity UUID, emitter ID,
        // foliage chunk index, ...). When two different owners render the same
        // mesh with their own instance arrays, keying by meshHandle alone would
        // make them overwrite each other's history and produce garbage motion
        // vectors; compose with the owner so each owner keeps its own stream.
        // Pass `0` to preserve legacy mesh-only keying.
        //
        // History is keyed and recorded from the **full, stable pre-cull**
        // `currFullTransforms` list so per-instance identity is preserved
        // across frames even when frustum culling drops different subsets each
        // frame. If `visibleIndices` is non-null, the returned prev array is
        // projected onto the visible subset (prevOut[i] = prevFull[visibleIndices[i]]);
        // otherwise the full prev array is returned. Sizing compatibility is
        // checked against `currFullTransforms.size()`, so a stable full-list
        // size keeps history valid even as visible counts fluctuate.
        static std::vector<glm::mat4> GetAndRecordPrevInstanceTransforms(u64 meshKey, u64 ownerKey,
                                                                         const std::vector<glm::mat4>& currFullTransforms,
                                                                         const std::vector<u32>* visibleIndices = nullptr,
                                                                         bool* outUsedFallback = nullptr)
        {
            // Hash-combine (Boost formula) -- cheap, order-sensitive, and the
            // result preserves the original mesh-only key when ownerKey == 0.
            u64 combinedKey = meshKey;
            if (ownerKey != 0)
                combinedKey ^= ownerKey + 0x9e3779b97f4a7c15ULL + (combinedKey << 6) + (combinedKey >> 2);

            // Record the **full pre-cull** list so per-instance identity
            // survives across frames regardless of which instances were
            // visible this frame.
            s_Data.CurrInstanceTransforms.insert_or_assign(combinedKey, currFullTransforms);

            auto prevIt = s_Data.PrevInstanceTransforms.find(combinedKey);
            const bool haveHistory = (prevIt != s_Data.PrevInstanceTransforms.end()) &&
                                     (prevIt->second.size() == currFullTransforms.size());

            auto project = [&visibleIndices](const std::vector<glm::mat4>& src) -> std::vector<glm::mat4>
            {
                if (!visibleIndices)
                    return src;
                std::vector<glm::mat4> out;
                out.reserve(visibleIndices->size());
                for (u32 idx : *visibleIndices)
                {
                    if (idx < src.size())
                        out.push_back(src[idx]);
                    else
                        out.emplace_back(1.0f); // Defensive guard — should not happen.
                }
                return out;
            };

            if (haveHistory)
            {
                if (outUsedFallback)
                    *outUsedFallback = false;
                return project(prevIt->second);
            }

            // First frame or size mismatch -> alias current visible subset
            // (zero motion). Signal the caller so it can skip the redundant
            // allocation/upload instead of relying on pointer identity.
            if (outUsedFallback)
                *outUsedFallback = true;
            return project(currFullTransforms);
        }

        // Depth-prepass shader swap: while the scene depth prepass is active,
        // CommandDispatch binds these minimal depth-only programs in place of
        // the full PBR forward / G-Buffer shaders so covered fragments never
        // run the expensive lighting fragment shader. Returned as one renderer-
        // ID snapshot (taken per prepass activation) so the per-draw resolve is
        // a handful of integer compares. IDs are 0 while shaders are unloaded,
        // which disables the swap safely.
        // Identities, not driver names (issue #691): these are
        // compared against PODMaterialData::shaderRendererID to decide whether a
        // material's program may be swapped for the depth-only one, and that
        // field is an identity now. Comparing programs by GL name across a
        // shader HOT-RELOAD is also unsound — a relinked program can be handed
        // the name a different program just freed.
        struct DepthPrepassShaderIDs
        {
            // Standard mesh programs eligible for the swap
            RHI::ResourceHandle PBRStatic{};
            RHI::ResourceHandle PBRSkinned{};
            RHI::ResourceHandle GBufferStatic{};
            RHI::ResourceHandle GBufferSkinned{};
            // Replacement depth-only programs (DepthPrepass*.glsl)
            RHI::ResourceHandle DepthStatic{};
            RHI::ResourceHandle DepthSkinned{};
            RHI::ResourceHandle DepthMaskStatic{};
            RHI::ResourceHandle DepthMaskSkinned{};
        };
        static DepthPrepassShaderIDs GetDepthPrepassShaderIDs()
        {
            DepthPrepassShaderIDs ids;
            if (s_Data.PBRMultiLightShader)
                ids.PBRStatic = s_Data.PBRMultiLightShader->GetRHIHandle();
            if (s_Data.PBRMultiLightSkinnedShader)
                ids.PBRSkinned = s_Data.PBRMultiLightSkinnedShader->GetRHIHandle();
            if (s_Data.PBRGBufferShader)
                ids.GBufferStatic = s_Data.PBRGBufferShader->GetRHIHandle();
            if (s_Data.PBRGBufferSkinnedShader)
                ids.GBufferSkinned = s_Data.PBRGBufferSkinnedShader->GetRHIHandle();
            if (s_Data.DepthPrepassShader)
                ids.DepthStatic = s_Data.DepthPrepassShader->GetRHIHandle();
            if (s_Data.DepthPrepassSkinnedShader)
                ids.DepthSkinned = s_Data.DepthPrepassSkinnedShader->GetRHIHandle();
            if (s_Data.DepthPrepassMaskShader)
                ids.DepthMaskStatic = s_Data.DepthPrepassMaskShader->GetRHIHandle();
            if (s_Data.DepthPrepassMaskSkinnedShader)
                ids.DepthMaskSkinned = s_Data.DepthPrepassMaskSkinnedShader->GetRHIHandle();
            return ids;
        }

        // Depth-only water program for the surface-depth capture
        // (Water_Depth.glsl). Snapshot per capture in
        // CommandDispatch::SetWaterDepthCaptureActive so shader hot-reloads
        // are picked up — same rationale as the prepass IDs above. Null while
        // shaders are unloaded, which disables the swap safely.
        static RHI::ResourceHandle GetWaterDepthShaderID()
        {
            return s_Data.WaterDepthShader ? s_Data.WaterDepthShader->GetRHIHandle() : RHI::ResourceHandle{};
        }

        static Ref<Shader> GetTerrainPBRShader()
        {
            return s_Data.TerrainPBRShader;
        }
        static Ref<Shader> GetTerrainDepthShader()
        {
            return s_Data.TerrainDepthShader;
        }
        static Ref<Shader> GetVoxelPBRShader()
        {
            return s_Data.VoxelPBRShader;
        }
        static Ref<Shader> GetVoxelDepthShader()
        {
            return s_Data.VoxelDepthShader;
        }
        // Packed-quad (binary greedy meshing) siblings — issue #727.
        static Ref<Shader> GetVoxelGreedyPBRShader()
        {
            return s_Data.VoxelGreedyPBRShader;
        }
        static Ref<Shader> GetVoxelGreedyDepthShader()
        {
            return s_Data.VoxelGreedyDepthShader;
        }
        static Ref<Shader> GetFoliageShader()
        {
            return s_Data.FoliageShader;
        }
        static Ref<Shader> GetFoliageDepthShader()
        {
            return s_Data.FoliageDepthShader;
        }
        static Ref<Shader> GetDecalShader()
        {
            return s_Data.DecalShader;
        }
        static Ref<Mesh> GetDecalCubeMesh()
        {
            return s_Data.DecalCubeMesh;
        }
        static Ref<Texture2D> GetWhiteTexture()
        {
            return s_Data.WhiteTexture;
        }
        static Ref<UniformBuffer> GetDecalUBO()
        {
            return s_Data.DecalUBO;
        }
        static Ref<UniformBuffer> GetTerrainUBO()
        {
            return s_Data.TerrainUBO;
        }
        static Ref<UniformBuffer> GetFoliageUBO()
        {
            return s_Data.FoliageUBO;
        }
        static Ref<UniformBuffer> GetWaterUBO()
        {
            return s_Data.WaterUBO;
        }
        // Per-draw instance data SSBO (binding = 15). Every mesh / shadow /
        // decal / foliage / water shader reads its model transform from here
        // via InstanceBlock.glsl; the legacy ModelMatrixUBO at binding 3 has
        // been retired.
        static Ref<InstanceBuffer> GetModelInstanceBuffer()
        {
            return s_Data.ModelInstanceBuffer;
        }

        static PostProcessSettings& GetPostProcessSettings()
        {
            return s_Data.PostProcess;
        }

        static SnowSettings& GetSnowSettings()
        {
            return s_Data.Snow;
        }

        static FogSettings& GetFogSettings()
        {
            return s_Data.Fog;
        }

        static void UploadFogVolumes(const FogVolumesUBOData& data);

        // OpenVDB-imported density volume (#724) bound by VolumetricFogPass
        // for FogVolumeShape::Texture3D. First-slice limitation: exactly one
        // texture at a time, chosen by Scene.cpp as the highest-priority
        // enabled Texture3D-shaped FogVolumeComponent with a resolved
        // VolumeAsset; nullptr when none is active this frame.
        static void UploadFogVolumeDensityTexture(const Ref<Texture3D>& texture)
        {
            s_Data.SceneEffectsGPU.FogVolumeDensityTexture = texture;
        }
        [[nodiscard]] static const Ref<Texture3D>& GetFogVolumeDensityTexture()
        {
            return s_Data.SceneEffectsGPU.FogVolumeDensityTexture;
        }

        // Underwater fog — runtime state for the camera-below-water pass
        // (WATER_FUTURE_IMPROVEMENTS.md §7.2). Populated each frame by the
        // scene's water update loop; consumed by `UnderwaterFogRenderPass`.
        static void SetUnderwaterFogState(const UnderwaterFogState& state)
        {
            s_Data.UnderwaterFog = state;
        }
        [[nodiscard]] static const UnderwaterFogState& GetUnderwaterFogState()
        {
            return s_Data.UnderwaterFog;
        }
        [[nodiscard]] static Ref<UniformBuffer> GetUnderwaterFogUBO()
        {
            return s_Data.UnderwaterFogBuffer;
        }
        static void UploadUnderwaterFogUBO(const UnderwaterFogUBOData& data);

        // Decal rendering (submits DrawDecalCommand to DecalRenderPass bucket)
        static CommandPacket* DrawDecal(
            const glm::mat4& decalTransform,
            const glm::mat4& inverseDecalTransform,
            const glm::vec4& decalColor,
            const glm::vec4& decalParams,
            RHI::ResourceHandle albedoTextureID,
            i32 entityID = -1);

        // Extended decal rendering — mode picks the G-Buffer channel (0=Albedo,
        // 1=Normal, 2=RMA) and the corresponding shader variant. Pass the matching
        // texture for that mode in the relevant slot; unused slots can be 0.
        // `transparent` forces the forward (alpha-blended / WB-OIT) path even
        // when the active RenderingPath is Deferred — the packet is then drained
        // by the graph-scheduled DecalRenderPass::Execute() instead of the
        // in-scene G-Buffer overlay drain.
        static CommandPacket* DrawDecal(
            const glm::mat4& decalTransform,
            const glm::mat4& inverseDecalTransform,
            const glm::vec4& decalColor,
            const glm::vec4& decalParams,
            RHI::ResourceHandle albedoTextureID,
            RHI::ResourceHandle normalTextureID,
            RHI::ResourceHandle rmaTextureID,
            DrawDecalCommand::DecalMode mode,
            bool transparent,
            i32 entityID = -1);

        // Octahedral impostor parameters for a foliage layer (issue #433).
        // Enabled == false selects the flat-billboard path (all other fields
        // ignored); the atlas texture IDs are the baked octahedral atlases.
        struct FoliageImpostorParams
        {
            bool Enabled = false;
            RHI::ResourceHandle AlbedoAtlasID{};      // rgb + coverage
            RHI::ResourceHandle NormalDepthAtlasID{}; // obj normal + card depth
            u32 FramesPerAxis = 8;
            bool Hemi = true;
            f32 StartDistance = 40.0f;
            f32 TransitionBand = 15.0f;
            f32 Radius = 1.0f;
            f32 ParallaxScale = 0.5f;
        };

        // Foliage rendering. This function owns submission as well as packet
        // allocation so the two operations cannot select different streams.
        static void DrawFoliageLayer(
            RHI::ResourceHandle vertexArrayID, u32 indexCount, u32 instanceCount,
            RHI::ResourceHandle albedoTextureID,
            const glm::mat4& modelTransform,
            f32 time,
            f32 prevTime,
            f32 windStrength, f32 windSpeed,
            f32 viewDistance, f32 fadeStart, f32 alphaCutoff,
            const glm::vec4& baseColor,
            const BoundingBox& layerBounds,
            i32 entityID,
            // Not defaulted: `= {}` would require FoliageImpostorParams's default
            // member initializers inside the enclosing class body, which Clang
            // rejects (MSVC accepts it non-conformingly). The sole caller passes
            // it explicitly.
            const FoliageImpostorParams& impostor);

        // Water rendering parameters (grouped to avoid 25+ parameter function)
        struct WaterDrawParams
        {
            glm::vec4 waveParams = glm::vec4(0.0f);
            glm::vec4 waveDir0 = glm::vec4(0.0f);
            glm::vec4 waveDir1 = glm::vec4(0.0f);
            glm::vec4 waterColor = glm::vec4(0.0f);
            glm::vec4 waterDeepColor = glm::vec4(0.0f);
            glm::vec4 visualParams = glm::vec4(0.0f);
            glm::vec4 normalMapScroll = glm::vec4(0.0f);
            glm::vec4 normalMapSpeed = glm::vec4(0.0f);
            glm::vec4 lightDirection = glm::vec4(0.0f);
            glm::vec4 depthRefractionParams = glm::vec4(0.0f);
            glm::vec4 refractionColor = glm::vec4(0.0f);
            glm::vec4 foamParams = glm::vec4(0.0f);
            glm::vec4 foamParams2 = glm::vec4(0.0f);
            glm::vec4 sssColor = glm::vec4(0.0f);
            glm::vec4 ssrParams = glm::vec4(0.0f);
            glm::vec4 tessParams = glm::vec4(0.0f);
            // FFT ocean (WATER_FUTURE_IMPROVEMENTS.md §1): x = useFFT (0/1),
            // y = 1/patchSize, z = heightScale, w = horizontalScale.
            glm::vec4 fftParams = glm::vec4(0.0f);
            glm::vec4 fftCascadeParams = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
            RHI::ResourceHandle normalMap0ID{};
            RHI::ResourceHandle normalMap1ID{};
            RHI::ResourceHandle noiseTextureID{};
            RHI::ResourceHandle foamTextureID{};
            RHI::ResourceHandle fftDisplacementID{}; // rgb = (dx,h,dz), a = foam
            RHI::ResourceHandle fftDerivativesID{};  // rgb = normal, a = jacobian
            bool refractionEnabled = true;
            bool ssrEnabled = true;
            // When true the water plane draws double-sided so it stays visible
            // from below; the fragment shader keeps the correct side per
            // fragment via the waterline discard (§7.2). When false it is
            // single-sided back-culled (original top-down behaviour).
            bool renderFromBelow = true;
        };

        // Water rendering (submits DrawWaterCommand to WaterRenderPass bucket)
        static CommandPacket* DrawWaterSurface(
            RHI::ResourceHandle vertexArrayID, u32 indexCount,
            const glm::mat4& modelTransform,
            f32 time,
            f32 prevTime,
            const WaterDrawParams& params,
            const BoundingBox& bounds,
            i32 entityID = -1);

        static WindSettings& GetWindSettings()
        {
            return s_Data.Wind;
        }

        static SnowAccumulationSettings& GetSnowAccumulationSettings()
        {
            return s_Data.SnowAccumulation;
        }

        /// Boat / actor wake foam field settings (issue #967). Published by the
        /// scene, consumed by RenderPipeline's disturbance dispatch.
        static WaterDisturbanceSettings& GetWaterDisturbanceSettings()
        {
            return s_Data.WaterDisturbance;
        }

        /// Boat / actor wake SHAPE settings (issue #968). Published by the
        /// scene, forwarded to WaterWakeSystem by RenderPipeline. The RENDER
        /// half only — the physics half rides on WaterProbe::Volume, so a
        /// headless tick that never reaches this still shapes the surface
        /// buoyancy samples.
        static WaterWakeSettings& GetWaterWakeSettings()
        {
            return s_Data.WaterWakeShape;
        }

        static SnowEjectaSettings& GetSnowEjectaSettings()
        {
            return s_Data.SnowEjecta;
        }

        static PrecipitationSettings& GetPrecipitationSettings()
        {
            return s_Data.Precipitation;
        }

        // Per-frame cloudscape / atmosphere state (issue #633). Written by
        // Scene::ProcessScene3DSharedLogic from the first enabled
        // CloudscapeComponent (+ weather director blend + wetness), reset to
        // defaults by BeginScene when the scene stops publishing — consumed by
        // RenderPipeline for the cloud raymarch pass, the cloud-shadow compute
        // and the AtmosphereShadingUBO upload.
        static void SetCloudscapeState(const CloudscapeRenderState& state)
        {
            s_Data.Cloudscape = state;
        }
        [[nodiscard]] static const CloudscapeRenderState& GetCloudscapeState()
        {
            return s_Data.Cloudscape;
        }

        // Shader library access for PBR material shader selection
        static ShaderLibrary& GetShaderLibrary();

        // The filepaths Init() loads into GetShaderLibrary() — exposed (issue
        // #908) so the headless ShaderPack bake enumerates EXACTLY what this
        // library will later try to serve from the pack, with no second list
        // that could drift from the real one. No GL call, no side effect.
        [[nodiscard]] static std::vector<std::string> GetShaderFilepaths();

        template<typename T>
        static CommandPacket* CreateRenderStreamDrawCall(RenderStreamType stream)
        {
            OLO_PROFILE_FUNCTION();

            if (auto* streamNode = GetRenderStreamNode(stream))
                return streamNode->CreateDrawCall<T>();

            OLO_CORE_WARN("Renderer3D::CreateRenderStreamDrawCall: Requested render stream is unavailable!");
            return nullptr;
        }

        static void SubmitRenderStreamPacket(RenderStreamType stream, CommandPacket* packet);

        template<typename T>
        static CommandPacket* CreateDrawCall()
        {
            return CreateRenderStreamDrawCall<T>(RenderStreamType::Geometry);
        }

        static void SubmitPacket(CommandPacket* packet)
        {
            SubmitRenderStreamPacket(RenderStreamType::Geometry, packet);
        }

        template<typename T>
        static CommandPacket* CreateDecalDrawCall()
        {
            return CreateRenderStreamDrawCall<T>(RenderStreamType::Decal);
        }

        static void SubmitDecalPacket(CommandPacket* packet)
        {
            SubmitRenderStreamPacket(RenderStreamType::Decal, packet);
        }

        template<typename T>
        static CommandPacket* CreateFoliageDrawCall()
        {
            return CreateRenderStreamDrawCall<T>(RenderStreamType::Foliage);
        }

        template<typename T>
        static CommandPacket* CreateForwardOverlayDrawCall()
        {
            return CreateRenderStreamDrawCall<T>(RenderStreamType::ForwardOverlay);
        }

        static void SubmitForwardOverlayPacket(CommandPacket* packet)
        {
            SubmitRenderStreamPacket(RenderStreamType::ForwardOverlay, packet);
        }

        template<typename T>
        static CommandPacket* CreateWaterDrawCall()
        {
            return CreateRenderStreamDrawCall<T>(RenderStreamType::Water);
        }

        static void SubmitWaterPacket(CommandPacket* packet)
        {
            SubmitRenderStreamPacket(RenderStreamType::Water, packet);
        }

      private:
        struct SceneBindingUBOs
        {
            Ref<UniformBuffer> Camera;
            Ref<UniformBuffer> Material;

            void Reset()
            {
                Camera.Reset();
                Material.Reset();
            }
        };

        struct PostProcessGPUState
        {
            Ref<UniformBuffer> PostProcess;
            Ref<UniformBuffer> MotionBlur;
            Ref<UniformBuffer> SSAO;
            Ref<UniformBuffer> GTAO;
            Ref<UniformBuffer> SSR;
            Ref<UniformBuffer> SSGI;
            Ref<UniformBuffer> ContactShadow;

            PostProcessUBOData PostProcessData{};
            MotionBlurUBOData MotionBlurData{};
            SSAOUBOData SSAOData{};
            UBOStructures::GTAOUBO GTAOData{};
            SSRUBOData SSRData{};
            SSGIUBOData SSGIData{};
            ContactShadowUBOData ContactShadowData{};

            void Reset()
            {
                PostProcess.Reset();
                MotionBlur.Reset();
                SSAO.Reset();
                GTAO.Reset();
                SSR.Reset();
                SSGI.Reset();
                ContactShadow.Reset();
            }
        };

        struct SceneEffectsGPUState
        {
            Ref<UniformBuffer> Snow;
            Ref<UniformBuffer> SSS;
            Ref<UniformBuffer> Fog;
            Ref<UniformBuffer> FogVolumes;
            Ref<UniformBuffer> DRS;

            SnowUBOData SnowData{};
            SSSUBOData SSSData{};
            FogUBOData FogData{};
            FogVolumesUBOData FogVolumesData{};
            DRSUBOData DRSData{};

            // OpenVDB-imported density volume (#724) — not a UBO, so it sits
            // outside the Ref<UniformBuffer>/*UBOData pairing above. See
            // UploadFogVolumeDensityTexture/GetFogVolumeDensityTexture.
            Ref<Texture3D> FogVolumeDensityTexture;

            void Reset()
            {
                Snow.Reset();
                SSS.Reset();
                Fog.Reset();
                FogVolumes.Reset();
                DRS.Reset();
                FogVolumeDensityTexture.Reset();
            }
        };

        struct RenderStreamNodes;
        struct PostProcessPassChain;
        struct SceneCompositionPassSet;
        struct FrameCorePassSet;
        struct RenderStreamPassSet;
        struct RenderPipeline;

        static void SetupRenderGraph(u32 width, u32 height);
        // Rebuild the RenderGraph topology (registered passes + edges) for
        // the given rendering path. Called from SetupRenderGraph on startup
        // and from ApplyRendererSettings when the user switches between
        // Forward / Forward+ / Deferred. Passes that are no-ops in the
        // target path (DeferredLightingPass / ForwardOverlayPass in
        // Forward+/Forward) are simply NOT registered in that topology.
        static void FinalizeConfiguredRenderGraph(RenderingPath path);
        static void ConfigureRenderGraph(RenderingPath path);

        // @brief Returns true if `shader` is one of the engine's
        // G-Buffer-writing variants (PBR / Skybox / LightCube / InfiniteGrid
        // / Terrain / Voxel / Foliage / Decal). Used at material-override
        // submission sites to decide whether a `material.GetShader()` can
        // safely run inside the Deferred ScenePass or whether it has to be
        // rerouted to ForwardOverlayPass to avoid aliasing its outputs onto
        // G-Buffer slots.
        static bool IsDeferredCapableShader(const Ref<Shader>& shader);
        static auto GetRenderStreamNode(RenderStreamType stream) -> CommandBufferRenderPass*;
        static auto ValidateDrawMeshResources(const char* context, RHI::ResourceHandle vertexArray,
                                              RHI::ResourceHandle shader) -> bool;

        // Shared Deferred-vs-forward-overlay shader routing decision for the
        // instanced submission paths (DrawMeshInstanced's CPU-cull path and
        // SubmitGPUCulledInstanced's GPU-cull path). Mirrors DrawMesh's
        // routing exactly; a single implementation keeps the two instanced
        // paths from drifting apart the way the pre-#515 routing bug did.
        struct InstancedShaderRouting
        {
            Ref<Shader> ShaderToUse;
            bool OverlayRoute = false;
        };
        static InstancedShaderRouting SelectInstancedShaderRouting(const Material& material);

        // Builds the DrawMeshInstancedCommand packet for DrawMeshInstanced's
        // CPU-cull path (frustum cull, FrameDataBuffer transform allocation,
        // prev-transform history, shader routing, command construction) OR
        // delegates to SubmitGPUCulledInstanced when the input count crosses
        // `s_Data.GPUCullThreshold`. Does NOT perform the final
        // ForwardOverlayPass submission for the CPU-cull path — instead
        // reports the routing decision via `outOverlayRoute` so the
        // std::span<const InstanceData> overload can patch the packet's
        // Color/Custom/EntityID streams before it is handed off. For the
        // GPU-cull delegate case `outOverlayRoute` is always false: that path
        // fully owns its own submission (overlay or not) and its returned
        // packet is either ready for the caller's normal SubmitPacket or
        // already null because SubmitGPUCulledInstanced handled everything
        // internally.
        static CommandPacket* BuildDrawMeshInstancedPacket(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms,
                                                           const Material& material, bool isStatic, u64 ownerKey,
                                                           bool& outOverlayRoute);

        // GPU-cull submission helper called from DrawMeshInstanced when the
        // input count exceeds `s_Data.GPUCullThreshold`. Builds the full
        // pre-cull InstanceData[], hands it to the GPUFrustumCuller, then
        // attaches the resulting `cullOutputInstanceBufferID` and
        // `cullIndirectBufferID` to the DrawMeshInstancedCommand so the
        // dispatcher takes the indirect-draw path. Returns nullptr on
        // allocation failure or if the cull resources weren't ready.
        static CommandPacket* SubmitGPUCulledInstanced(const Ref<Mesh>& mesh,
                                                       const std::vector<glm::mat4>& transforms,
                                                       const Material& material, bool isStatic,
                                                       u64 ownerKey);

        // Regenerate the persistent Hi-Z occlusion pyramid (#431) from this
        // frame's scene depth. Called at the tail of EndScene() once the render
        // graph has executed (so geometry depth is final). No-op when HZB
        // occlusion is disabled or the scene depth is unavailable. The pyramid
        // is consumed by next frame's GPU instance cull.
        static void GenerateOcclusionHZB();

        // Two-phase occlusion (#431), driven by GPUDrivenOcclusionPass at
        // execute time:
        //   * BuildCurrentOcclusionHZB rebuilds the occlusion pyramid from THIS
        //     frame's partial depth (occluders + phase-1 survivors) and returns
        //     the inputs (reprojected with the CURRENT view-projection) for the
        //     phase-2 re-test. Returns a disabled struct if it can't build.
        //   * DispatchOcclusionPhase2 re-tests one batch's reject list against it.
        //   * GetGPUOcclusionPass exposes the pass for submission-side routing.
        // The first two are public because GPUDrivenOcclusionPass::Execute calls
        // them; GetGPUOcclusionPass stays private (only SubmitGPUCulledInstanced
        // uses it).
      public:
        static GPUFrustumCuller::HZBOcclusionInputs BuildCurrentOcclusionHZB(RHI::ResourceHandle depthTexture,
                                                                             u32 width, u32 height);
        static void DispatchOcclusionPhase2(const GPUFrustumCuller::TwoPhaseCullResult& cull,
                                            const GPUFrustumCuller::HZBOcclusionInputs& currentHZB);

        // The RETAINED occlusion pyramid, i.e. the one GenerateOcclusionHZB()
        // built at the tail of the PREVIOUS EndScene from that frame's FINAL
        // depth — so unlike BuildCurrentOcclusionHZB's mid-frame rebuild it
        // already contains last frame's virtual geometry. Phase 1 of the
        // virtualized-geometry two-phase cull consumes it (issue #682), which is
        // what lets a VG occluder cull the VG behind it.
        //
        // `PrevViewProjection` is last frame's VP made relative to THIS frame's
        // render origin, matching the render-origin-relative instance transforms
        // the cull reads (same treatment RenderPipeline gives the instance cull).
        //
        // ORDERING: valid only until something rebuilds the pyramid this frame.
        // BuildCurrentOcclusionHZB overwrites it in place, so any consumer must
        // read this BEFORE calling that — VirtualGeometryPass runs before
        // DeferredGPUOcclusionPass in the deferred graph for exactly this reason.
        // Returns a non-usable struct on frame 0, with HZB culling off, or after
        // an invalidation (the frustum-and-cone-only fallback).
        [[nodiscard]] static GPUFrustumCuller::HZBOcclusionInputs GetRetainedOcclusionHZB();

      private:
        // Copy render camera -> culling camera unless the culling camera is
        // frozen (issue #726). Called from every BeginScene overload BEFORE
        // PrepareFrame, which is what derives ViewFrustum and uploads the cull
        // camera UBO from these fields.
        static void RefreshCullingCamera();

        [[nodiscard]] static GPUDrivenOcclusionPass* GetGPUOcclusionPass();
        // Deferred two-phase occlusion (#486). Exposes the deferred phase-2 pass
        // for submission-side routing; mirrors GetGPUOcclusionPass for the
        // forward path.
        [[nodiscard]] static DeferredGPUOcclusionPass* GetDeferredGPUOcclusionPass();

      private:
        struct Renderer3DData
        {
            Renderer3DData();
            ~Renderer3DData();

            Ref<Mesh> CubeMesh;
            Ref<Mesh> SphereMesh; // Unit-radius icosphere for DrawSphere / joint markers
            Ref<Mesh> QuadMesh;
            Ref<Mesh> SkyboxMesh;
            Ref<Mesh> LineQuadMesh; // Cached unit-length quad for debug lines
            Ref<Shader> LightCubeShader;
            Ref<Shader> DefaultForwardShader;
            Ref<Shader> DefaultForwardSkinnedShader;
            Ref<Shader> QuadShader;
            Ref<Shader> PBRShader;
            Ref<Shader> PBRSkinnedShader;
            Ref<Shader> PBRMultiLightShader;
            Ref<Shader> PBRMultiLightSkinnedShader;
            Ref<Shader> PBRGBufferShader;        // Deferred: PBR_GBuffer.glsl
            Ref<Shader> PBRGBufferSkinnedShader; // Deferred: PBR_GBuffer_Skinned.glsl
            Ref<Shader> SkyboxShader;
            Ref<Shader> SkyboxGBufferShader;    // Deferred: Skybox_GBuffer.glsl (emissive unlit)
            Ref<Shader> LightCubeGBufferShader; // Deferred: LightCube_GBuffer.glsl (emissive unlit)
            Ref<Shader> InfiniteGridShader;
            Ref<Shader> InfiniteGridGBufferShader; // Deferred: InfiniteGrid_GBuffer.glsl (emissive unlit)
            Ref<Shader> ForwardPlusDebugShader;
            Ref<VertexArray> FullscreenQuadVAO; // Fullscreen quad for grid and post-processing
            SceneBindingUBOs SharedSceneUBOs;
            Ref<UniformBuffer> MultiLightBuffer;
            Ref<UniformBuffer> BoneMatricesUBO;
            Ref<UniformBuffer> PrevBoneMatricesUBO;
            // Per-draw instance data SSBO at ShaderBindingLayout::SSBO_INSTANCE_DATA
            // (= 15), indexed by gl_InstanceIndex in vertex stages and by the
            // flat `v_InstanceIndex` varying in fragment stages. Every
            // mesh-rendering shader reads its model transform from here; the
            // legacy ModelMatrixUBO at binding 3 has been retired.
            Ref<InstanceBuffer> ModelInstanceBuffer;

            // GPU-side per-instance frustum cull pre-pass. Used by
            // `DrawMeshInstanced` when the input count crosses
            // `s_Data.GPUCullThreshold` — the cull compute moves the
            // per-instance sphere test off the CPU and produces a compacted
            // InstanceBuffer + indirect draw command for the actual draw.
            // Null when the compute shader failed to load (engine falls back
            // to the CPU loop in that case).
            Ref<class GPUFrustumCuller> GPUFrustumCuller;
            // Minimum input count required to route a DrawMeshInstanced
            // submission through the GPU cull path. Below this, the CPU
            // loop wins on launch overhead.
            u32 GPUCullThreshold = 1024;

            // Persistent Hi-Z occlusion pyramid (#431). A max-reduction depth
            // pyramid regenerated at the end of every EndScene from this frame's
            // scene depth and RETAINED across frames, so the GPU instance cull —
            // dispatched at submission time, before this frame's depth exists —
            // can additionally reject instances hidden behind last frame's
            // depth. Value-owned (lifetime = engine); Initialize()/Shutdown()
            // bracket the GL context in Renderer3D::Init / Shutdown.
            HZBGenerator OcclusionHZB;
            // Master toggle for the HZB occlusion path. Off by default: it
            // trades a one-frame-latent, conservative occlusion test for the
            // possibility of transient disocclusion popping on fast camera cuts
            // (the two-pass refinement that removes the popping is a documented
            // follow-up). Mirrors RendererSettings::HZBOcclusionCullingEnabled.
            bool HZBOcclusionCullingEnabled = false;
            // True once OcclusionHZB has been generated at least once — guards
            // the first frame, where no retained depth exists yet.
            bool OcclusionHZBValid = false;
            // Device-Z slack subtracted before an instance is rejected as
            // occluded. Larger = more conservative (fewer false culls).
            f32 HZBOcclusionDepthBias = 0.0f;
            PostProcessGPUState PostProcessGPU;
            Ref<UniformBuffer> TerrainUBO;
            Ref<UniformBuffer> FoliageUBO;
            Ref<UniformBuffer> WaterUBO;
            SceneEffectsGPUState SceneEffectsGPU;
            Ref<UniformBuffer> DecalUBO;
            Ref<UniformBuffer> LightProbeVolumeUBO;
            Ref<StorageBuffer> LightProbeSHBuffer;
            Ref<UniformBuffer> LightmapUBO; // scene lightmap parameters (issue #439)
            // Dirty guard for UploadLightmapData: the caller uploads every
            // frame but the values only change on resolve/toggle, so skip the
            // redundant GL buffer write when the bytes match.
            ShaderBindingLayout::LightmapUBO LastLightmapUBO{};
            bool LightmapUBOUploaded = false;
            // 1x1x1 white RGBA16F placeholder published at TEX_LIGHTMAP when no
            // bake is resolved (issue #868). It must be a Texture2DArray, not
            // the plain WhiteTexture: TEX_LIGHTMAP's sampler became a
            // sampler2DArray when the atlas gained pages, and binding a
            // GL_TEXTURE_2D to a sampler2DArray unit is undefined. Owned here
            // (created in Init, released in Shutdown) rather than as a lazy
            // static — see docs/agent-rules/lazy-static-release-ownership.md.
            Ref<Texture2DArray> LightmapPlaceholderAtlas;

            glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
            // Inverse of the *world* view-projection (issue #429). The depth was
            // written by render-relative geometry (ndc = VP_rel * worldPos_rel),
            // and inverse(VP_world) * ndc == worldPos_ABSOLUTE, so depth-recon
            // consumers (decals, volumetric fog, underwater) get an absolute world
            // position for free — the same value the pre-#429 pipeline produced,
            // so their world-space UBO inputs stay unshifted. A consumer whose
            // *other* inputs are render-relative (deferred lighting: camera /
            // lights / shadow matrices) must instead subtract u_RenderOrigin from
            // the reconstructed position in-shader to match. See
            // docs/agent-rules/camera-relative-rendering.md.
            glm::mat4 InverseViewProjectionMatrix = glm::mat4(1.0f);
            glm::mat4 ViewMatrix = glm::mat4(1.0f);
            glm::mat4 ProjectionMatrix = glm::mat4(1.0f);
            // Camera-relative render origin for this frame (issue #429); see
            // GetRenderOrigin(). (0,0,0) within the first grid cell.
            glm::vec3 RenderOrigin = glm::vec3(0.0f);
            // Debug lever: when false the render origin is pinned to (0,0,0) so
            // the renderer reverts to exact pre-#429 world-space behaviour.
            bool CameraRelativeEnabled = true;

            // Frustum of the CULLING camera — derived from
            // CullViewProjectionMatrix, NOT ViewProjectionMatrix (issue #726).
            Frustum ViewFrustum;
            bool FrustumCullingEnabled = true;
            bool DynamicCullingEnabled = true;
            bool DepthPrepassEnabled = false;
            bool OcclusionCullingEnabled = false;
            bool OcclusionResultsAvailable = false;

            glm::vec3 ViewPos;
            // Direction of the scene's primary (first) directional light. Used
            // by the fog/atmosphere sun-direction derivation; defaults to
            // straight-down when the scene has no directional light.
            glm::vec3 PrimaryDirectionalLightDir = glm::vec3(0.0f, -1.0f, 0.0f);
            f32 CameraNearClip = 0.1f;
            f32 CameraFarClip = 1000.0f;

            // ---- Culling camera (issue #726) ------------------------------
            // Mirrors the render camera above one-for-one. RefreshCullingCamera()
            // copies render -> cull every frame while CullingCameraFrozen is
            // false, so these are the same matrices in the normal case and the
            // frozen ones the moment the observer is switched on. World space,
            // like their render twins; the render-relative copies are derived
            // where they are uploaded.
            bool CullingCameraFrozen = false;
            glm::mat4 CullViewMatrix = glm::mat4(1.0f);
            glm::mat4 CullProjectionMatrix = glm::mat4(1.0f);
            glm::mat4 CullViewProjectionMatrix = glm::mat4(1.0f);
            glm::vec3 CullViewPos = glm::vec3(0.0f);
            f32 CullNearClip = 0.1f;
            f32 CullFarClip = 1000.0f;
            // The culling camera's PREVIOUS-frame view-projection, i.e. the VP
            // the retained Hi-Z pyramid was rendered with. Deliberately NOT
            // PrevViewProjectionMatrix: that one is motion-vector history and
            // must keep tracking the render camera or TAA / motion blur break.
            // Frozen together with the pyramid itself — see
            // GenerateOcclusionHZB(), which stops regenerating while frozen so
            // the depth the occlusion cull tests against stays the depth the
            // frozen camera actually rendered.
            glm::mat4 CullPrevViewProjectionMatrix = glm::mat4(1.0f);
            // Render-origin-relative derivations of the above, recomputed once
            // per frame in PrepareFrame and consumed by the GPU cull dispatches
            // (which read render-relative instance transforms, issue #429).
            // Derived centrally rather than at each dispatch so the three cull
            // shaders cannot disagree about which origin they were shifted by.
            glm::mat4 CullViewProjectionRelative = glm::mat4(1.0f);
            glm::vec3 CullViewPosRelative = glm::vec3(0.0f);
            // x = |CullProjectionMatrix[1][1]| (cot(fovY/2)), y = near-plane
            // distance. The virtual-geometry cull's screen-error math needs both
            // and has no other route to the frozen projection.
            glm::vec2 CullProjParams = glm::vec2(1.0f, 0.1f);

            // Pixel-error mesh-LOD selection inputs (issue #711), derived once per
            // frame in PrepareFrame from the CULLING camera + the render target.
            // Deliberately carries no view matrix: LOD must not move when the
            // camera merely rotates, so only position, FOV and resolution are
            // inputs. Culling camera rather than render camera for the same reason
            // CullViewPos is used for LOD today (issue #726) — a frozen cut must
            // keep the levels it was cut with.
            LODViewParams LODView;

            Statistics Stats;
            u32 CommandCounter = 0;

            // Global resource registry for scene-wide resources like environment maps, shadows, etc.
            ShaderResourceRegistry GlobalResourceRegistry;

            // Per-entity transform history for G-Buffer motion vectors. Prev
            // holds the previous frame's world transform keyed by entityID;
            // Curr accumulates this frame's transforms and becomes Prev at the
            // next BeginScene. Only populated / consulted when Deferred is
            // active — Forward / Forward+ leave these empty.
            std::unordered_map<i32, glm::mat4> PrevEntityTransforms;
            std::unordered_map<i32, glm::mat4> CurrEntityTransforms;

            // Per-mesh per-instance previous-frame transform cache for DrawMeshInstanced.
            // Keyed by mesh AssetHandle; the std::vector preserves per-instance order
            // frame-to-frame — callers are expected to submit stable ordering, which
            // matches how foliage / particle emitters allocate their instance streams.
            // Only touched from Deferred submission (stays empty in Forward / Forward+).
            std::unordered_map<u64, std::vector<glm::mat4>> PrevInstanceTransforms;
            std::unordered_map<u64, std::vector<glm::mat4>> CurrInstanceTransforms;

            Ref<RenderGraph> RGraph;
            std::unique_ptr<RenderPipeline> Pipeline;

            // Auxiliary mesh-caster sink (issue #705) — see SetAuxCasterSink.
            // Non-owning; null except while a bake path is collecting.
            std::vector<DDGIMeshCaster>* AuxCasterSink = nullptr;

            // True once Init() has allocated the renderer's one-shot singletons
            // (FrameDataBufferManager, FrameResourceManager, command dispatch, …).
            // Distinct from IsInitialized(): Init() can legitimately run with a
            // 0x0 framebuffer (window not yet sized on the editor's OnAttach path),
            // in which case SetupRenderGraph early-outs and the Scene pass — and
            // therefore IsInitialized() — stays false until the first real
            // OnWindowResize completes the deferred graph build. This flag guards
            // Init() against being re-entered in that window, which would otherwise
            // re-run FrameDataBufferManager::Init() and trip its "already
            // initialized" assert. Reset in Shutdown().
            bool CoreInitialized = false;

            // Shadow mapping
            ShadowMap Shadow;
            Ref<Shader> ShadowDepthShader;
            Ref<Shader> ShadowDepthSkinnedShader;

            // Depth-prepass depth-only programs (see GetDepthPrepassShaderIDs)
            Ref<Shader> DepthPrepassShader;
            Ref<Shader> DepthPrepassSkinnedShader;
            Ref<Shader> DepthPrepassMaskShader;
            Ref<Shader> DepthPrepassMaskSkinnedShader;

            // Terrain
            Ref<Shader> TerrainPBRShader;
            Ref<Shader> TerrainGBufferShader; // Deferred: Terrain_GBuffer.glsl
            Ref<Shader> TerrainDepthShader;
            Ref<Shader> VoxelPBRShader;
            Ref<Shader> VoxelGBufferShader; // Deferred: Terrain_Voxel_GBuffer.glsl
            Ref<Shader> VoxelDepthShader;
            // Packed-quad greedy meshing siblings (issue #727) — same three
            // roles, instanced unit-quad vertex stage.
            Ref<Shader> VoxelGreedyPBRShader;
            Ref<Shader> VoxelGreedyGBufferShader;
            Ref<Shader> VoxelGreedyDepthShader;
            Ref<Shader> FoliageShader;
            Ref<Shader> FoliageGBufferShader; // Deferred: Foliage_Instance_GBuffer.glsl
            Ref<Shader> FoliageDepthShader;
            Ref<Shader> FoliageImpostorShader; // Octahedral impostor card (issue #433)

            // Water
            Ref<Shader> WaterShader;
            // Depth-only variant for the surface-depth capture: shared
            // VS/TCS/TES displacement chain, no color outputs (#691).
            Ref<Shader> WaterDepthShader;

            // Decals
            Ref<Shader> DecalShader;
            Ref<Shader> DecalGBufferShader;
            Ref<Shader> DecalGBufferNormalShader;
            Ref<Shader> DecalGBufferRMAShader;
            Ref<Shader> DecalGBufferEmissiveShader;
            Ref<Mesh> DecalCubeMesh;
            Ref<Texture2D> WhiteTexture; // 1x1 fallback for untextured decals

            // Volumetric cloudscape / atmosphere per-frame state (issue #633).
            // Cloudscape: scene-published snapshot (see SetCloudscapeState).
            // CloudWindOffset/CloudTime: wind-advection accumulators shared by
            // the raymarch pass and the cloud-shadow compute so both sample
            // the field at the same scroll position (advanced once per frame
            // in RenderPipeline::UploadExecutionState from the wind settings).
            CloudscapeRenderState Cloudscape;
            glm::vec2 CloudWindOffset{ 0.0f, 0.0f };
            f32 CloudTime = 0.0f;
            u32 CloudFrameIndex = 0;
            // Previous Time::GetTime() sample for the cloud dt (mockable via
            // Time::SetMockTime, unlike a steady_clock time point — the
            // determinism hook the visual-evidence goldens rely on).
            f32 CloudPrevTimeSeconds = 0.0f;

            // Post-processing
            PostProcessSettings PostProcess;
            SnowSettings Snow;
            FogSettings Fog;
            u32 FogFrameIndex = 0;
            std::chrono::steady_clock::time_point FogLastTime{};
            f32 FogTime = 0.0f;
            UnderwaterFogState UnderwaterFog{};
            Ref<UniformBuffer> UnderwaterFogBuffer;
            WindSettings Wind;
            // Boat / actor wake foam (issue #967). Written each frame by
            // Scene::ProcessScene3DSharedLogic from the dominant enabled
            // WaterComponent, read by RenderPipeline to drive the disturbance
            // compute. Reset to defaults (disabled) by BeginScene when the
            // scene stops publishing, so a scene with no water cannot inherit
            // the previous one's wake settings.
            WaterDisturbanceSettings WaterDisturbance;
            // Boat / actor wake SHAPE settings (issue #968), published the same
            // way and reset to the disabled default by BeginScene for the same
            // reason: a scene with no water must actively clear the wake rather
            // than inherit the previous scene's.
            WaterWakeSettings WaterWakeShape;
            // Previous Time::GetTime() sample for the wake field's dt — the same
            // mockable-clock trick CloudPrevTimeSeconds uses, so
            // Time::SetMockTime freezes the decay for deterministic golden
            // captures instead of decaying by however long the last frame took.
            f32 WaterDisturbancePrevTimeSeconds = 0.0f;
            SnowAccumulationSettings SnowAccumulation;
            SnowEjectaSettings SnowEjecta;
            PrecipitationSettings Precipitation;
            glm::mat4 PrevViewProjectionMatrix = glm::mat4(1.0f);

            // TAA projection jitter state (Halton(2,3) sub-pixel sequence).
            // `RenderPipeline::PrepareFrame(...)` rotates CurrJitterUV ->
            // PrevJitterUV and then samples the next Halton pair when
            // PostProcess.TAAEnabled; the
            // jitter offset is baked into `ProjectionMatrix` (and therefore
            // `ViewProjectionMatrix`) so all downstream passes observe the
            // jittered camera consistently. In Forward / Forward+ without
            // TAA, and in any path with TAA disabled, both jitters stay at
            // zero — no behavioural change.
            u32 TAAJitterFrameIndex = 0;
            glm::vec2 CurrJitterUV = glm::vec2(0.0f);
            glm::vec2 PrevJitterUV = glm::vec2(0.0f);

            // FSR2 temporal upscaling (#684).
            //
            // `TemporalUpscaleActive` is the ONE authoritative answer to "is FSR2
            // running this frame", derived once per frame at the top of
            // `RenderPipeline::PrepareFrame(...)` from the settings AND from
            // whether the backend can actually do it. Everything downstream —
            // the jitter branch below, the EASU/FSR2/TAA/Upscaler enable calls,
            // the blackboard resource declaration and the pipeline fingerprint —
            // reads this rather than re-deriving it, because re-deriving it is
            // how one site ends up disagreeing with another and the frame ends up
            // jittered with nothing accumulating it.
            //
            // `TemporalUpscaleJitterPixels` is the sub-pixel offset actually
            // baked into ProjectionMatrix this frame, in RENDER-resolution
            // pixels. FSR2 subtracts exactly this value, so it is passed to the
            // pass rather than recomputed there.
            bool TemporalUpscaleActive = false;
            u32 TemporalUpscalePhaseIndex = 0;
            glm::vec2 TemporalUpscaleJitterPixels = glm::vec2(0.0f);

            // Engine-wide counter for the shared blue-noise sampler (issue
            // #706), advanced once per frame in RenderPipeline regardless of
            // which passes are enabled — see the comment at the increment for
            // why it must NOT be conditional the way the two indices above are.
            // Wrapped to 2^20 so it stays exactly representable in the f32 the
            // SSR / SSGI params blocks carry it in.
            u32 StochasticFrameIndex = 0;

            // Global IBL fallback (from scene's EnvironmentMap)
            RHI::ResourceHandle GlobalIrradianceMapID{};
            RHI::ResourceHandle GlobalPrefilterMapID{};
            RHI::ResourceHandle GlobalBRDFLutMapID{};
            RHI::ResourceHandle GlobalEnvironmentMapID{};
            // Native siblings of the three above that RenderPipeline imports
            // into the render graph. Set from the same SetGlobalIBL call so the
            // two spellings cannot describe different textures.
            f32 GlobalIBLIntensity = 1.0f;

            // Nearest water-surface depth texture for underwater fog (§7.2);
            // published by WaterRenderPass, consumed by ToneMap. 0 = no water.
            RHI::ResourceHandle WaterSurfaceDepthTextureID{};

            // Planar-reflection colour texture published by
            // PlanarReflectionRenderPass, sampled by WaterRenderPass. 0 = none.
            RHI::ResourceHandle PlanarReflectionTextureID{};

            // Per-frame planar-reflection request from Scene.cpp (dominant water
            // surface), forwarded to PlanarReflectionRenderPass at EndScene.
            glm::vec4 PlanarReflectionPlane{ 0.0f, 1.0f, 0.0f, 0.0f };
            bool PlanarReflectionEnabled = false;
            f32 PlanarReflectionIntensity = 1.0f;
            f32 PlanarReflectionDistortion = 0.0f;

            // Parallel submission state
            ParallelSceneContext ParallelContext;
            bool ParallelSubmissionActive = false;

            // Forward+ light culling
            TiledForwardPlus ForwardPlus;

            // Distance-impostor reflection probe arrays (issue #705)
            ReflectionProbeArray ReflectionProbes;

            // Global renderer settings (path selection, culling toggles, etc.)
            RendererSettings Settings;

            // Driver-advertised MSAA sample-count caps, queried once in
            // Renderer3D::Init. Zero until queried. ApplyRendererSettings
            // clamps Settings.Deferred.MSAASampleCount to min(color, depth).
            // Exposed via GetMaxMSAASamples() so the editor panel can disable
            // unsupported combo entries rather than accepting silent clamps.
            u32 MaxMSAASamplesColor = 0;
            u32 MaxMSAASamplesDepth = 0;

            // RenderingPath the current RenderGraph topology was built for.
            // Compared against Settings.Path in ApplyRendererSettings to
            // detect a mode switch and trigger ConfigureRenderGraph.
            // Initialised to a sentinel so the first ConfigureRenderGraph
            // in SetupRenderGraph always runs.
            RenderingPath ActiveGraphPath = static_cast<RenderingPath>(0xFF);
            AOTechnique ActiveGraphAOTechnique = static_cast<AOTechnique>(0xFF); // sentinel: never configured

            // Editor-only features gated behind opt-in flags
            bool EnableSelectionOutline = false;
            std::vector<i32> SelectionOutlineEntityIDs;
            RenderCallback PendingParticleRenderCallback;
            RenderCallback PendingUICompositeRenderCallback;
            std::vector<FluidRenderData> PendingFluidDraws; // #630, drained by ConfigurePassesForFrame
        };

        static Renderer3DData s_Data;
        static ShaderLibrary m_ShaderLibrary;
    };
} // namespace OloEngine
