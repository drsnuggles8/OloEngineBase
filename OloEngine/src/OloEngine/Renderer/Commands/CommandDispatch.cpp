#include "OloEnginePCH.h"
#include "CommandDispatch.h"
#include "RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Light.h"

namespace OloEngine
{	struct CommandDispatchData
	{
		// Existing UBO references
		Ref<UniformBuffer> TransformUBO = nullptr;
		Ref<UniformBuffer> MaterialUBO = nullptr;
		Ref<UniformBuffer> TextureFlagUBO = nullptr;
		Ref<UniformBuffer> CameraUBO = nullptr;
		Ref<UniformBuffer> LightUBO = nullptr;
		Ref<UniformBuffer> BoneMatricesUBO = nullptr;
		Ref<UniformBuffer> ModelMatrixUBO = nullptr;
		// Cached matrices and light data
		glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
		glm::mat4 ViewMatrix = glm::mat4(1.0f);
		Light SceneLight;
		glm::vec3 ViewPos = glm::vec3(0.0f);
		
		// State tracking for optimizations
		u32 CurrentBoundShaderID = 0;
		std::array<u32, 32> BoundTextureIDs = { 0 };
		
		// Registry tracking for shader resources
		std::unordered_map<u32, ShaderResourceRegistry*> ShaderRegistries;
		
		// Statistics for performance monitoring
		CommandDispatch::Statistics Stats;
	};

	static CommandDispatchData s_Data;	// Add this to SetSharedUBOs function
	void CommandDispatch::SetSharedUBOs(
		const Ref<UniformBuffer>& transformUBO,
		const Ref<UniformBuffer>& materialUBO,
		const Ref<UniformBuffer>& textureFlagUBO,
		const Ref<UniformBuffer>& cameraUBO,
		const Ref<UniformBuffer>& lightUBO,
		const Ref<UniformBuffer>& boneMatricesUBO,
		const Ref<UniformBuffer>& modelMatrixUBO)
	{
		s_Data.TransformUBO = transformUBO;
		s_Data.MaterialUBO = materialUBO;
		s_Data.TextureFlagUBO = textureFlagUBO;
		s_Data.CameraUBO = cameraUBO;
		s_Data.LightUBO = lightUBO;
		s_Data.BoneMatricesUBO = boneMatricesUBO;
		s_Data.ModelMatrixUBO = modelMatrixUBO;
		
		OLO_CORE_INFO("CommandDispatch: Shared UBOs configured (including bone and model matrices)");
	}

	void CommandDispatch::SetSceneLight(const Light& light)
	{
		s_Data.SceneLight = light;
	}

