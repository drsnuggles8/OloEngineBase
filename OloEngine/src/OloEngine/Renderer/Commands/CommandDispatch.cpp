#include "OloEnginePCH.h"
#include "CommandDispatch.h"
#include "RenderCommand.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    // Forward declaration of helper function
    static Ref<VertexArray> GetVertexArrayFromID(u32 rendererID);
    
    // Array of dispatch functions indexed by CommandType
    static CommandDispatchFn s_DispatchTable[static_cast<size_t>(CommandType::SetMultisampling) + 1] = { nullptr };
      // Initialize all command dispatch functions
    void CommandDispatch::Initialize()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing CommandDispatch system");
        
        // Clear the dispatch table first to ensure all entries are nullptr
        for (size_t i = 0; i < sizeof(s_DispatchTable) / sizeof(CommandDispatchFn); ++i)
        {
            s_DispatchTable[i] = nullptr;
        }
        
        // State management dispatch functions
        s_DispatchTable[static_cast<size_t>(CommandType::SetViewport)] = CommandDispatch::SetViewport;
        s_DispatchTable[static_cast<size_t>(CommandType::SetClearColor)] = CommandDispatch::SetClearColor;
        s_DispatchTable[static_cast<size_t>(CommandType::Clear)] = CommandDispatch::Clear;
        s_DispatchTable[static_cast<size_t>(CommandType::ClearStencil)] = CommandDispatch::ClearStencil;
        s_DispatchTable[static_cast<size_t>(CommandType::SetBlendState)] = CommandDispatch::SetBlendState;
        s_DispatchTable[static_cast<size_t>(CommandType::SetBlendFunc)] = CommandDispatch::SetBlendFunc;
        s_DispatchTable[static_cast<size_t>(CommandType::SetBlendEquation)] = CommandDispatch::SetBlendEquation;
        s_DispatchTable[static_cast<size_t>(CommandType::SetDepthTest)] = CommandDispatch::SetDepthTest;
        s_DispatchTable[static_cast<size_t>(CommandType::SetDepthMask)] = CommandDispatch::SetDepthMask;
        s_DispatchTable[static_cast<size_t>(CommandType::SetDepthFunc)] = CommandDispatch::SetDepthFunc;
        s_DispatchTable[static_cast<size_t>(CommandType::SetStencilTest)] = CommandDispatch::SetStencilTest;
        s_DispatchTable[static_cast<size_t>(CommandType::SetStencilFunc)] = CommandDispatch::SetStencilFunc;
        s_DispatchTable[static_cast<size_t>(CommandType::SetStencilMask)] = CommandDispatch::SetStencilMask;
        s_DispatchTable[static_cast<size_t>(CommandType::SetStencilOp)] = CommandDispatch::SetStencilOp;
        s_DispatchTable[static_cast<size_t>(CommandType::SetCulling)] = CommandDispatch::SetCulling;
        s_DispatchTable[static_cast<size_t>(CommandType::SetCullFace)] = CommandDispatch::SetCullFace;
        s_DispatchTable[static_cast<size_t>(CommandType::SetLineWidth)] = CommandDispatch::SetLineWidth;
        s_DispatchTable[static_cast<size_t>(CommandType::SetPolygonMode)] = CommandDispatch::SetPolygonMode;
        s_DispatchTable[static_cast<size_t>(CommandType::SetPolygonOffset)] = CommandDispatch::SetPolygonOffset;
        s_DispatchTable[static_cast<size_t>(CommandType::SetScissorTest)] = CommandDispatch::SetScissorTest;
        s_DispatchTable[static_cast<size_t>(CommandType::SetScissorBox)] = CommandDispatch::SetScissorBox;
        s_DispatchTable[static_cast<size_t>(CommandType::SetColorMask)] = CommandDispatch::SetColorMask;
        s_DispatchTable[static_cast<size_t>(CommandType::SetMultisampling)] = CommandDispatch::SetMultisampling;
        
        // Draw commands dispatch functions
        s_DispatchTable[static_cast<size_t>(CommandType::BindDefaultFramebuffer)] = CommandDispatch::BindDefaultFramebuffer;
        s_DispatchTable[static_cast<size_t>(CommandType::BindTexture)] = CommandDispatch::BindTexture;
        s_DispatchTable[static_cast<size_t>(CommandType::DrawIndexed)] = CommandDispatch::DrawIndexed;
        s_DispatchTable[static_cast<size_t>(CommandType::DrawIndexedInstanced)] = CommandDispatch::DrawIndexedInstanced;
        s_DispatchTable[static_cast<size_t>(CommandType::DrawArrays)] = CommandDispatch::DrawArrays;
        s_DispatchTable[static_cast<size_t>(CommandType::DrawLines)] = CommandDispatch::DrawLines;
        
        // Higher-level commands - pay special attention to these as they are causing errors
        OLO_CORE_INFO("Registering DrawMesh at index {}", static_cast<size_t>(CommandType::DrawMesh));
        s_DispatchTable[static_cast<size_t>(CommandType::DrawMesh)] = CommandDispatch::DrawMesh;
        
        OLO_CORE_INFO("Registering DrawMeshInstanced at index {}", static_cast<size_t>(CommandType::DrawMeshInstanced));
        s_DispatchTable[static_cast<size_t>(CommandType::DrawMeshInstanced)] = CommandDispatch::DrawMeshInstanced;
        
        OLO_CORE_INFO("Registering DrawQuad at index {}", static_cast<size_t>(CommandType::DrawQuad));
        s_DispatchTable[static_cast<size_t>(CommandType::DrawQuad)] = CommandDispatch::DrawQuad;
        
        // Verify that the problematic entries have been set properly
        if (s_DispatchTable[static_cast<size_t>(CommandType::DrawMesh)] == nullptr) {
            OLO_CORE_ERROR("Failed to register DrawMesh dispatch function!");
        }
        
        if (s_DispatchTable[static_cast<size_t>(CommandType::DrawQuad)] == nullptr) {
            OLO_CORE_ERROR("Failed to register DrawQuad dispatch function!");
        }
        
        // Log the full dispatch table for debugging
        for (size_t i = 0; i < sizeof(s_DispatchTable) / sizeof(CommandDispatchFn); ++i) {
            OLO_CORE_TRACE("Dispatch table[{}] = {}", i, s_DispatchTable[i] ? "Set" : "nullptr");
        }
        
        OLO_CORE_INFO("CommandDispatch system initialized with {} dispatch functions", static_cast<size_t>(CommandType::SetMultisampling) + 1);
    }
    
    // Get the dispatch function for a command type
    CommandDispatchFn CommandDispatch::GetDispatchFunction(CommandType type)
    {
        if (type == CommandType::Invalid || static_cast<size_t>(type) >= sizeof(s_DispatchTable) / sizeof(CommandDispatchFn))
        {
            OLO_CORE_ERROR("CommandDispatch::GetDispatchFunction: Invalid command type {}", static_cast<int>(type));
            return nullptr;
        }
        
        return s_DispatchTable[static_cast<size_t>(type)];
    }
    
    // State management dispatch functions
    void CommandDispatch::SetViewport(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetViewportCommand*>(data);
        api.SetViewport(cmd->x, cmd->y, cmd->width, cmd->height);
    }
    
    void CommandDispatch::SetClearColor(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetClearColorCommand*>(data);
        api.SetClearColor(cmd->color);
    }
    
    void CommandDispatch::Clear(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const ClearCommand*>(data);
        // We only have a Clear() method which clears both color and depth
        // In a full implementation, you'd have separate methods for partial clears
        if (cmd->clearColor || cmd->clearDepth)
            api.Clear();
    }
    
    void CommandDispatch::ClearStencil(const void* data, RendererAPI& api)
    {
        api.ClearStencil();
    }
    
    void CommandDispatch::SetBlendState(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetBlendStateCommand*>(data);
        api.SetBlendState(cmd->enabled);
    }
    
    void CommandDispatch::SetBlendFunc(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetBlendFuncCommand*>(data);
        api.SetBlendFunc(cmd->sourceFactor, cmd->destFactor);
    }
    
    void CommandDispatch::SetBlendEquation(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetBlendEquationCommand*>(data);
        api.SetBlendEquation(cmd->mode);
    }
    
    void CommandDispatch::SetDepthTest(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetDepthTestCommand*>(data);
        api.SetDepthTest(cmd->enabled);
    }
    
    void CommandDispatch::SetDepthMask(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetDepthMaskCommand*>(data);
        api.SetDepthMask(cmd->writeMask);
    }
    
    void CommandDispatch::SetDepthFunc(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetDepthFuncCommand*>(data);
        api.SetDepthFunc(cmd->function);
    }
    
    void CommandDispatch::SetStencilTest(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilTestCommand*>(data);
        if (cmd->enabled)
            api.EnableStencilTest();
        else
            api.DisableStencilTest();
    }
    
    void CommandDispatch::SetStencilFunc(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilFuncCommand*>(data);
        api.SetStencilFunc(cmd->function, cmd->reference, cmd->mask);
    }
    
    void CommandDispatch::SetStencilMask(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilMaskCommand*>(data);
        api.SetStencilMask(cmd->mask);
    }
    
    void CommandDispatch::SetStencilOp(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetStencilOpCommand*>(data);
        api.SetStencilOp(cmd->stencilFail, cmd->depthFail, cmd->depthPass);
    }
    
    void CommandDispatch::SetCulling(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetCullingCommand*>(data);
        if (cmd->enabled)
            api.EnableCulling();
        else
            api.DisableCulling();
    }
    
    void CommandDispatch::SetCullFace(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetCullFaceCommand*>(data);
        api.SetCullFace(cmd->face);
    }
    
    void CommandDispatch::SetLineWidth(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetLineWidthCommand*>(data);
        api.SetLineWidth(cmd->width);
    }
    
    void CommandDispatch::SetPolygonMode(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetPolygonModeCommand*>(data);
        api.SetPolygonMode(cmd->face, cmd->mode);
    }
    
    void CommandDispatch::SetPolygonOffset(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetPolygonOffsetCommand*>(data);
        if (cmd->enabled)
            api.SetPolygonOffset(cmd->factor, cmd->units);
        else
            api.SetPolygonOffset(0.0f, 0.0f);
    }
    
    void CommandDispatch::SetScissorTest(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetScissorTestCommand*>(data);
        if (cmd->enabled)
            api.EnableScissorTest();
        else
            api.DisableScissorTest();
    }
    
    void CommandDispatch::SetScissorBox(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetScissorBoxCommand*>(data);
        api.SetScissorBox(cmd->x, cmd->y, cmd->width, cmd->height);
    }
    
    void CommandDispatch::SetColorMask(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetColorMaskCommand*>(data);
        api.SetColorMask(cmd->red, cmd->green, cmd->blue, cmd->alpha);
    }
    
    void CommandDispatch::SetMultisampling(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const SetMultisamplingCommand*>(data);
        if (cmd->enabled)
            api.EnableMultisampling();
        else
            api.DisableMultisampling();
    }
    
    // Draw commands dispatch functions
    void CommandDispatch::BindDefaultFramebuffer(const void* data, RendererAPI& api)
    {
        api.BindDefaultFramebuffer();
    }
    
    void CommandDispatch::BindTexture(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const BindTextureCommand*>(data);
        api.BindTexture(cmd->slot, cmd->textureID);
    }
    
    void CommandDispatch::DrawIndexed(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawIndexedCommand*>(data);
        
        // In a real implementation, we would get the VertexArray from a resource manager
        // For now, let's use a helper function to get/create the vertex array
        auto vertexArray = GetVertexArrayFromID(cmd->rendererID);
        if (vertexArray)
        {
            api.DrawIndexed(vertexArray, cmd->indexCount);
        }
        else
        {
            OLO_CORE_ERROR("CommandDispatch::DrawIndexed: Invalid vertex array ID: {}", cmd->rendererID);
        }
    }
    
    void CommandDispatch::DrawIndexedInstanced(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawIndexedInstancedCommand*>(data);
        
        // In a real implementation, we would get the VertexArray from a resource manager
        auto vertexArray = GetVertexArrayFromID(cmd->rendererID);
        if (vertexArray)
        {
            api.DrawIndexedInstanced(vertexArray, cmd->indexCount, cmd->instanceCount);
        }
        else
        {
            OLO_CORE_ERROR("CommandDispatch::DrawIndexedInstanced: Invalid vertex array ID: {}", cmd->rendererID);
        }
    }
    
    void CommandDispatch::DrawArrays(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawArraysCommand*>(data);
        
        // In a real implementation, we would get the VertexArray from a resource manager
        auto vertexArray = GetVertexArrayFromID(cmd->rendererID);
        if (vertexArray)
        {
            api.DrawArrays(vertexArray, cmd->vertexCount);
        }
        else
        {
            OLO_CORE_ERROR("CommandDispatch::DrawArrays: Invalid vertex array ID: {}", cmd->rendererID);
        }
    }
    
    void CommandDispatch::DrawLines(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawLinesCommand*>(data);
        
        // In a real implementation, we would get the VertexArray from a resource manager
        auto vertexArray = GetVertexArrayFromID(cmd->rendererID);
        if (vertexArray)
        {
            api.DrawLines(vertexArray, cmd->vertexCount);
        }
        else
        {
            OLO_CORE_ERROR("CommandDispatch::DrawLines: Invalid vertex array ID: {}", cmd->rendererID);
        }
    }
    
    // Higher-level commands
    void CommandDispatch::DrawMesh(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawMeshCommand*>(data);
        
        // Get the vertex array and shader from the resource manager
        auto vertexArray = GetVertexArrayFromID(cmd->vaoID);
        if (!vertexArray)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: Invalid vertex array ID: {}", cmd->vaoID);
            return;
        }
        
        // Bind material textures if needed
        if (cmd->useTextureMaps)
        {
            if (cmd->diffuseMapID > 0)
            {
                api.BindTexture(0, cmd->diffuseMapID);
            }
            
            if (cmd->specularMapID > 0)
            {
                api.BindTexture(1, cmd->specularMapID);
            }
        }
        
        // Draw the mesh using the index buffer
        api.DrawIndexed(vertexArray, cmd->indexCount);
    }
    
    void CommandDispatch::DrawMeshInstanced(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawMeshInstancedCommand*>(data);
        
        // Get the vertex array and shader from the resource manager
        auto vertexArray = GetVertexArrayFromID(cmd->vaoID);
        if (!vertexArray)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: Invalid vertex array ID: {}", cmd->vaoID);
            return;
        }
        
        // Bind material textures if needed
        if (cmd->useTextureMaps)
        {
            if (cmd->diffuseMapID > 0)
            {
                api.BindTexture(0, cmd->diffuseMapID);
            }
            
            if (cmd->specularMapID > 0)
            {
                api.BindTexture(1, cmd->specularMapID);
            }
        }
        
        // In a real implementation, we'd need to provide the instance transforms to the shader
        // For now we'll just assume they're already set up
        
        // Draw the mesh using instancing
        api.DrawIndexedInstanced(vertexArray, cmd->indexCount, cmd->instanceCount);
    }
    
    void CommandDispatch::DrawQuad(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const DrawQuadCommand*>(data);
        
        // Bind texture
        if (cmd->textureID > 0)
        {
            api.BindTexture(0, cmd->textureID);
        }
        
        // In a real implementation, we would have built-in quad rendering
        // For now, we'll just simulate it with a warning
        static bool warnedOnce = false;
        if (!warnedOnce)
        {
            OLO_CORE_WARN("CommandDispatch::DrawQuad: Direct quad drawing not implemented in RendererAPI");
            OLO_CORE_WARN("                         You need to create a quad mesh and use DrawMesh instead");
            warnedOnce = true;
        }
    }
    
    // Helper function to get a VertexArray from its ID
    Ref<VertexArray> GetVertexArrayFromID(u32 rendererID)
    {
        // In a real implementation, this would look up the VertexArray in a resource manager
        // For now, we'll create a dummy VertexArray
        static std::unordered_map<u32, Ref<VertexArray>> s_VertexArrayCache;
        
        if (s_VertexArrayCache.find(rendererID) != s_VertexArrayCache.end())
        {
            return s_VertexArrayCache[rendererID];
        }
        else if (rendererID > 0)
        {
            // Create a new one - in a real implementation this would be fetched from a registry
            auto vertexArray = VertexArray::Create();
            s_VertexArrayCache[rendererID] = vertexArray;
            OLO_CORE_INFO("Created proxy VertexArray for ID: {}", rendererID);
            return vertexArray;
        }
        
        return nullptr;
    }
}