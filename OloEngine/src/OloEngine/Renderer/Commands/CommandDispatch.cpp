#include "OloEnginePCH.h"
#include "CommandDispatch.h"
#include "RenderCommand.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/UniformBuffer.h" 

namespace OloEngine
{
	struct CommandDispatchData
	{
		// References to UBOs owned by StatelessRenderer3D
		Ref<UniformBuffer> TransformUBO = nullptr;
		Ref<UniformBuffer> MaterialUBO = nullptr;
		Ref<UniformBuffer> TextureFlagUBO = nullptr;
		Ref<UniformBuffer> CameraUBO = nullptr;
		
		// Cached matrices for transforms
		glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
		
		// State tracking for optimizations
		u32 CurrentBoundShaderID = 0;
		std::array<u32, 32> BoundTextureIDs = { 0 };
		
		// Statistics for performance monitoring
		CommandDispatch::Statistics Stats;
	};

	static CommandDispatchData s_Data;

	void CommandDispatch::SetSharedUBOs(
        const Ref<UniformBuffer>& transformUBO,
        const Ref<UniformBuffer>& materialUBO,
        const Ref<UniformBuffer>& textureFlagUBO,
        const Ref<UniformBuffer>& cameraUBO)
    {
        s_Data.TransformUBO = transformUBO;
        s_Data.MaterialUBO = materialUBO;
        s_Data.TextureFlagUBO = textureFlagUBO;
        s_Data.CameraUBO = cameraUBO;
        
        OLO_CORE_INFO("CommandDispatch: Shared UBOs configured");
    }

	void CommandDispatch::SetViewProjectionMatrix(const glm::mat4& viewProjection)
    {
        s_Data.ViewProjectionMatrix = viewProjection;
    }

	CommandDispatch::Statistics& CommandDispatch::GetStatistics()
    {
        return s_Data.Stats;
    }