	void CommandDispatch::SetViewPosition(const glm::vec3& viewPos)
	{
		s_Data.ViewPos = viewPos;
	}
	void CommandDispatch::UpdateLightPropertiesUBO(const Light& light, const glm::vec3& viewPos)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.LightUBO)
		{
			OLO_CORE_WARN("CommandDispatch::UpdateLightPropertiesUBO: LightUBO not initialized");
			return;
		}
		
		// Use standardized light UBO structure (without material properties)
		ShaderBindingLayout::LightUBO lightData;

		auto lightType = std::to_underlying(light.Type);
		lightData.LightPosition = glm::vec4(light.Position, 1.0f);
		lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
		lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
		lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
		lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);

		lightData.LightAttParams = glm::vec4(
			light.Constant,
			light.Linear,
			light.Quadratic,
			0.0f
		);

		lightData.LightSpotParams = glm::vec4(
			light.CutOff,
			light.OuterCutOff,
			0.0f,
			0.0f
		);

		lightData.ViewPosAndLightType = glm::vec4(viewPos, static_cast<f32>(lightType));

		s_Data.LightUBO->SetData(&lightData, sizeof(ShaderBindingLayout::LightUBO));
	}

	CommandDispatch::Statistics& CommandDispatch::GetStatistics()
	{
		return s_Data.Stats;
	}

	void CommandDispatch::SetViewProjectionMatrix(const glm::mat4& viewProjection)
    {
        s_Data.ViewProjectionMatrix = viewProjection;
    }

	void CommandDispatch::SetViewMatrix(const glm::mat4& view)
    {
        s_Data.ViewMatrix = view;
    }

	// UBO update methods that use the shared UBOs - Updated for standardized binding layout
    void CommandDispatch::UpdateTransformUBO(const glm::mat4& modelMatrix)
    {
        // This function is deprecated - use UpdateModelMatrixUBO instead
        UpdateModelMatrixUBO(modelMatrix);
    }
    
    void CommandDispatch::UpdateModelMatrixUBO(const glm::mat4& modelMatrix)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Data.ModelMatrixUBO)
        {
            OLO_CORE_WARN("CommandDispatch::UpdateModelMatrixUBO: ModelMatrixUBO not initialized");
            return;
        }
        
        // Use standardized model UBO structure
        ShaderBindingLayout::ModelUBO modelData;
        modelData.Model = modelMatrix;
        modelData.Normal = glm::transpose(glm::inverse(modelMatrix));  // Pre-calculate normal matrix
        
        s_Data.ModelMatrixUBO->SetData(&modelData, sizeof(ShaderBindingLayout::ModelUBO));
    }
    
    void CommandDispatch::UpdateMaterialUBO(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& specular, f32 shininess)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Data.MaterialUBO)
        {
            OLO_CORE_WARN("CommandDispatch::UpdateMaterialUBO: MaterialUBO not initialized");
            return;
        }
        
        // Use standardized material UBO structure
        ShaderBindingLayout::MaterialUBO materialData;
        materialData.Ambient = glm::vec4(ambient, 1.0f);
        materialData.Diffuse = glm::vec4(diffuse, 1.0f);
        materialData.Specular = glm::vec4(specular, shininess);  // shininess in w component
        materialData.Emissive = glm::vec4(0.0f);  // Default emissive
        materialData.UseTextureMaps = 0;  // Will be set by UpdateMaterialTextureFlag
        materialData._padding[0] = materialData._padding[1] = materialData._padding[2] = 0;
        
        s_Data.MaterialUBO->SetData(&materialData, sizeof(ShaderBindingLayout::MaterialUBO));
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
        size_t offset = offsetof(ShaderBindingLayout::MaterialUBO, UseTextureMaps);
        s_Data.MaterialUBO->SetData(&flag, sizeof(i32), static_cast<u32>(offset));
    }
    
    void CommandDispatch::UpdateTextureFlag(bool useTextures)
    {
        // Deprecated - use UpdateMaterialTextureFlag instead
        UpdateMaterialTextureFlag(useTextures);
    }

    // Array of dispatch functions indexed by CommandType
    static CommandDispatchFn s_DispatchTable[static_cast<sizet>(CommandType::SetMultisampling) + 1] = { nullptr };

      // Initialize all command dispatch functions
    void CommandDispatch::Initialize()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing CommandDispatch system.");

		s_Data.CurrentBoundShaderID = 0;
		std::fill(s_Data.BoundTextureIDs.begin(), s_Data.BoundTextureIDs.end(), 0);
		s_Data.Stats.Reset();
        
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
        OLO_CORE_INFO("Registering DrawMesh at index {}", static_cast<sizet>(CommandType::DrawMesh));
        s_DispatchTable[static_cast<sizet>(CommandType::DrawMesh)] = CommandDispatch::DrawMesh;
        OLO_CORE_INFO("Registering DrawMeshInstanced at index {}", static_cast<sizet>(CommandType::DrawMeshInstanced));
        s_DispatchTable[static_cast<sizet>(CommandType::DrawMeshInstanced)] = CommandDispatch::DrawMeshInstanced;
        OLO_CORE_INFO("Registering DrawSkinnedMesh at index {}", static_cast<sizet>(CommandType::DrawSkinnedMesh));
        s_DispatchTable[static_cast<sizet>(CommandType::DrawSkinnedMesh)] = CommandDispatch::DrawSkinnedMesh;
        OLO_CORE_INFO("Registering DrawQuad at index {}", static_cast<sizet>(CommandType::DrawQuad));
        s_DispatchTable[static_cast<sizet>(CommandType::DrawQuad)] = CommandDispatch::DrawQuad;
        
        OLO_CORE_INFO("CommandDispatch system initialized with {} dispatch functions", static_cast<sizet>(CommandType::SetMultisampling) + 1);
    }
    
    // Get the dispatch function for a command type
    CommandDispatchFn CommandDispatch::GetDispatchFunction(CommandType type)
    {
        if (type == CommandType::Invalid || static_cast<sizet>(type) >= sizeof(s_DispatchTable) / sizeof(CommandDispatchFn))
        {
            OLO_CORE_ERROR("CommandDispatch::GetDispatchFunction: Invalid command type {}", static_cast<int>(type));
            return nullptr;
        }
        
        return s_DispatchTable[static_cast<sizet>(type)];
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
    
    // Draw commands dispatch functions
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
        
        auto* registry = GetShaderRegistry(cmd->shaderID);
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
		
		api.DrawLines(cmd->vertexArray, cmd->vertexCount);	}
    
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
		
		// Update UBOs according to standardized binding layout
		UpdateModelMatrixUBO(cmd->transform);
		
		// Update material properties next
		struct MaterialData
		{
			glm::vec4 Ambient;    // vec3 aligned to vec4
			glm::vec4 Diffuse;    // vec3 aligned to vec4
			glm::vec4 Specular;   // vec3 aligned to vec4
			float Shininess;
			float _pad[3];        // Padding for alignment
		};
		
		MaterialData materialData{
			glm::vec4(cmd->ambient, 1.0f),
			glm::vec4(cmd->diffuse, 1.0f),
			glm::vec4(cmd->specular, 1.0f),
			cmd->shininess,
			{0.0f, 0.0f, 0.0f}
		};
		
		if (s_Data.MaterialUBO)
		{
			s_Data.MaterialUBO->SetData(&materialData, sizeof(MaterialData));
		}
		
		// Update texture flag
		int useTextureMaps = cmd->useTextureMaps ? 1 : 0;
		if (s_Data.TextureFlagUBO)
		{
			s_Data.TextureFlagUBO->SetData(&useTextureMaps, sizeof(int));
		}
		
		// Update light properties with the specific material
		struct LightPropertiesData
		{
			glm::vec4 MaterialAmbient;
			glm::vec4 MaterialDiffuse;
			glm::vec4 MaterialSpecular;
			glm::vec4 Padding1;

			glm::vec4 LightPosition;
			glm::vec4 LightDirection;
			glm::vec4 LightAmbient;
			glm::vec4 LightDiffuse;
			glm::vec4 LightSpecular;
			glm::vec4 LightAttParams;
			glm::vec4 LightSpotParams;

			glm::vec4 ViewPosAndLightType;
		};

		LightPropertiesData lightData;

		lightData.MaterialAmbient = glm::vec4(cmd->ambient, 0.0f);
		lightData.MaterialDiffuse = glm::vec4(cmd->diffuse, 0.0f);
		lightData.MaterialSpecular = glm::vec4(cmd->specular, cmd->shininess);
		lightData.Padding1 = glm::vec4(0.0f);

		const Light& light = s_Data.SceneLight;
		auto lightType = std::to_underlying(light.Type);
		lightData.LightPosition = glm::vec4(light.Position, 1.0f); // Use 1.0 for w to indicate position
		lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
		lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
		lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
		lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);

		lightData.LightAttParams = glm::vec4(
			s_Data.SceneLight.Constant,
			s_Data.SceneLight.Linear,
			s_Data.SceneLight.Quadratic,
			0.0f
		);

		lightData.LightSpotParams = glm::vec4(
			s_Data.SceneLight.CutOff,
			s_Data.SceneLight.OuterCutOff,
			0.0f,
			0.0f
		);

		lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(lightType));

		if (s_Data.LightUBO)
		{
			s_Data.LightUBO->SetData(&lightData, sizeof(LightPropertiesData));
		}
		
		// Efficiently bind textures if needed
		if (cmd->useTextureMaps)
		{
			// Bind textures to standardized binding points (no SetInt needed with layout bindings)
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
		
		// Update material UBOs
		UpdateMaterialUBO(cmd->ambient, cmd->diffuse, cmd->specular, cmd->shininess);
		UpdateMaterialTextureFlag(cmd->useTextureMaps);
		
		// For instanced rendering, we'll set model matrices as shader uniforms
		// A proper implementation would use an instance buffer or SSBO for better performance
		// This is a simplified approach for now
		const sizet maxInstances = 100; // Limit for uniform-based instancing
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
		
		// Bind textures with state tracking
		if (cmd->useTextureMaps)
		{
			// Bind textures to standardized binding points
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

		// DEBUG: Log mesh transform position
		glm::vec3 meshPosition = glm::vec3(cmd->modelMatrix[3]);
		OLO_CORE_INFO("DrawSkinnedMesh - Mesh position: ({:.2f}, {:.2f}, {:.2f})", meshPosition.x, meshPosition.y, meshPosition.z);

		// Apply render state if provided (same as regular mesh)
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
		
		// Bind shader (same pattern as regular mesh)
		if (u32 shaderID = cmd->shader->GetRendererID(); s_Data.CurrentBoundShaderID != shaderID)
		{
			cmd->shader->Bind();
			s_Data.CurrentBoundShaderID = shaderID;
			s_Data.Stats.ShaderBinds++;
		}
		// Update camera matrices UBO (ViewProjection and View at binding 0)
		// Note: Skinned shader expects binding 0 to have ViewProjection + View, not ViewProjection + Model
		struct CameraMatrices
		{
			glm::mat4 ViewProjection;
			glm::mat4 View;
		};
		CameraMatrices cameraMatrices;
		cameraMatrices.ViewProjection = s_Data.ViewProjectionMatrix;
		cameraMatrices.View = s_Data.ViewMatrix;
		
		// Use TransformUBO (binding 0) instead of CameraUBO (binding 3) for skinned meshes
		// This ensures the skinned shader gets the right data at binding 0
		if (s_Data.TransformUBO)
		{
			s_Data.TransformUBO->SetData(&cameraMatrices, sizeof(CameraMatrices));
		}
		
		// Update model matrix UBO using standardized structure
		UpdateModelMatrixUBO(cmd->modelMatrix);
		
		// Update material properties (same as regular mesh)
		struct MaterialData
		{
			glm::vec4 Ambient;
			glm::vec4 Diffuse;
			glm::vec4 Specular;
			float Shininess;
			float _pad[3];
		};
		
		MaterialData materialData{
			glm::vec4(cmd->ambient, 1.0f),
			glm::vec4(cmd->diffuse, 1.0f),
			glm::vec4(cmd->specular, 1.0f),
			cmd->shininess,
			{0.0f, 0.0f, 0.0f}
		};
		
		if (s_Data.MaterialUBO)
		{
			s_Data.MaterialUBO->SetData(&materialData, sizeof(MaterialData));
		}
		
		// Update texture flag
		int useTextureMaps = cmd->useTextureMaps ? 1 : 0;
		if (s_Data.TextureFlagUBO)
		{
			s_Data.TextureFlagUBO->SetData(&useTextureMaps, sizeof(int));
		}
		
		// **NEW: Update bone matrices UBO for GPU skinning**
		if (s_Data.BoneMatricesUBO && !cmd->boneMatrices.empty())
		{
			// Ensure we don't exceed max bone count (typically 100 bones)
			constexpr sizet MAX_BONES = 100;
			sizet boneCount = glm::min(cmd->boneMatrices.size(), MAX_BONES);
			
			// Upload bone matrices to GPU
			s_Data.BoneMatricesUBO->SetData(cmd->boneMatrices.data(), boneCount * sizeof(glm::mat4));
		}
		
		// Update light properties (same pattern as regular mesh)
		struct LightPropertiesData
		{
			glm::vec4 MaterialAmbient;
			glm::vec4 MaterialDiffuse;
			glm::vec4 MaterialSpecular;
			glm::vec4 Padding1;

			glm::vec4 LightPosition;
			glm::vec4 LightDirection;
			glm::vec4 LightAmbient;
			glm::vec4 LightDiffuse;
			glm::vec4 LightSpecular;
			glm::vec4 LightAttParams;
			glm::vec4 LightSpotParams;

			glm::vec4 ViewPosAndLightType;
		};

		LightPropertiesData lightData;
		lightData.MaterialAmbient = glm::vec4(cmd->ambient, 0.0f);
		lightData.MaterialDiffuse = glm::vec4(cmd->diffuse, 0.0f);
		lightData.MaterialSpecular = glm::vec4(cmd->specular, cmd->shininess);
		lightData.Padding1 = glm::vec4(0.0f);

		const Light& light = s_Data.SceneLight;
		auto lightType = std::to_underlying(light.Type);
		lightData.LightPosition = glm::vec4(light.Position, 1.0f);
		lightData.LightDirection = glm::vec4(light.Direction, 0.0f);
		lightData.LightAmbient = glm::vec4(light.Ambient, 0.0f);
		lightData.LightDiffuse = glm::vec4(light.Diffuse, 0.0f);
		lightData.LightSpecular = glm::vec4(light.Specular, 0.0f);
		lightData.LightAttParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, 0.0f);
		lightData.LightSpotParams = glm::vec4(light.CutOff, light.OuterCutOff, 0.0f, 0.0f);
		lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(lightType));

		// DEBUG: Log lighting values for skinned mesh
		OLO_CORE_INFO("DrawSkinnedMesh - Light Debug:");
		OLO_CORE_INFO("  Material Ambient: ({:.2f}, {:.2f}, {:.2f})", lightData.MaterialAmbient.x, lightData.MaterialAmbient.y, lightData.MaterialAmbient.z);
		OLO_CORE_INFO("  Material Diffuse: ({:.2f}, {:.2f}, {:.2f})", lightData.MaterialDiffuse.x, lightData.MaterialDiffuse.y, lightData.MaterialDiffuse.z);
		OLO_CORE_INFO("  Light Ambient: ({:.2f}, {:.2f}, {:.2f})", lightData.LightAmbient.x, lightData.LightAmbient.y, lightData.LightAmbient.z);
		OLO_CORE_INFO("  Light Diffuse: ({:.2f}, {:.2f}, {:.2f})", lightData.LightDiffuse.x, lightData.LightDiffuse.y, lightData.LightDiffuse.z);
		OLO_CORE_INFO("  Light Direction: ({:.2f}, {:.2f}, {:.2f})", lightData.LightDirection.x, lightData.LightDirection.y, lightData.LightDirection.z);
		OLO_CORE_INFO("  View Position: ({:.2f}, {:.2f}, {:.2f})", lightData.ViewPosAndLightType.x, lightData.ViewPosAndLightType.y, lightData.ViewPosAndLightType.z);
		OLO_CORE_INFO("  Light Type: {}", lightData.ViewPosAndLightType.w);

		if (s_Data.LightUBO)
		{
			s_Data.LightUBO->SetData(&lightData, sizeof(LightPropertiesData));
		}
		
		// Bind textures for skinned mesh (using correct binding points)
		if (cmd->useTextureMaps)
		{
			OLO_CORE_INFO("DrawSkinnedMesh - Binding textures for skinned mesh");
			if (cmd->diffuseMap)
			{
				u32 texID = cmd->diffuseMap->GetRendererID();
				// Skinned shader expects diffuse at binding 3
				if (s_Data.BoundTextureIDs[3] != texID)
				{
					cmd->diffuseMap->Bind(3);
					// Don't set uniform - shader uses layout(binding = 3)
					s_Data.BoundTextureIDs[3] = texID;
					s_Data.Stats.TextureBinds++;
					OLO_CORE_INFO("DrawSkinnedMesh - Bound diffuse texture {} to slot 3", texID);
				}
			}
			
			if (cmd->specularMap)
			{
				u32 texID = cmd->specularMap->GetRendererID();
				// Skinned shader expects specular at binding 4
				if (s_Data.BoundTextureIDs[4] != texID)
				{
					cmd->specularMap->Bind(4);
					// Don't set uniform - shader uses layout(binding = 4)
					s_Data.BoundTextureIDs[4] = texID;
					s_Data.Stats.TextureBinds++;
					OLO_CORE_INFO("DrawSkinnedMesh - Bound specular texture {} to slot 4", texID);
				}
			}
		}
		
		// Get index count
		u32 indexCount = cmd->indexCount > 0 ? cmd->indexCount : 
			cmd->vertexArray->GetIndexBuffer() ? cmd->vertexArray->GetIndexBuffer()->GetCount() : 0;
		
		if (indexCount == 0)
		{
			OLO_CORE_ERROR("CommandDispatch::DrawSkinnedMesh: No indices to draw");
			return;
		}
		
		// DEBUG: Log before GPU draw call
		OLO_CORE_INFO("DrawSkinnedMesh - About to call GPU DrawIndexed with {} indices", indexCount);
		
		// Track statistics
		s_Data.Stats.DrawCalls++;
		
		// Issue the draw call (same as regular indexed mesh)
		api.DrawIndexed(cmd->vertexArray, indexCount);
		
		// DEBUG: Log after GPU draw call
		OLO_CORE_INFO("DrawSkinnedMesh - GPU DrawIndexed call completed");
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
		
		// Update UBOs according to standardized binding layout
		UpdateModelMatrixUBO(cmd->transform);
		
		// Bind texture to standardized binding point (no SetInt needed with layout binding)
		cmd->texture->Bind(ShaderBindingLayout::TEX_DIFFUSE);
		s_Data.BoundTextureIDs[ShaderBindingLayout::TEX_DIFFUSE] = cmd->texture->GetRendererID();
		s_Data.Stats.TextureBinds++;
		
		// Make sure the vertex array is bound
		cmd->quadVA->Bind();
		
		// Track draw calls for statistics
		s_Data.Stats.DrawCalls++;
		
		// Draw the quad with explicit 6 indices (two triangles)
		api.DrawIndexed(cmd->quadVA, 6);
	}

	void CommandDispatch::ResetState()
	{
		s_Data.CurrentBoundShaderID = 0;
        std::ranges::fill(s_Data.BoundTextureIDs, 0);
		s_Data.Stats.Reset();
	}

	// Registry management for shader resources
	ShaderResourceRegistry* CommandDispatch::GetShaderRegistry(u32 shaderID)
	{
		auto it = s_Data.ShaderRegistries.find(shaderID);
		return it != s_Data.ShaderRegistries.end() ? it->second : nullptr;
	}

	void CommandDispatch::RegisterShaderRegistry(u32 shaderID, ShaderResourceRegistry* registry)
	{
		if (registry)
		{
			s_Data.ShaderRegistries[shaderID] = registry;
			OLO_CORE_TRACE("Registered shader registry for shader ID: {0}", shaderID);
		}
	}

	void CommandDispatch::UnregisterShaderRegistry(u32 shaderID)
	{
		auto it = s_Data.ShaderRegistries.find(shaderID);
		if (it != s_Data.ShaderRegistries.end())
		{
			s_Data.ShaderRegistries.erase(it);
			OLO_CORE_TRACE("Unregistered shader registry for shader ID: {0}", shaderID);
		}
	}

	const std::unordered_map<u32, ShaderResourceRegistry*>& CommandDispatch::GetShaderRegistries()
	{
		return s_Data.ShaderRegistries;
	}

	void CommandDispatch::ApplyResourceBindings(u32 shaderID)
	{
		auto* registry = GetShaderRegistry(shaderID);
		if (registry)
		{
			registry->ApplyBindings();
		}
	}
}
