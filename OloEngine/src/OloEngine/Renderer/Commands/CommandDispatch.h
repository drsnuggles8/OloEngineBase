#pragma once

#include "RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine
{
    // Command dispatch functions that take POD command data and execute it
    namespace CommandDispatch
    {
        // State management dispatch functions
        void SetViewport(const void* data, RendererAPI& api);
        void SetClearColor(const void* data, RendererAPI& api);
        void Clear(const void* data, RendererAPI& api);
        void ClearStencil(const void* data, RendererAPI& api);
        void SetBlendState(const void* data, RendererAPI& api);
        void SetBlendFunc(const void* data, RendererAPI& api);
        void SetBlendEquation(const void* data, RendererAPI& api);
        void SetDepthTest(const void* data, RendererAPI& api);
        void SetDepthMask(const void* data, RendererAPI& api);
        void SetDepthFunc(const void* data, RendererAPI& api);
        void SetStencilTest(const void* data, RendererAPI& api);
        void SetStencilFunc(const void* data, RendererAPI& api);
        void SetStencilMask(const void* data, RendererAPI& api);
        void SetStencilOp(const void* data, RendererAPI& api);
        void SetCulling(const void* data, RendererAPI& api);
        void SetCullFace(const void* data, RendererAPI& api);
        void SetLineWidth(const void* data, RendererAPI& api);
        void SetPolygonMode(const void* data, RendererAPI& api);
        void SetPolygonOffset(const void* data, RendererAPI& api);
        void SetScissorTest(const void* data, RendererAPI& api);
        void SetScissorBox(const void* data, RendererAPI& api);
        void SetColorMask(const void* data, RendererAPI& api);
        void SetMultisampling(const void* data, RendererAPI& api);

        // Draw commands dispatch functions
        void BindDefaultFramebuffer(const void* data, RendererAPI& api);
        void BindTexture(const void* data, RendererAPI& api);
        void DrawIndexed(const void* data, RendererAPI& api);
        void DrawIndexedInstanced(const void* data, RendererAPI& api);
        void DrawArrays(const void* data, RendererAPI& api);
        void DrawLines(const void* data, RendererAPI& api);
        
        // Higher-level commands
        void DrawMesh(const void* data, RendererAPI& api);
        void DrawMeshInstanced(const void* data, RendererAPI& api);
        void DrawQuad(const void* data, RendererAPI& api);
        
        // Initialize all command dispatch functions
        void Initialize();
        
        // Get the dispatch function for a command type
        CommandDispatchFn GetDispatchFunction(CommandType type);
    }
}