	// UBO update methods that use the shared UBOs
    void CommandDispatch::UpdateTransformUBO(const glm::mat4& modelMatrix)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Data.TransformUBO)
        {
            OLO_CORE_WARN("CommandDispatch::UpdateTransformUBO: TransformUBO not initialized");
            return;
        }
        
        // Using the engine's expected layout for the transform UBO
        struct alignas(16) TransformData
        {
            glm::mat4 Model;
            glm::mat4 ViewProjection;
        };
        
        TransformData data{
            modelMatrix,
            s_Data.ViewProjectionMatrix
        };
        
        s_Data.TransformUBO->SetData(&data, sizeof(TransformData));
    }
    
    void CommandDispatch::UpdateMaterialUBO(const glm::vec3& ambient, const glm::vec3& diffuse, 
                                           const glm::vec3& specular, f32 shininess)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Data.MaterialUBO)
        {
            OLO_CORE_WARN("CommandDispatch::UpdateMaterialUBO: MaterialUBO not initialized");
            return;
        }
        
        struct alignas(16) MaterialData
        {
            glm::vec4 Ambient;    // vec3 aligned to vec4
            glm::vec4 Diffuse;    // vec3 aligned to vec4
            glm::vec4 Specular;   // vec3 aligned to vec4
            float Shininess;
            float _pad[3];        // Padding for alignment
        };
        
        MaterialData data{
            glm::vec4(ambient, 1.0f),
            glm::vec4(diffuse, 1.0f),
            glm::vec4(specular, 1.0f),
            shininess,
            {0.0f, 0.0f, 0.0f}
        };
        
        s_Data.MaterialUBO->SetData(&data, sizeof(MaterialData));
    }
    
    void CommandDispatch::UpdateTextureFlag(bool useTextures)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Data.TextureFlagUBO)
        {
            OLO_CORE_WARN("CommandDispatch::UpdateTextureFlag: TextureFlagUBO not initialized");
            return;
        }
        
        int flag = useTextures ? 1 : 0;
        s_Data.TextureFlagUBO->SetData(&flag, sizeof(int));
    }

    // Array of dispatch functions indexed by CommandType
    static CommandDispatchFn s_DispatchTable[static_cast<size_t>(CommandType::SetMultisampling) + 1] = { nullptr };
      // Initialize all command dispatch functions
    void CommandDispatch::Initialize()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing CommandDispatch system");

		// Initialize the state tracking and statistics
		s_Data.CurrentBoundShaderID = 0;
		std::fill(s_Data.BoundTextureIDs.begin(), s_Data.BoundTextureIDs.end(), 0);
		s_Data.Stats.Reset();
        
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
		
		if (!cmd->vertexArray)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawIndexed: Invalid vertex array");
			return;
		}
		
		api.DrawIndexed(cmd->vertexArray, cmd->indexCount);
	}

	void CommandDispatch::DrawIndexedInstanced(const void* data, RendererAPI& api)
	{
		auto const* cmd = static_cast<const DrawIndexedInstancedCommand*>(data);
		
		if (!cmd->vertexArray)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawIndexedInstanced: Invalid vertex array");
			return;
		}
		
		api.DrawIndexedInstanced(cmd->vertexArray, cmd->indexCount, cmd->instanceCount);
	}

	void CommandDispatch::DrawArrays(const void* data, RendererAPI& api)
	{
		auto const* cmd = static_cast<const DrawArraysCommand*>(data);
		
		if (!cmd->vertexArray)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawArrays: Invalid vertex array");
			return;
		}
		
		api.DrawArrays(cmd->vertexArray, cmd->vertexCount);
	}

	void CommandDispatch::DrawLines(const void* data, RendererAPI& api)
	{
		auto const* cmd = static_cast<const DrawLinesCommand*>(data);
		
		if (!cmd->vertexArray)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawLines: Invalid vertex array");
			return;
		}
		
		api.DrawLines(cmd->vertexArray, cmd->vertexCount);
	}
    
    // Higher-level commands
    void CommandDispatch::DrawMesh(const void* data, RendererAPI& api)
	{
		OLO_PROFILE_FUNCTION();
		
		auto const* cmd = static_cast<const DrawMeshCommand*>(data);
		
		if (!cmd->vertexArray || !cmd->shader)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawMesh: Invalid vertex array or shader");
			return;
		}
		
		// Optimize shader binding - only bind if different from currently bound
		u32 shaderID = cmd->shader->GetRendererID();
		if (s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		
		// Update UBOs with transform and material data
		UpdateTransformUBO(cmd->transform);
		UpdateMaterialUBO(cmd->ambient, cmd->diffuse, cmd->specular, cmd->shininess);
		UpdateTextureFlag(cmd->useTextureMaps);
		
		// Efficiently bind textures if needed
		if (cmd->useTextureMaps)
		{
			if (cmd->diffuseMap)
			{
				u32 texID = cmd->diffuseMap->GetRendererID();
				if (s_Data.BoundTextureIDs[0] != texID)
				{
					cmd->diffuseMap->Bind(0);
					s_Data.BoundTextureIDs[0] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->specularMap)
			{
				u32 texID = cmd->specularMap->GetRendererID();
				if (s_Data.BoundTextureIDs[1] != texID)
				{
					cmd->specularMap->Bind(1);
					s_Data.BoundTextureIDs[1] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
		}
		
		// Get index count efficiently
		u32 indexCount = cmd->indexCount > 0 ? cmd->indexCount : 
			cmd->vertexArray->GetIndexBuffer() ? cmd->vertexArray->GetIndexBuffer()->GetCount() : 0;
		
		if (indexCount == 0)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawMesh: No indices to draw");
			return;
		}
		
		// Track draw calls for statistics
		s_Data.Stats.DrawCalls++;
		
		// Issue the actual draw call
		api.DrawIndexed(cmd->vertexArray, indexCount);
	}
    
    void CommandDispatch::DrawMeshInstanced(const void* data, RendererAPI& api)
	{
		OLO_PROFILE_FUNCTION();
		
		auto const* cmd = static_cast<const DrawMeshInstancedCommand*>(data);
		
		if (!cmd->vertexArray || !cmd->shader)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: Invalid vertex array or shader");
			return;
		}
		
		// Optimize shader binding - only bind if different from currently bound
		u32 shaderID = cmd->shader->GetRendererID();
		if (s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		
		// Update material UBOs
		UpdateMaterialUBO(cmd->ambient, cmd->diffuse, cmd->specular, cmd->shininess);
		UpdateTextureFlag(cmd->useTextureMaps);
		
		// For instanced rendering, we'll set model matrices as shader uniforms
		// A proper implementation would use an instance buffer or SSBO for better performance
		// This is a simplified approach for now
		const size_t maxInstances = 100; // Limit for uniform-based instancing
		if (cmd->transforms.size() > maxInstances)
		{
			OLO_CORE_WARN("CommandDispatch::DrawMeshInstanced: Too many instances ({}). Only first {} will be rendered.",
						cmd->transforms.size(), maxInstances);
		}
		
		// Set instance transforms (limited approach - a better solution would use SSBOs or instance buffers)
		size_t instanceCount = std::min(cmd->transforms.size(), maxInstances);
		for (size_t i = 0; i < instanceCount; i++)
		{
			std::string uniformName = "u_ModelMatrices[" + std::to_string(i) + "]";
			cmd->shader->SetMat4(uniformName, cmd->transforms[i]);
		}
		cmd->shader->SetInt("u_InstanceCount", static_cast<int>(instanceCount));
		
		// Bind textures with state tracking
		if (cmd->useTextureMaps)
		{
			if (cmd->diffuseMap)
			{
				u32 texID = cmd->diffuseMap->GetRendererID();
				if (s_Data.BoundTextureIDs[0] != texID)
				{
					cmd->diffuseMap->Bind(0);
					s_Data.BoundTextureIDs[0] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->specularMap)
			{
				u32 texID = cmd->specularMap->GetRendererID();
				if (s_Data.BoundTextureIDs[1] != texID)
				{
					cmd->specularMap->Bind(1);
					s_Data.BoundTextureIDs[1] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
		}
		
		// Get index count efficiently
		u32 indexCount = cmd->indexCount > 0 ? cmd->indexCount : 
			cmd->vertexArray->GetIndexBuffer() ? cmd->vertexArray->GetIndexBuffer()->GetCount() : 0;
		
		if (indexCount == 0)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: No indices to draw");
			return;
		}
		
		// Track draw calls for statistics
		s_Data.Stats.DrawCalls++;
		
		// Draw the instanced mesh
		api.DrawIndexedInstanced(cmd->vertexArray, indexCount, instanceCount);
	}
    
    void CommandDispatch::DrawQuad(const void* data, RendererAPI& api)
	{
		OLO_PROFILE_FUNCTION();
		
		auto const* cmd = static_cast<const DrawQuadCommand*>(data);
		
		if (!cmd->quadVA || !cmd->shader)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawQuad: Invalid vertex array or shader");
			return;
		}
		
		// Optimize shader binding - only bind if different from currently bound
		u32 shaderID = cmd->shader->GetRendererID();
		if (s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		
		// Update transform UBO with model matrix
		UpdateTransformUBO(cmd->transform);
		
		// Bind texture with state tracking
		if (cmd->texture)
		{
			u32 texID = cmd->texture->GetRendererID();
			if (s_Data.BoundTextureIDs[0] != texID)
			{
				cmd->texture->Bind(0);
				cmd->shader->SetInt("u_Texture", 0);
				s_Data.BoundTextureIDs[0] = texID;
				s_Data.Stats.TextureBinds++;
			}
		}
		
		// Track draw calls for statistics
		s_Data.Stats.DrawCalls++;
		
		// Draw the quad
		api.DrawIndexed(cmd->quadVA, 6); // Quad has 6 indices (2 triangles)
	}

	void CommandDispatch::ResetState()
	{
		s_Data.CurrentBoundShaderID = 0;
		std::fill(s_Data.BoundTextureIDs.begin(), s_Data.BoundTextureIDs.end(), 0);
		s_Data.Stats.Reset();
	}
}