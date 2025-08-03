#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <glad/gl.h>

namespace OloEngine
{	struct CommandDispatchData
	{
		AssetRef<UniformBuffer> CameraUBO = nullptr;
		AssetRef<UniformBuffer> MaterialUBO = nullptr;
		AssetRef<UniformBuffer> LightUBO = nullptr;
		AssetRef<UniformBuffer> BoneMatricesUBO = nullptr;
		AssetRef<UniformBuffer> ModelMatrixUBO = nullptr;
		glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
		glm::mat4 ViewMatrix = glm::mat4(1.0f);
		glm::mat4 ProjectionMatrix = glm::mat4(1.0f);
		Light SceneLight;
		glm::vec3 ViewPos = glm::vec3(0.0f);
		
		u32 CurrentBoundShaderID = 0;
		std::array<u32, 32> BoundTextureIDs = { 0 };
		
		CommandDispatch::Statistics Stats;
	};

	static CommandDispatchData s_Data;
	
	// Array of dispatch functions indexed by CommandType
	static CommandDispatchFn s_DispatchTable[static_cast<sizet>(CommandType::SetMultisampling) + 1];
	
	void CommandDispatch::Initialize()
	{
		OLO_PROFILE_FUNCTION();
		
		// Initialize dispatch table
		std::ranges::fill(s_DispatchTable, nullptr);
        
        // State management dispatch functions
        s_DispatchTable[static_cast<sizet>(CommandType::SetViewport)] = CommandDispatch::SetViewport;
        s_DispatchTable[static_cast<sizet>(CommandType::SetClearColor)] = CommandDispatch::SetClearColor;
        s_DispatchTable[static_cast<sizet>(CommandType::Clear)] = CommandDispatch::Clear;
        s_DispatchTable[static_cast<sizet>(CommandType::ClearStencil)] = CommandDispatch::ClearStencil;
        s_DispatchTable[static_cast<sizet>(CommandType::SetBlendState)] = CommandDispatch::SetBlendState;
        s_DispatchTable[static_cast<sizet>(CommandType::SetBlendFunc)] = CommandDispatch::SetBlendFunc;
        s_DispatchTable[static_cast<sizet>(CommandType::SetBlendEquation)] = CommandDispatch::SetBlendEquation;
        s_DispatchTable[static_cast<sizet>(CommandType::SetDepthTest)] = CommandDispatch::SetDepthTest;
        s_DispatchTable[static_cast<sizet>(CommandType::SetDepthMask)] = CommandDispatch::SetDepthMask;
        s_DispatchTable[static_cast<sizet>(CommandType::SetDepthFunc)] = CommandDispatch::SetDepthFunc;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilTest)] = CommandDispatch::SetStencilTest;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilFunc)] = CommandDispatch::SetStencilFunc;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilMask)] = CommandDispatch::SetStencilMask;
        s_DispatchTable[static_cast<sizet>(CommandType::SetStencilOp)] = CommandDispatch::SetStencilOp;
        s_DispatchTable[static_cast<sizet>(CommandType::SetCulling)] = CommandDispatch::SetCulling;
        s_DispatchTable[static_cast<sizet>(CommandType::SetCullFace)] = CommandDispatch::SetCullFace;
        s_DispatchTable[static_cast<sizet>(CommandType::SetLineWidth)] = CommandDispatch::SetLineWidth;
        s_DispatchTable[static_cast<sizet>(CommandType::SetPolygonMode)] = CommandDispatch::SetPolygonMode;
        s_DispatchTable[static_cast<sizet>(CommandType::SetPolygonOffset)] = CommandDispatch::SetPolygonOffset;
        s_DispatchTable[static_cast<sizet>(CommandType::SetScissorTest)] = CommandDispatch::SetScissorTest;
        s_DispatchTable[static_cast<sizet>(CommandType::SetScissorBox)] = CommandDispatch::SetScissorBox;
        s_DispatchTable[static_cast<sizet>(CommandType::SetColorMask)] = CommandDispatch::SetColorMask;
        s_DispatchTable[static_cast<sizet>(CommandType::SetMultisampling)] = CommandDispatch::SetMultisampling;
        
        // Draw commands dispatch functions
        s_DispatchTable[static_cast<sizet>(CommandType::BindDefaultFramebuffer)] = CommandDispatch::BindDefaultFramebuffer;
        s_DispatchTable[static_cast<sizet>(CommandType::BindTexture)] = CommandDispatch::BindTexture;
        s_DispatchTable[static_cast<sizet>(CommandType::SetShaderResource)] = CommandDispatch::SetShaderResource;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawIndexed)] = CommandDispatch::DrawIndexed;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawIndexedInstanced)] = CommandDispatch::DrawIndexedInstanced;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawArrays)] = CommandDispatch::DrawArrays;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawLines)] = CommandDispatch::DrawLines;
        // Higher-level commands
        s_DispatchTable[static_cast<sizet>(CommandType::DrawMesh)] = CommandDispatch::DrawMesh;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawMeshInstanced)] = CommandDispatch::DrawMeshInstanced;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawSkinnedMesh)] = CommandDispatch::DrawSkinnedMesh;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawSkybox)] = CommandDispatch::DrawSkybox;
        s_DispatchTable[static_cast<sizet>(CommandType::DrawQuad)] = CommandDispatch::DrawQuad;
		
		s_Data.CurrentBoundShaderID = 0;
		std::fill(s_Data.BoundTextureIDs.begin(), s_Data.BoundTextureIDs.end(), 0);
		s_Data.Stats.Reset();
		
		OLO_CORE_INFO("CommandDispatch: Initialized (UBOs managed by Renderer3D)");
	}
	
	void CommandDispatch::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
		s_Data.CameraUBO.Reset();
		s_Data.MaterialUBO.Reset();
		s_Data.LightUBO.Reset();
		s_Data.BoneMatricesUBO.Reset();
		s_Data.ModelMatrixUBO.Reset();
	}

	void CommandDispatch::SetUBOReferences(
		const AssetRef<UniformBuffer>& cameraUBO,
		const AssetRef<UniformBuffer>& materialUBO,
		const AssetRef<UniformBuffer>& lightUBO,
		const AssetRef<UniformBuffer>& boneMatricesUBO,
		const AssetRef<UniformBuffer>& modelMatrixUBO)
	{
		s_Data.CameraUBO = cameraUBO;
		s_Data.MaterialUBO = materialUBO;
		s_Data.LightUBO = lightUBO;
		s_Data.BoneMatricesUBO = boneMatricesUBO;
		s_Data.ModelMatrixUBO = modelMatrixUBO;
	}

	void CommandDispatch::ResetState()
	{
		s_Data.CurrentBoundShaderID = 0;
		s_Data.BoundTextureIDs.fill(0);
		s_Data.Stats.Reset();
	}

	void CommandDispatch::SetViewProjectionMatrix(const glm::mat4& vp)
	{
		s_Data.ViewProjectionMatrix = vp;
	}

	void CommandDispatch::SetViewMatrix(const glm::mat4& view)
	{
		s_Data.ViewMatrix = view;
	}

	void CommandDispatch::SetProjectionMatrix(const glm::mat4& projection)
	{
		s_Data.ProjectionMatrix = projection;
	}

	void CommandDispatch::SetSceneLight(const Light& light)
	{
		s_Data.SceneLight = light;
	}

	void CommandDispatch::SetViewPosition(const glm::vec3& viewPos)
	{
		s_Data.ViewPos = viewPos;
	}

	CommandDispatch::Statistics& CommandDispatch::GetStatistics()
	{
		return s_Data.Stats;
	}

    void CommandDispatch::UpdateMaterialTextureFlag(bool useTextures)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Data.MaterialUBO)
        {
            OLO_CORE_WARN("CommandDispatch::UpdateMaterialTextureFlag: MaterialUBO not initialized");
            return;
        }
        
        // Update only the UseTextureMaps field in the material UBO
        i32 flag = useTextures ? 1 : 0;
        u32 offset = static_cast<u32>(offsetof(ShaderBindingLayout::MaterialUBO, UseTextureMaps));
        
        s_Data.MaterialUBO->SetData(&flag, sizeof(i32), offset);
    }

    CommandDispatchFn CommandDispatch::GetDispatchFunction(CommandType type)
    {
        if (type == CommandType::Invalid || static_cast<sizet>(type) >= sizeof(s_DispatchTable) / sizeof(CommandDispatchFn))
        {
            OLO_CORE_ERROR("CommandDispatch::GetDispatchFunction: Invalid command type {}", static_cast<int>(type));
            return nullptr;
        }
        
        return s_DispatchTable[static_cast<sizet>(type)];
    }
    
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
        // TODO(olbu): Have separate methods for partial clears
        if (cmd->clearColor || cmd->clearDepth)
            api.Clear();
    }
    
    void CommandDispatch::ClearStencil(const void* /*data*/, RendererAPI& api)
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
    
    void CommandDispatch::BindDefaultFramebuffer(const void* /*data*/, RendererAPI& api)
    {
        api.BindDefaultFramebuffer();
    }
    
    void CommandDispatch::BindTexture(const void* data, RendererAPI& api)
    {
        auto const* cmd = static_cast<const BindTextureCommand*>(data);
        api.BindTexture(cmd->slot, cmd->textureID);
    }
    
    void CommandDispatch::SetShaderResource(const void* data, RendererAPI& /*api*/)
    {
        auto const* cmd = static_cast<const SetShaderResourceCommand*>(data);
        
        auto* registry = Renderer3D::GetShaderRegistry(cmd->shaderID);
        if (registry)
        {
            bool success = registry->SetResource(cmd->resourceName, cmd->resourceInput);
            if (!success)
            {
                OLO_CORE_WARN("Failed to set shader resource '{0}' for shader ID {1}", 
                              cmd->resourceName, cmd->shaderID);
            }
        }
        else
        {
            OLO_CORE_WARN("No registry found for shader ID {0} when setting resource '{1}'", 
                          cmd->shaderID, cmd->resourceName);
        }
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
    
    void CommandDispatch::DrawMesh(const void* data, RendererAPI& api)
    {
        OLO_PROFILE_FUNCTION();
        auto const* cmd = static_cast<const DrawMeshCommand*>(data);
        if (!cmd->vertexArray || !cmd->shader)
        {
            OLO_CORE_ERROR("CommandDispatch::DrawMesh: Invalid vertex array or shader");
            return;
        }

        if (cmd->renderState)
        {
            const RenderState& state = *cmd->renderState;
            api.SetBlendState(state.Blend.Enabled);
            api.SetBlendFunc(state.Blend.SrcFactor, state.Blend.DstFactor);
            api.SetBlendEquation(state.Blend.Equation);
            api.SetDepthTest(state.Depth.TestEnabled);
            api.SetDepthFunc(state.Depth.Function);
            api.SetDepthMask(state.Depth.WriteMask);
            if (state.Stencil.Enabled)
                api.EnableStencilTest();
            else
                api.DisableStencilTest();
            api.SetStencilFunc(state.Stencil.Function, state.Stencil.Reference, state.Stencil.ReadMask);
            api.SetStencilMask(state.Stencil.WriteMask);
            api.SetStencilOp(state.Stencil.StencilFail, state.Stencil.DepthFail, state.Stencil.DepthPass);
            if (state.Culling.Enabled)
                api.EnableCulling();
            else
                api.DisableCulling();
            api.SetCullFace(state.Culling.Face);
            api.SetLineWidth(state.LineWidth.Width);
			api.SetPolygonMode(state.PolygonMode.Face, state.PolygonMode.Mode);

            if (state.Scissor.Enabled)
                api.EnableScissorTest();
            else
                api.DisableScissorTest();
            api.SetScissorBox(state.Scissor.X, state.Scissor.Y, state.Scissor.Width, state.Scissor.Height);
            api.SetColorMask(state.ColorMask.Red, state.ColorMask.Green, state.ColorMask.Blue, state.ColorMask.Alpha);
            api.SetPolygonOffset(state.PolygonOffset.Enabled ? state.PolygonOffset.Factor : 0.0f, state.PolygonOffset.Enabled ? state.PolygonOffset.Units : 0.0f);
            if (state.Multisampling.Enabled) api.EnableMultisampling(); else api.DisableMultisampling();
        }
		
		if (u32 shaderID = cmd->shader->GetRendererID(); s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		
		ShaderBindingLayout::CameraUBO cameraData;
		cameraData.ViewProjection = s_Data.ViewProjectionMatrix;
		cameraData.View = s_Data.ViewMatrix;
		// Calculate projection matrix from ViewProjection and View: Projection = ViewProjection * inverse(View)
		cameraData.Projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);
		cameraData.Position = s_Data.ViewPos; // Add camera position
		cameraData._padding0 = 0.0f; // Initialize padding
		
		if (s_Data.CameraUBO)
		{
			constexpr u32 expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch in DrawMesh");
			s_Data.CameraUBO->SetData(&cameraData, expectedSize);
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
		}
		
		// Update model matrix UBO
		if (s_Data.ModelMatrixUBO)
		{
			ShaderBindingLayout::ModelUBO modelData;
			modelData.Model = cmd->transform;
			modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
			
			constexpr u32 expectedSize = ShaderBindingLayout::ModelUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::ModelUBO) == expectedSize, "ModelUBO size mismatch");
			
			s_Data.ModelMatrixUBO->SetData(&modelData, expectedSize);
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
		}
		
		// Update material UBO - use PBR if enabled, otherwise use legacy
		if (cmd->enablePBR)
		{
			ShaderBindingLayout::PBRMaterialUBO pbrMaterialData;
			pbrMaterialData.BaseColorFactor = cmd->baseColorFactor;
			pbrMaterialData.EmissiveFactor = cmd->emissiveFactor;
			pbrMaterialData.MetallicFactor = cmd->metallicFactor;
			pbrMaterialData.RoughnessFactor = cmd->roughnessFactor;
			pbrMaterialData.NormalScale = cmd->normalScale;
			pbrMaterialData.OcclusionStrength = cmd->occlusionStrength;
			pbrMaterialData.UseAlbedoMap = cmd->albedoMap ? 1 : 0;
			pbrMaterialData.UseNormalMap = cmd->normalMap ? 1 : 0;
			pbrMaterialData.UseMetallicRoughnessMap = cmd->metallicRoughnessMap ? 1 : 0;
			pbrMaterialData.UseAOMap = cmd->aoMap ? 1 : 0;
			pbrMaterialData.UseEmissiveMap = cmd->emissiveMap ? 1 : 0;
			pbrMaterialData.EnableIBL = cmd->enableIBL ? 1 : 0;
			pbrMaterialData.ApplyGammaCorrection = 1;  // Enable gamma correction by default
			pbrMaterialData.AlphaCutoff = 0;           // Default alpha cutoff
			
			if (s_Data.MaterialUBO)
			{
				constexpr u32 expectedSize = ShaderBindingLayout::PBRMaterialUBO::GetSize();
				static_assert(sizeof(ShaderBindingLayout::PBRMaterialUBO) == expectedSize, "PBRMaterialUBO size mismatch in DrawMesh");
				s_Data.MaterialUBO->SetData(&pbrMaterialData, expectedSize);
				glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
			}
		}
		else
		{
			ShaderBindingLayout::MaterialUBO materialData;
			materialData.Ambient = glm::vec4(cmd->ambient, 1.0f);
			materialData.Diffuse = glm::vec4(cmd->diffuse, 1.0f);
			materialData.Specular = glm::vec4(cmd->specular, cmd->shininess);
			materialData.Emissive = glm::vec4(0.0f);
			materialData.UseTextureMaps = cmd->useTextureMaps ? 1 : 0;
			materialData.AlphaMode = 0;                // Default alpha mode
			materialData.DoubleSided = 0;              // Default double-sided
			materialData._padding = 0;                 // Clear remaining padding
			
			if (s_Data.MaterialUBO)
			{
				constexpr u32 expectedSize = ShaderBindingLayout::MaterialUBO::GetSize();
				static_assert(sizeof(ShaderBindingLayout::MaterialUBO) == expectedSize, "MaterialUBO size mismatch in DrawMesh");
				s_Data.MaterialUBO->SetData(&materialData, expectedSize);
				glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
			}
		}
		
		const Light& light = s_Data.SceneLight;
		auto lightType = std::to_underlying(light.Type);
		
		ShaderBindingLayout::LightUBO lightData;
		lightData.LightPosition = glm::vec4(light.Position, 1.0f);
		lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
		lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
		lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
		lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);
		lightData.LightAttParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, 0.0f);
		lightData.LightSpotParams = glm::vec4(light.CutOff, light.OuterCutOff, 0.0f, 0.0f);
		lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(lightType));

		if (s_Data.LightUBO)
		{
			constexpr u32 expectedSize = ShaderBindingLayout::LightUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::LightUBO) == expectedSize, "LightUBO size mismatch");
			s_Data.LightUBO->SetData(&lightData, expectedSize);
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_LIGHTS, s_Data.LightUBO->GetRendererID());
		}
		
		// Bind textures based on material type
		if (cmd->enablePBR)
		{
			// PBR texture binding
			if (cmd->albedoMap)
			{
				u32 texID = cmd->albedoMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != texID)
				{
					cmd->albedoMap->Bind(ShaderBindingLayout::TEX_DIFFUSE);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->metallicRoughnessMap)
			{
				u32 texID = cmd->metallicRoughnessMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != texID)
				{
					cmd->metallicRoughnessMap->Bind(ShaderBindingLayout::TEX_SPECULAR);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->normalMap)
			{
				u32 texID = cmd->normalMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_NORMAL] != texID)
				{
					cmd->normalMap->Bind(ShaderBindingLayout::TEX_NORMAL);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_NORMAL] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->aoMap)
			{
				u32 texID = cmd->aoMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_AMBIENT] != texID)
				{
					cmd->aoMap->Bind(ShaderBindingLayout::TEX_AMBIENT);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_AMBIENT] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->emissiveMap)
			{
				u32 texID = cmd->emissiveMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_EMISSIVE] != texID)
				{
					cmd->emissiveMap->Bind(ShaderBindingLayout::TEX_EMISSIVE);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_EMISSIVE] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->environmentMap)
			{
				u32 texID = cmd->environmentMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] != texID)
				{
					cmd->environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->irradianceMap)
			{
				u32 texID = cmd->irradianceMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] != texID)
				{
					cmd->irradianceMap->Bind(ShaderBindingLayout::TEX_USER_0);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->prefilterMap)
			{
				u32 texID = cmd->prefilterMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] != texID)
				{
					cmd->prefilterMap->Bind(ShaderBindingLayout::TEX_USER_1);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->brdfLutMap)
			{
				u32 texID = cmd->brdfLutMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] != texID)
				{
					cmd->brdfLutMap->Bind(ShaderBindingLayout::TEX_USER_2);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
		}
		else if (cmd->useTextureMaps)
		{
			// Legacy texture binding
			if (cmd->diffuseMap)
			{
				u32 texID = cmd->diffuseMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != texID)
				{
					cmd->diffuseMap->Bind(ShaderBindingLayout::TEX_DIFFUSE);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->specularMap)
			{
				u32 texID = cmd->specularMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != texID)
				{
					cmd->specularMap->Bind(ShaderBindingLayout::TEX_SPECULAR);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
		}
		
		u32 indexCount = cmd->indexCount > 0 ? cmd->indexCount : 
			cmd->vertexArray->GetIndexBuffer() ? cmd->vertexArray->GetIndexBuffer()->GetCount() : 0;
		
		if (indexCount == 0)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawMesh: No indices to draw");
			return;
		}
		
		s_Data.Stats.DrawCalls++;
		
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

        if (cmd->renderState)
        {
            const RenderState& state = *cmd->renderState;
            api.SetBlendState(state.Blend.Enabled);
            api.SetBlendFunc(state.Blend.SrcFactor, state.Blend.DstFactor);
            api.SetBlendEquation(state.Blend.Equation);
            api.SetDepthTest(state.Depth.TestEnabled);
            api.SetDepthFunc(state.Depth.Function);
            api.SetDepthMask(state.Depth.WriteMask);
            if (state.Stencil.Enabled)
                api.EnableStencilTest();
            else
                api.DisableStencilTest();
            api.SetStencilFunc(state.Stencil.Function, state.Stencil.Reference, state.Stencil.ReadMask);
            api.SetStencilMask(state.Stencil.WriteMask);
            api.SetStencilOp(state.Stencil.StencilFail, state.Stencil.DepthFail, state.Stencil.DepthPass);
            if (state.Culling.Enabled)
                api.EnableCulling();
            else
                api.DisableCulling();
            api.SetCullFace(state.Culling.Face);
            api.SetLineWidth(state.LineWidth.Width);
            api.SetPolygonMode(state.PolygonMode.Face, state.PolygonMode.Mode);
            if (state.Scissor.Enabled)
                api.EnableScissorTest();
            else
                api.DisableScissorTest();
            api.SetScissorBox(state.Scissor.X, state.Scissor.Y, state.Scissor.Width, state.Scissor.Height);
            api.SetColorMask(state.ColorMask.Red, state.ColorMask.Green, state.ColorMask.Blue, state.ColorMask.Alpha);
            api.SetPolygonOffset(state.PolygonOffset.Enabled ? state.PolygonOffset.Factor : 0.0f, state.PolygonOffset.Enabled ? state.PolygonOffset.Units : 0.0f);
            if (state.Multisampling.Enabled) api.EnableMultisampling(); else api.DisableMultisampling();
        }
        	
		if (u32 shaderID = cmd->shader->GetRendererID(); s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		
		// Update material UBO
		ShaderBindingLayout::MaterialUBO materialData;
		materialData.Ambient = glm::vec4(cmd->ambient, 1.0f);
		materialData.Diffuse = glm::vec4(cmd->diffuse, 1.0f);
		materialData.Specular = glm::vec4(cmd->specular, cmd->shininess);
		materialData.Emissive = glm::vec4(0.0f);
		materialData.UseTextureMaps = cmd->useTextureMaps ? 1 : 0;
		materialData.AlphaMode = 0;                // Default alpha mode
		materialData.DoubleSided = 0;              // Default double-sided
		materialData._padding = 0;                 // Clear remaining padding
		
		if (s_Data.MaterialUBO)
		{
			constexpr u32 expectedSize = ShaderBindingLayout::MaterialUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::MaterialUBO) == expectedSize, "MaterialUBO size mismatch");
			
			s_Data.MaterialUBO->SetData(&materialData, expectedSize);
		}
		
		UpdateMaterialTextureFlag(cmd->useTextureMaps);
		
		// For instanced rendering, we'll set model matrices as shader uniforms
		// A proper implementation would use an instance buffer or SSBO for better performance
		// This is a simplified approach for now
		const sizet maxInstances = 100;
		if (cmd->transforms.size() > maxInstances)
		{
			OLO_CORE_WARN("CommandDispatch::DrawMeshInstanced: Too many instances ({}). Only first {} will be rendered.",
						cmd->transforms.size(), maxInstances);
		}
		
		// Set instance transforms (limited approach - a better solution would use SSBOs or instance buffers)
		sizet instanceCount = std::min(cmd->transforms.size(), maxInstances);
		for (sizet i = 0; i < instanceCount; i++)
		{
			std::string uniformName = "u_ModelMatrices[" + std::to_string(i) + "]";
			cmd->shader->SetMat4(uniformName, cmd->transforms[i]);
		}
		cmd->shader->SetInt("u_InstanceCount", static_cast<int>(instanceCount));
		
		if (cmd->useTextureMaps)
		{
			if (cmd->diffuseMap)
			{
				u32 texID = cmd->diffuseMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != texID)
				{
					cmd->diffuseMap->Bind(ShaderBindingLayout::TEX_DIFFUSE);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->specularMap)
			{
				u32 texID = cmd->specularMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != texID)
				{
					cmd->specularMap->Bind(ShaderBindingLayout::TEX_SPECULAR);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
		}
		
		u32 indexCount = cmd->indexCount > 0 ? cmd->indexCount : 
			cmd->vertexArray->GetIndexBuffer() ? cmd->vertexArray->GetIndexBuffer()->GetCount() : 0;
		
		if (indexCount == 0)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawMeshInstanced: No indices to draw");
			return;
		}
		
		s_Data.Stats.DrawCalls++;
		api.DrawIndexedInstanced(cmd->vertexArray, indexCount, static_cast<u32>(instanceCount));
	}	   
	
	void CommandDispatch::DrawSkinnedMesh(const void* data, RendererAPI& api)
	{
		OLO_PROFILE_FUNCTION();
		auto const* cmd = static_cast<const DrawSkinnedMeshCommand*>(data);
		
		if (!cmd->vertexArray || !cmd->shader)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawSkinnedMesh: Invalid vertex array or shader");
			return;
		}

		if (cmd->renderState)
		{
			const RenderState& state = *cmd->renderState;
			api.SetBlendState(state.Blend.Enabled);
			api.SetBlendFunc(state.Blend.SrcFactor, state.Blend.DstFactor);
			api.SetBlendEquation(state.Blend.Equation);
			api.SetDepthTest(state.Depth.TestEnabled);
			api.SetDepthFunc(state.Depth.Function);
			api.SetDepthMask(state.Depth.WriteMask);
			if (state.Stencil.Enabled) api.EnableStencilTest(); else api.DisableStencilTest();
			api.SetStencilFunc(state.Stencil.Function, state.Stencil.Reference, state.Stencil.ReadMask);
			api.SetStencilMask(state.Stencil.WriteMask);
			api.SetStencilOp(state.Stencil.StencilFail, state.Stencil.DepthFail, state.Stencil.DepthPass);
			if (state.Culling.Enabled) api.EnableCulling(); else api.DisableCulling();
			api.SetCullFace(state.Culling.Face);
			api.SetLineWidth(state.LineWidth.Width);
			api.SetPolygonMode(state.PolygonMode.Face, state.PolygonMode.Mode);
			if (state.Scissor.Enabled) api.EnableScissorTest(); else api.DisableScissorTest();
			api.SetScissorBox(state.Scissor.X, state.Scissor.Y, state.Scissor.Width, state.Scissor.Height);
			api.SetColorMask(state.ColorMask.Red, state.ColorMask.Green, state.ColorMask.Blue, state.ColorMask.Alpha);
			api.SetPolygonOffset(state.PolygonOffset.Enabled ? state.PolygonOffset.Factor : 0.0f, state.PolygonOffset.Enabled ? state.PolygonOffset.Units : 0.0f);
			if (state.Multisampling.Enabled) api.EnableMultisampling(); else api.DisableMultisampling();
		}
		
		if (u32 shaderID = cmd->shader->GetRendererID(); s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		// Note: Skinned shader expects binding 0 to have ViewProjection + View, not ViewProjection + Model
		ShaderBindingLayout::CameraUBO cameraData;
		cameraData.ViewProjection = s_Data.ViewProjectionMatrix;
		cameraData.View = s_Data.ViewMatrix;
		// Calculate projection matrix from ViewProjection and View: Projection = ViewProjection * inverse(View)
		cameraData.Projection = s_Data.ViewProjectionMatrix * glm::inverse(s_Data.ViewMatrix);
		cameraData.Position = s_Data.ViewPos;
		cameraData._padding0 = 0.0f;
		
		if (s_Data.CameraUBO)
		{
			constexpr u32 expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch in DrawSkinnedMesh");
			s_Data.CameraUBO->SetData(&cameraData, expectedSize);
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, s_Data.CameraUBO->GetRendererID());
		}
		
		// Update model matrix UBO
		if (s_Data.ModelMatrixUBO)
		{
			ShaderBindingLayout::ModelUBO modelData;
			modelData.Model = cmd->modelMatrix;
			modelData.Normal = glm::transpose(glm::inverse(cmd->modelMatrix));
			
			constexpr u32 expectedSize = ShaderBindingLayout::ModelUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::ModelUBO) == expectedSize, "ModelUBO size mismatch");
			
			s_Data.ModelMatrixUBO->SetData(&modelData, expectedSize);
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
		}
		

		// Update material UBO - use PBR if enabled, otherwise use legacy
		if (cmd->enablePBR)
		{
			ShaderBindingLayout::PBRMaterialUBO pbrMaterialData;
			pbrMaterialData.BaseColorFactor = cmd->baseColorFactor;
			pbrMaterialData.EmissiveFactor = cmd->emissiveFactor;
			pbrMaterialData.MetallicFactor = cmd->metallicFactor;
			pbrMaterialData.RoughnessFactor = cmd->roughnessFactor;
			pbrMaterialData.NormalScale = cmd->normalScale;
			pbrMaterialData.OcclusionStrength = cmd->occlusionStrength;
			pbrMaterialData.UseAlbedoMap = cmd->albedoMap ? 1 : 0;
			pbrMaterialData.UseNormalMap = cmd->normalMap ? 1 : 0;
			pbrMaterialData.UseMetallicRoughnessMap = cmd->metallicRoughnessMap ? 1 : 0;
			pbrMaterialData.UseAOMap = cmd->aoMap ? 1 : 0;
			pbrMaterialData.UseEmissiveMap = cmd->emissiveMap ? 1 : 0;
			pbrMaterialData.EnableIBL = cmd->enableIBL ? 1 : 0;
			pbrMaterialData.ApplyGammaCorrection = 1;  // Enable gamma correction by default
			pbrMaterialData.AlphaCutoff = 0;           // Default alpha cutoff
			
			if (s_Data.MaterialUBO)
			{
				constexpr u32 expectedSize = ShaderBindingLayout::PBRMaterialUBO::GetSize();
				static_assert(sizeof(ShaderBindingLayout::PBRMaterialUBO) == expectedSize, "PBRMaterialUBO size mismatch in DrawSkinnedMesh");
				s_Data.MaterialUBO->SetData(&pbrMaterialData, expectedSize);
				glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
			}
		}
		else
		{
			ShaderBindingLayout::MaterialUBO materialData;
			materialData.Ambient = glm::vec4(cmd->ambient, 1.0f);
			materialData.Diffuse = glm::vec4(cmd->diffuse, 1.0f);
			materialData.Specular = glm::vec4(cmd->specular, cmd->shininess);
			materialData.Emissive = glm::vec4(0.0f);
			materialData.UseTextureMaps = cmd->useTextureMaps ? 1 : 0;
			materialData.AlphaMode = 0;                // Default alpha mode
			materialData.DoubleSided = 0;              // Default double-sided
			materialData._padding = 0;                 // Clear remaining padding
			
			if (s_Data.MaterialUBO)
			{
				constexpr u32 expectedSize = ShaderBindingLayout::MaterialUBO::GetSize();
				static_assert(sizeof(ShaderBindingLayout::MaterialUBO) == expectedSize, "MaterialUBO size mismatch in DrawSkinnedMesh");
				s_Data.MaterialUBO->SetData(&materialData, expectedSize);
				glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MATERIAL, s_Data.MaterialUBO->GetRendererID());
			}
		}
		
		if (s_Data.BoneMatricesUBO && !cmd->boneMatrices.empty())
		{
			constexpr sizet MAX_BONES = 100;
			sizet boneCount = glm::min(cmd->boneMatrices.size(), MAX_BONES);
			
			s_Data.BoneMatricesUBO->SetData(cmd->boneMatrices.data(), static_cast<u32>(boneCount * sizeof(glm::mat4)));
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_ANIMATION, s_Data.BoneMatricesUBO->GetRendererID());
		}

		const Light& light = s_Data.SceneLight;
		auto lightType = std::to_underlying(light.Type);
		
		ShaderBindingLayout::LightUBO lightData;
		lightData.LightPosition = glm::vec4(light.Position, 1.0f);
		lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
		lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
		lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
		lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);
		lightData.LightAttParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, 0.0f);
		lightData.LightSpotParams = glm::vec4(light.CutOff, light.OuterCutOff, 0.0f, 0.0f);
		lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(lightType));

		if (s_Data.LightUBO)
		{
			constexpr u32 expectedSize = ShaderBindingLayout::LightUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::LightUBO) == expectedSize, "LightUBO size mismatch in DrawSkinnedMesh");
			s_Data.LightUBO->SetData(&lightData, expectedSize);
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_LIGHTS, s_Data.LightUBO->GetRendererID());
		}
		
		// Bind textures based on material type
		if (cmd->enablePBR)
		{
			// PBR texture binding
			if (cmd->albedoMap)
			{
				u32 texID = cmd->albedoMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != texID)
				{
					cmd->albedoMap->Bind(ShaderBindingLayout::TEX_DIFFUSE);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->metallicRoughnessMap)
			{
				u32 texID = cmd->metallicRoughnessMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != texID)
				{
					cmd->metallicRoughnessMap->Bind(ShaderBindingLayout::TEX_SPECULAR);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->normalMap)
			{
				u32 texID = cmd->normalMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_NORMAL] != texID)
				{
					cmd->normalMap->Bind(ShaderBindingLayout::TEX_NORMAL);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_NORMAL] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->aoMap)
			{
				u32 texID = cmd->aoMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_AMBIENT] != texID)
				{
					cmd->aoMap->Bind(ShaderBindingLayout::TEX_AMBIENT);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_AMBIENT] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->emissiveMap)
			{
				u32 texID = cmd->emissiveMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_EMISSIVE] != texID)
				{
					cmd->emissiveMap->Bind(ShaderBindingLayout::TEX_EMISSIVE);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_EMISSIVE] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->environmentMap)
			{
				u32 texID = cmd->environmentMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] != texID)
				{
					cmd->environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_ENVIRONMENT] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->irradianceMap)
			{
				u32 texID = cmd->irradianceMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] != texID)
				{
					cmd->irradianceMap->Bind(ShaderBindingLayout::TEX_USER_0);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_0] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->prefilterMap)
			{
				u32 texID = cmd->prefilterMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] != texID)
				{
					cmd->prefilterMap->Bind(ShaderBindingLayout::TEX_USER_1);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_1] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->brdfLutMap)
			{
				u32 texID = cmd->brdfLutMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] != texID)
				{
					cmd->brdfLutMap->Bind(ShaderBindingLayout::TEX_USER_2);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_USER_2] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
		}
		else if (cmd->useTextureMaps)
		{
			// Legacy texture binding
			if (cmd->diffuseMap)
			{
				u32 texID = cmd->diffuseMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] != texID)
				{
					cmd->diffuseMap->Bind(ShaderBindingLayout::TEX_DIFFUSE);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
			
			if (cmd->specularMap)
			{
				u32 texID = cmd->specularMap->GetRendererID();
				if (s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] != texID)
				{
					cmd->specularMap->Bind(ShaderBindingLayout::TEX_SPECULAR);
					s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_SPECULAR] = texID;
					s_Data.Stats.TextureBinds++;
				}
			}
		}
		
		u32 indexCount = cmd->indexCount > 0 ? cmd->indexCount : 
			cmd->vertexArray->GetIndexBuffer() ? cmd->vertexArray->GetIndexBuffer()->GetCount() : 0;
		
		if (indexCount == 0)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawSkinnedMesh: No indices to draw");
			return;
		}
		
		s_Data.Stats.DrawCalls++;
		
		api.DrawIndexed(cmd->vertexArray, indexCount);
	}

	void CommandDispatch::DrawSkybox(const void* data, RendererAPI& api)
	{
		OLO_PROFILE_FUNCTION();
		
		auto const* cmd = static_cast<const DrawSkyboxCommand*>(data);
		
		if (!cmd->vertexArray || !cmd->shader || !cmd->skyboxTexture)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawSkybox: Invalid vertex array, shader, or skybox texture");
			return;
		}

		// Apply skybox-specific render state manually
		api.SetDepthTest(true);
		api.SetDepthFunc(GL_LEQUAL); // Important for skybox
		api.SetDepthMask(false); // Don't write to depth buffer
		api.DisableCulling(); // Don't cull faces for skybox

		// Bind skybox shader
		cmd->shader->Bind();
		s_Data.CurrentBoundShaderID = cmd->shader->GetRendererID();

		// Bind skybox texture to the correct slot
		cmd->skyboxTexture->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);

		// Bind vertex array
		cmd->vertexArray->Bind();

		// Draw skybox
		api.DrawIndexed(cmd->vertexArray, cmd->indexCount);

		// Restore default render state
		api.SetDepthFunc(GL_LESS);
		api.SetDepthMask(true);
		api.EnableCulling();

		// Update statistics
		s_Data.Stats.DrawCalls++;
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
		
		if (!cmd->texture)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawQuad: Missing texture for quad");
			return;
		}

		if (cmd->renderState)
		{
			const RenderState& state = *cmd->renderState;
			api.SetBlendState(state.Blend.Enabled);
			api.SetBlendFunc(state.Blend.SrcFactor, state.Blend.DstFactor);
			api.SetBlendEquation(state.Blend.Equation);
			api.SetDepthTest(state.Depth.TestEnabled);
			api.SetDepthFunc(state.Depth.Function);
			api.SetDepthMask(state.Depth.WriteMask);
			if (state.Stencil.Enabled)
				api.EnableStencilTest();
			else
				api.DisableStencilTest();
			api.SetStencilFunc(state.Stencil.Function, state.Stencil.Reference, state.Stencil.ReadMask);
			api.SetStencilMask(state.Stencil.WriteMask);
			api.SetStencilOp(state.Stencil.StencilFail, state.Stencil.DepthFail, state.Stencil.DepthPass);
			if (state.Culling.Enabled)
				api.EnableCulling();
			else
				api.DisableCulling();
			api.SetCullFace(state.Culling.Face);
			api.SetLineWidth(state.LineWidth.Width);
			api.SetPolygonMode(state.PolygonMode.Face, state.PolygonMode.Mode);
			if (state.Scissor.Enabled)
				api.EnableScissorTest();
			else
				api.DisableScissorTest();
			api.SetScissorBox(state.Scissor.X, state.Scissor.Y, state.Scissor.Width, state.Scissor.Height);
			api.SetColorMask(state.ColorMask.Red, state.ColorMask.Green, state.ColorMask.Blue, state.ColorMask.Alpha);
			api.SetPolygonOffset(state.PolygonOffset.Enabled ? state.PolygonOffset.Factor : 0.0f, state.PolygonOffset.Enabled ? state.PolygonOffset.Units : 0.0f);
			if (state.Multisampling.Enabled) api.EnableMultisampling(); else api.DisableMultisampling();
		}
		
		if (u32 shaderID = cmd->shader->GetRendererID(); s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		
		// Update model matrix UBO
		if (s_Data.ModelMatrixUBO)
		{
			ShaderBindingLayout::ModelUBO modelData;
			modelData.Model = cmd->transform;
			modelData.Normal = glm::transpose(glm::inverse(cmd->transform));
			
			constexpr u32 expectedSize = ShaderBindingLayout::ModelUBO::GetSize();
			static_assert(sizeof(ShaderBindingLayout::ModelUBO) == expectedSize, "ModelUBO size mismatch");
			
			s_Data.ModelMatrixUBO->SetData(&modelData, expectedSize);
			glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_MODEL, s_Data.ModelMatrixUBO->GetRendererID());
		}
		
		cmd->texture->Bind(ShaderBindingLayout::TEX_DIFFUSE);
		s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->texture->GetRendererID();
		s_Data.Stats.TextureBinds++;
		
		cmd->quadVA->Bind();
		
		s_Data.Stats.DrawCalls++;
		
		api.DrawIndexed(cmd->quadVA, 6);
	}
}
