#pragma once

#include "RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine
{
	class UniformBuffer;

    // Command dispatch functions that take POD command data and execute it
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

		// Initialize all command dispatch functions
		static void Initialize();
        
        // Get the dispatch function for a command type
        static CommandDispatchFn GetDispatchFunction(CommandType type);
		
		static void SetSharedUBOs(
            const Ref<UniformBuffer>& transformUBO,
            const Ref<UniformBuffer>& materialUBO,
            const Ref<UniformBuffer>& textureFlagUBO,
            const Ref<UniformBuffer>& cameraUBO);

		static void SetViewProjectionMatrix(const glm::mat4& viewProjection);

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
        static void DrawIndexed(const void* data, RendererAPI& api);
        static void DrawIndexedInstanced(const void* data, RendererAPI& api);
        static void DrawArrays(const void* data, RendererAPI& api);
        static void DrawLines(const void* data, RendererAPI& api);
        
        // Higher-level commands
        static void DrawMesh(const void* data, RendererAPI& api);
        static void DrawMeshInstanced(const void* data, RendererAPI& api);
        static void DrawQuad(const void* data, RendererAPI& api);       
		
		static Statistics& GetStatistics();

		static void ResetState();

	private:
        static void UpdateTransformUBO(const glm::mat4& modelMatrix);
        static void UpdateMaterialUBO(const glm::vec3& ambient, const glm::vec3& diffuse, 
                                     const glm::vec3& specular, f32 shininess);
        static void UpdateTextureFlag(bool useTextures);
    };
}
