#pragma once

#include "RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include <array>

namespace OloEngine
{
    class TiledForwardPlus;
    class UniformBuffer;
    class InstanceBuffer;
    class ShaderResourceRegistry;

    class CommandDispatch
    {
      public:
        struct Statistics
        {
            u32 ShaderBinds = 0;
            u32 TextureBinds = 0;
            u32 DrawCalls = 0;
            u32 ConditionalDraws = 0; // Draws issued inside conditional render blocks

            void Reset()
            {
                ShaderBinds = 0;
                TextureBinds = 0;
                DrawCalls = 0;
                ConditionalDraws = 0;
            }
        };

        static void Initialize();
        static void Shutdown();

        static CommandDispatchFn GetDispatchFunction(CommandType type);

        // State tracking for current frame rendering
        static void ResetState();
        static void InvalidateRenderStateCache();
        static void InvalidateUBOCache(u32 bindingPoint);

        // Clear any cached texture-slot binding that points at this OpenGL texture ID.
        // Must be called when a Texture2D is destroyed so that a future call to
        // BindTrackedTexture with a recycled GL ID is not incorrectly skipped.
        static void InvalidateTextureBinding(u32 textureID);
        static void SetDepthPrepassActive(bool active);
        static void SetDepthPrepassColorPassActive(bool active);
        // Overdraw debug view (#519): when active, ApplyPODRenderState forces
        // additive (GL_ONE, GL_ONE) blending with depth testing off and the
        // colour mask on, and batchable opaque draws are swapped for the
        // depth-only DepthPrepass* programs (fragment emits 1.0) so each covered
        // fragment adds 1 to the accumulation target. OverdrawRenderPass sets this
        // around a replay of the scene command bucket. Mutually exclusive with the
        // depth-prepass modes.
        static void SetOverdrawActive(bool active);
        // Water surface-depth capture: when active, ApplyPODRenderState forces
        // depth-only state (color writes off, depth writes on, GL_LESS, no blend)
        // even for the blended water draw, so the nearest wavy water surface is
        // written into a dedicated depth target for the underwater fog. The depth
        // prepass deliberately no-ops blended geometry, so this needs its own flag.
        static void SetWaterDepthCaptureActive(bool active);
        static void SetViewProjectionMatrix(const glm::mat4& vp);
        static void SetViewMatrix(const glm::mat4& view);
        static void SetProjectionMatrix(const glm::mat4& projection);
        // @brief Mirror the previous-frame view-projection from Renderer3D
        // so dispatch paths that drive the shared CameraUBO themselves
        // (terrain / voxel / decal) can fill CameraUBO::PrevViewProjection
        // without aliasing the current-frame VP.
        static void SetPrevViewProjectionMatrix(const glm::mat4& prevVP);
        static void SetViewPosition(const glm::vec3& viewPos);
        // @brief Camera-relative render origin for this frame (issue #429). The
        // stored view / view-projection / position above remain *world*-space
        // (sort keys and the planar-reflection mirror need them); the camera-UBO
        // packing here derives the relative matrices from those plus this origin
        // so the shared, terrain, voxel and mirror uploads all agree.
        static void SetRenderOrigin(const glm::vec3& origin);
        static const glm::vec3& GetRenderOrigin();

        // Push the current CommandDispatch camera matrices (the Set* values above)
        // into the shared CameraUBO buffer and (re)bind it at UBO_CAMERA — the
        // same upload the terrain / voxel dispatch paths do inline. Exposed so a
        // pass that re-renders the opaque scene from an alternate camera (the
        // planar reflection) can swap the camera, replay the bucket, and restore.
        static void UploadCameraUBO();

        // Shadow texture binding — set per-frame from Renderer3D/Scene.
        // The *Raw ids are the comparison-OFF views of the CSM / spot arrays used
        // by the PCSS blocker search (0 = none; bound only when non-zero).
        static void SetShadowTextureIDs(u32 csmTextureID, u32 spotTextureID,
                                        u32 csmRawTextureID = 0, u32 spotRawTextureID = 0);
        static void SetPointShadowTextureIDs(const std::array<u32, UBOStructures::ShadowUBO::MAX_POINT_SHADOWS>& pointTextureIDs);

