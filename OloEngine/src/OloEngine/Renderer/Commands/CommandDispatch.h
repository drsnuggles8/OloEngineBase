#pragma once

#include "RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include <array>

namespace OloEngine
{
    class UniformBuffer;
    class Light;
    class ShaderResourceRegistry;

    class CommandDispatch
    {
      public:
        struct Statistics
        {
            u32 ShaderBinds = 0;
            u32 TextureBinds = 0;
            u32 DrawCalls = 0;

            void Reset()
            {
                ShaderBinds = 0;
                TextureBinds = 0;
                DrawCalls = 0;
            }
        };

        static void Initialize();
        static void Shutdown();

        static CommandDispatchFn GetDispatchFunction(CommandType type);

        // State tracking for current frame rendering
        static void ResetState();
        static void SetViewProjectionMatrix(const glm::mat4& vp);
        static void SetViewMatrix(const glm::mat4& view);
        static void SetProjectionMatrix(const glm::mat4& projection);
        static void SetSceneLight(const Light& light);
        static void SetViewPosition(const glm::vec3& viewPos);

        // Shadow texture binding — set per-frame from Renderer3D/Scene
        static void SetShadowTextureIDs(u32 csmTextureID, u32 spotTextureID);
        static void SetPointShadowTextureIDs(const std::array<u32, 4>& pointTextureIDs);

        // Getters for current frame state (used for sort key generation)
        static const glm::mat4& GetViewMatrix();

        // UBO access - Renderer3D provides these, CommandDispatch uses them
        static void SetUBOReferences(
            const Ref<UniformBuffer>& cameraUBO,
            const Ref<UniformBuffer>& materialUBO,
            const Ref<UniformBuffer>& lightUBO,
            const Ref<UniformBuffer>& boneMatricesUBO,
            const Ref<UniformBuffer>& modelMatrixUBO);

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

        static Statistics& GetStatistics();

      private:
        static void UpdateMaterialTextureFlag(bool useTextures);
    };
} // namespace OloEngine