        // Snow accumulation depth texture — set per-frame
        static void SetSnowDepthTextureID(u32 textureID);

        // Getters for current frame state (used for sort key generation and per-bucket view state)
        static const glm::mat4& GetViewMatrix();
        static const glm::mat4& GetProjectionMatrix();
        static const glm::mat4& GetViewProjectionMatrix();
        static const glm::vec3& GetViewPosition();

        // Shared scene/runtime bindings supplied by the renderer frontend at init.
        static void SetUBOReferences(
            const Ref<UniformBuffer>& cameraUBO,
            const Ref<UniformBuffer>& materialUBO,
            const Ref<UniformBuffer>& boneMatricesUBO,
            const Ref<InstanceBuffer>& modelInstanceBuffer,
            const Ref<UniformBuffer>& prevBoneMatricesUBO = nullptr,
            TiledForwardPlus* forwardPlus = nullptr);

        // Rebind the shared scene camera UBO after earlier passes reused that
        // binding point, and ensure the Forward+ UBO baseline remains valid
        // even when tiled Forward+ lighting is inactive this frame.
        static void BindSceneResources();

        // State management dispatch functions
        static void SetViewport(const void* data, RendererAPI& api);
        static void SetClearColor(const void* data, RendererAPI& api);
        static void Clear(const void* data, RendererAPI& api);
        static void ClearStencil(const void* data, RendererAPI& api);
        static void SetBlendState(const void* data, RendererAPI& api);
        static void SetBlendFunc(const void* data, RendererAPI& api);
        static void SetBlendEquation(const void* data, RendererAPI& api);
        static void SetDepthTest(const void* data, RendererAPI& api);
        static void SetDepthMask(const void* data, RendererAPI& api);
        static void SetDepthFunc(const void* data, RendererAPI& api);
        static void SetStencilTest(const void* data, RendererAPI& api);
        static void SetStencilFunc(const void* data, RendererAPI& api);
        static void SetStencilMask(const void* data, RendererAPI& api);
        static void SetStencilOp(const void* data, RendererAPI& api);
        static void SetCulling(const void* data, RendererAPI& api);
        static void SetCullFace(const void* data, RendererAPI& api);
        static void SetLineWidth(const void* data, RendererAPI& api);
        static void SetPolygonMode(const void* data, RendererAPI& api);
        static void SetPolygonOffset(const void* data, RendererAPI& api);
        static void SetScissorTest(const void* data, RendererAPI& api);
        static void SetScissorBox(const void* data, RendererAPI& api);
        static void SetColorMask(const void* data, RendererAPI& api);
        static void SetMultisampling(const void* data, RendererAPI& api);

        // Draw commands dispatch functions
        static void BindDefaultFramebuffer(const void* data, RendererAPI& api);
        static void BindTexture(const void* data, RendererAPI& api);
        static void SetShaderResource(const void* data, RendererAPI& api);
        static void DrawIndexed(const void* data, RendererAPI& api);
        static void DrawIndexedInstanced(const void* data, RendererAPI& api);
        static void DrawArrays(const void* data, RendererAPI& api);
        static void DrawLines(const void* data, RendererAPI& api);
        static void DrawMesh(const void* data, RendererAPI& api);
        static void DrawMeshInstanced(const void* data, RendererAPI& api);
        static void DrawSkybox(const void* data, RendererAPI& api);
        static void DrawInfiniteGrid(const void* data, RendererAPI& api);
        static void DrawQuad(const void* data, RendererAPI& api);

        // Terrain/Voxel dispatch functions
        static void DrawTerrainPatch(const void* data, RendererAPI& api);
        static void DrawVoxelMesh(const void* data, RendererAPI& api);

        // Decal dispatch function
        static void DrawDecal(const void* data, RendererAPI& api);

        // Foliage dispatch function
        static void DrawFoliageLayer(const void* data, RendererAPI& api);

        // Water dispatch function
        static void DrawWater(const void* data, RendererAPI& api);

        static Statistics& GetStatistics();

      private:
        static void UpdateMaterialTextureFlag(bool useTextures);
    };
} // namespace OloEngine
