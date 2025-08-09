#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Animation/Skeleton.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>

namespace OloEngine
{
	static bool s_ForceDisableCulling = false;

	Renderer3D::Renderer3DData Renderer3D::s_Data;
	ShaderLibrary Renderer3D::m_ShaderLibrary;
	
	void Renderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Initializing Renderer3D.");

		CommandMemoryManager::Init();

		CommandDispatch::Initialize();
		OLO_CORE_INFO("CommandDispatch system initialized.");

		s_Data.CubeMesh = MeshPrimitives::CreateCube();
		s_Data.QuadMesh = MeshPrimitives::CreatePlane(1.0f, 1.0f);
		s_Data.SkyboxMesh = MeshPrimitives::CreateSkyboxCube();
		// Cached unit line quad (length 1 along +X, centered on X with half-thickness of 0.5 on Y)
		// We'll scale/rotate/translate this via a transform in DrawLine.
		{
			std::vector<Vertex> verts;
			verts.reserve(4);
			// Define a unit line along +X from 0 to 1, quad thickness 1 in Y (will be scaled by desired thickness)
			verts.emplace_back(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f));
			verts.emplace_back(glm::vec3(0.0f,  0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(1.0f, 0.0f));
			verts.emplace_back(glm::vec3(1.0f,  0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(1.0f));
			verts.emplace_back(glm::vec3(1.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f, 1.0f));

			std::vector<u32> inds = { 0, 1, 2, 2, 3, 0 };

			auto src = Ref<MeshSource>::Create(verts, inds);
			Submesh sm;
			sm.m_BaseVertex = 0; sm.m_BaseIndex = 0; sm.m_IndexCount = (u32)inds.size(); sm.m_VertexCount = (u32)verts.size();
			sm.m_MaterialIndex = 0; sm.m_IsRigged = false; sm.m_NodeName = "LineQuad";
			src->AddSubmesh(sm);
			src->Build();
			s_Data.LineQuadMesh = Ref<Mesh>::Create(src, 0);
		}
		m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
		m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");
		m_ShaderLibrary.Load("assets/shaders/SkinnedLighting3D_Simple.glsl");
		m_ShaderLibrary.Load("assets/shaders/Renderer3D_Quad.glsl");
		m_ShaderLibrary.Load("assets/shaders/PBR.glsl");
		m_ShaderLibrary.Load("assets/shaders/PBR_Skinned.glsl");
		m_ShaderLibrary.Load("assets/shaders/PBR_MultiLight.glsl");
		m_ShaderLibrary.Load("assets/shaders/PBR_MultiLight_Skinned.glsl");
		m_ShaderLibrary.Load("assets/shaders/EquirectangularToCubemap.glsl");
		m_ShaderLibrary.Load("assets/shaders/IrradianceConvolution.glsl");
		m_ShaderLibrary.Load("assets/shaders/IBLPrefilter.glsl");
		m_ShaderLibrary.Load("assets/shaders/BRDFLutGeneration.glsl");
		m_ShaderLibrary.Load("assets/shaders/Skybox.glsl");
		
		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");
		s_Data.SkinnedLightingShader = m_ShaderLibrary.Get("SkinnedLighting3D_Simple");
		s_Data.QuadShader = m_ShaderLibrary.Get("Renderer3D_Quad");
		s_Data.PBRShader = m_ShaderLibrary.Get("PBR");
		s_Data.PBRSkinnedShader = m_ShaderLibrary.Get("PBR_Skinned");
		s_Data.PBRMultiLightShader = m_ShaderLibrary.Get("PBR_MultiLight");
		s_Data.PBRMultiLightSkinnedShader = m_ShaderLibrary.Get("PBR_MultiLight_Skinned");
		s_Data.SkyboxShader = m_ShaderLibrary.Get("Skybox");
		
		s_Data.CameraUBO = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
		s_Data.LightPropertiesUBO = UniformBuffer::Create(ShaderBindingLayout::LightUBO::GetSize(), ShaderBindingLayout::UBO_LIGHTS);
		s_Data.MaterialUBO = UniformBuffer::Create(ShaderBindingLayout::MaterialUBO::GetSize(), ShaderBindingLayout::UBO_MATERIAL);
		s_Data.MultiLightBuffer = UniformBuffer::Create(ShaderBindingLayout::MultiLightUBO::GetSize(), ShaderBindingLayout::UBO_MULTI_LIGHTS);
		s_Data.ModelMatrixUBO = UniformBuffer::Create(ShaderBindingLayout::ModelUBO::GetSize(), ShaderBindingLayout::UBO_MODEL);
		s_Data.BoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION);
		
		CommandDispatch::SetUBOReferences(
			s_Data.CameraUBO,
			s_Data.MaterialUBO, 
			s_Data.LightPropertiesUBO,
			s_Data.BoneMatricesUBO,
			s_Data.ModelMatrixUBO
		);
		
		EnvironmentMap::InitializeIBLSystem(m_ShaderLibrary);
		OLO_CORE_INFO("IBL system initialized.");
		
		s_Data.SceneLight.Type = LightType::Directional;
		s_Data.SceneLight.Position = glm::vec3(1.2f, 1.0f, 2.0f);
		s_Data.SceneLight.Direction = glm::vec3(-0.2f, -1.0f, -0.3f);
		s_Data.SceneLight.Ambient = glm::vec3(0.2f, 0.2f, 0.2f);
		s_Data.SceneLight.Diffuse = glm::vec3(0.5f, 0.5f, 0.5f);
		s_Data.SceneLight.Specular = glm::vec3(1.0f, 1.0f, 1.0f);
		s_Data.SceneLight.Constant = 1.0f;
		s_Data.SceneLight.Linear = 0.09f;
		s_Data.SceneLight.Quadratic = 0.032f;

		s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);
		
		s_Data.Stats.Reset();
		
		Window& window = Application::Get().GetWindow();
		s_Data.RGraph = Ref<RenderGraph>::Create();
		SetupRenderGraph(window.GetFramebufferWidth(), window.GetFramebufferHeight());		
		OLO_CORE_INFO("Renderer3D initialization complete.");
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Shutting down Renderer3D.");
		
		// Clear shader registries
		s_Data.ShaderRegistries.clear();
		
		if (s_Data.RGraph)
		 s_Data.RGraph->Shutdown();
		
		OLO_CORE_INFO("Renderer3D shutdown complete.");
	}

	void Renderer3D::BeginScene(const PerspectiveCamera& camera)
	{
		OLO_PROFILE_FUNCTION();
		
		RendererProfiler::GetInstance().BeginFrame();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::BeginScene: ScenePass is null!");
			return;
		}

		CommandAllocator* frameAllocator = CommandMemoryManager::GetFrameAllocator();
		s_Data.ScenePass->GetCommandBucket().SetAllocator(frameAllocator);
		s_Data.ViewMatrix = camera.GetView();
		s_Data.ProjectionMatrix = camera.GetProjection();
		s_Data.ViewProjectionMatrix = camera.GetViewProjection();

		CommandDispatch::SetViewProjectionMatrix(s_Data.ViewProjectionMatrix);
		CommandDispatch::SetViewMatrix(s_Data.ViewMatrix);
		CommandDispatch::SetProjectionMatrix(s_Data.ProjectionMatrix);

		s_Data.ViewFrustum.Update(s_Data.ViewProjectionMatrix);
		
		s_Data.Stats.Reset();
		s_Data.CommandCounter = 0;
		
		UpdateCameraMatricesUBO(s_Data.ViewMatrix, s_Data.ProjectionMatrix);
		UpdateLightPropertiesUBO();
		
		CommandDispatch::SetSceneLight(s_Data.SceneLight);
    	CommandDispatch::SetViewPosition(s_Data.ViewPos);
		
		s_Data.ScenePass->ResetCommandBucket();
		
		CommandDispatch::ResetState();
	}
	
	void Renderer3D::EndScene()
	{
		OLO_PROFILE_FUNCTION();
        
        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("Renderer3D::EndScene: Render graph is null!");
            return;
        }
        
        if (s_Data.ScenePass && s_Data.FinalPass)
        {
            s_Data.FinalPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
        }
        auto& profiler = RendererProfiler::GetInstance();
        if (s_Data.ScenePass)
        {
            const auto& commandBucket = s_Data.ScenePass->GetCommandBucket();
            profiler.IncrementCounter(RendererProfiler::MetricType::CommandPackets, static_cast<u32>(commandBucket.GetCommandCount()));
        }
        
        ApplyGlobalResources();
        
		s_Data.RGraph->Execute();

		CommandAllocator* allocator = s_Data.ScenePass->GetCommandBucket().GetAllocator();
		CommandMemoryManager::ReturnAllocator(allocator);
		s_Data.ScenePass->GetCommandBucket().SetAllocator(nullptr);
		
		profiler.EndFrame();
	}

	void Renderer3D::SetLight(const Light& light)
	{
		s_Data.SceneLight = light;
	}

	void Renderer3D::SetViewPosition(const glm::vec3& position)
	{
		s_Data.ViewPos = position;
	}
	
	void Renderer3D::EnableFrustumCulling(bool enable)
	{
		s_Data.FrustumCullingEnabled = enable;
	}
	
	bool Renderer3D::IsFrustumCullingEnabled()
	{
		if (s_ForceDisableCulling) return false;
		return s_Data.FrustumCullingEnabled;
	}
	
	void Renderer3D::EnableDynamicCulling(bool enable)
	{
		s_Data.DynamicCullingEnabled = enable;
	}
	
	bool Renderer3D::IsDynamicCullingEnabled()
	{
		if (s_ForceDisableCulling) return false;
		return s_Data.DynamicCullingEnabled;
	}
	
	const Frustum& Renderer3D::GetViewFrustum()
	{
		return s_Data.ViewFrustum;
	}
	
	void Renderer3D::SetForceDisableCulling(bool disable)
	{
		s_ForceDisableCulling = disable;
		if (disable)
		{
			EnableFrustumCulling(false);
			EnableDynamicCulling(false);
			OLO_CORE_WARN("Renderer3D: All culling forcibly disabled for debugging!");
		}
	}

	bool Renderer3D::IsForceDisableCulling()
	{
		return s_ForceDisableCulling;
	}
	
	Renderer3D::Statistics Renderer3D::GetStats()
	{
		return s_Data.Stats;
	}
	
	void Renderer3D::ResetStats()
	{
		s_Data.Stats.Reset();
	}

	bool Renderer3D::IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform)
	{
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		BoundingSphere sphere = mesh->GetTransformedBoundingSphere(transform);
		sphere.Radius *= 1.3f;
		
		return s_Data.ViewFrustum.IsBoundingSphereVisible(sphere);
	}

	bool Renderer3D::IsVisibleInFrustum(const BoundingSphere& sphere)
	{
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		BoundingSphere expandedSphere = sphere;
		expandedSphere.Radius *= 1.3f;
		
		return s_Data.ViewFrustum.IsBoundingSphereVisible(expandedSphere);
	}
	
	bool Renderer3D::IsVisibleInFrustum(const BoundingBox& box)
	{
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		return s_Data.ViewFrustum.IsBoundingBoxVisible(box);
	}

	CommandPacket* Renderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawMesh: ScenePass is null!");
			return nullptr;
		}
		s_Data.Stats.TotalMeshes++;
		
		if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
		{
			if (!IsVisibleInFrustum(mesh, modelMatrix))
			{
				s_Data.Stats.CulledMeshes++;
				return nullptr;
			}
		}
		if (!mesh || !mesh->GetVertexArray())
		{
			OLO_CORE_ERROR("Renderer3D::DrawMesh: Invalid mesh or vertex array!");
			return nullptr;
		}
		
		Ref<Shader> shaderToUse;
		if (material.Shader)
		{
			shaderToUse = material.Shader;
		}
		else if (material.Type == MaterialType::PBR)
		{
			shaderToUse = s_Data.PBRShader;
		}
		else
		{
			shaderToUse = s_Data.LightingShader;
		}
		
		if (!shaderToUse)
		{
			OLO_CORE_ERROR("Renderer3D::DrawMesh: No shader available!");
			return nullptr;
		}
		
		// TODO: Asset Management Integration
		// This entire command creation section needs to be updated once we have
		// a proper asset management system. All Ref<T> assignments should be
		// converted to asset ID assignments using asset handles.
		// Example: cmd->shaderID = shaderToUse->GetAssetHandle();
		// instead of: cmd->shader = shaderToUse;
		
		CommandPacket* packet = CreateDrawCall<DrawMeshCommand>();
		auto* cmd = packet->GetCommandData<DrawMeshCommand>();
		cmd->header.type = CommandType::DrawMesh;
		cmd->mesh = mesh;
		cmd->vertexArray = mesh->GetVertexArray();
		cmd->indexCount = mesh->GetIndexCount();
		cmd->transform = glm::mat4(modelMatrix);
		
		// Legacy material properties (for backward compatibility)
		cmd->ambient = material.Ambient;
		cmd->diffuse = material.Diffuse;
		cmd->specular = material.Specular;
		cmd->shininess = material.Shininess;
		cmd->useTextureMaps = material.UseTextureMaps;
		cmd->diffuseMap = material.DiffuseMap;
		cmd->specularMap = material.SpecularMap;
		
		// PBR material properties
		cmd->enablePBR = (material.Type == MaterialType::PBR);
		cmd->baseColorFactor = material.BaseColorFactor;
		cmd->emissiveFactor = material.EmissiveFactor;
		cmd->metallicFactor = material.MetallicFactor;
		cmd->roughnessFactor = material.RoughnessFactor;
		cmd->normalScale = material.NormalScale;
		cmd->occlusionStrength = material.OcclusionStrength;
		cmd->enableIBL = material.EnableIBL;
		
		// PBR texture references
		cmd->albedoMap = material.AlbedoMap;
		cmd->metallicRoughnessMap = material.MetallicRoughnessMap;
		cmd->normalMap = material.NormalMap;
		cmd->aoMap = material.AOMap;
		cmd->emissiveMap = material.EmissiveMap;
		cmd->environmentMap = material.EnvironmentMap;
		cmd->irradianceMap = material.IrradianceMap;
		cmd->prefilterMap = material.PrefilterMap;
		cmd->brdfLutMap = material.BRDFLutMap;
		
		cmd->shader = shaderToUse;
		cmd->renderState = Ref<RenderState>::Create();
		packet->SetCommandType(cmd->header.type);
		packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));
		return packet;
	}

	CommandPacket* Renderer3D::DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
	{
		OLO_PROFILE_FUNCTION();
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawQuad: ScenePass is null!");
			return nullptr;
		}
		if (!texture)
		{
			OLO_CORE_ERROR("Renderer3D::DrawQuad: No texture provided!");
			return nullptr;
		}
		if (!s_Data.QuadShader)
		{
			OLO_CORE_ERROR("Renderer3D::DrawQuad: Quad shader is not loaded!");
			return nullptr;
		}
		if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
		{
			OLO_CORE_ERROR("Renderer3D::DrawQuad: Quad mesh or its vertex array is invalid!");
			s_Data.QuadMesh = MeshPrimitives::CreatePlane(1.0f, 1.0f);
			if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
				return nullptr;
		}
		CommandPacket* packet = CreateDrawCall<DrawQuadCommand>();
		auto* cmd = packet->GetCommandData<DrawQuadCommand>();
		cmd->header.type = CommandType::DrawQuad;
		cmd->transform = glm::mat4(modelMatrix);
		cmd->texture = texture;
		cmd->shader = s_Data.QuadShader;
		cmd->quadVA = s_Data.QuadMesh->GetVertexArray();
		cmd->renderState = Ref<RenderState>::Create();
		packet->SetCommandType(cmd->header.type);
		packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));
		return packet;
	}

	CommandPacket* Renderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: ScenePass is null!");
			return nullptr;
		}
		if (transforms.empty())
		{
			OLO_CORE_WARN("Renderer3D::DrawMeshInstanced: No transforms provided");
			return nullptr;
		}
		s_Data.Stats.TotalMeshes += static_cast<u32>(transforms.size());
		
		if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
		{
			if (!IsVisibleInFrustum(mesh, transforms[0]))
			{
				s_Data.Stats.CulledMeshes += static_cast<u32>(transforms.size());
				return nullptr;
			}
		}

		CommandPacket* packet = CreateDrawCall<DrawMeshInstancedCommand>();
		auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();		
		cmd->header.type = CommandType::DrawMeshInstanced;
		cmd->mesh = mesh;
		cmd->vertexArray = mesh->GetVertexArray();
		cmd->indexCount = mesh->GetIndexCount();
		cmd->instanceCount = static_cast<u32>(transforms.size());
		cmd->transforms = transforms;
		
		// Legacy material properties (for backward compatibility)
		cmd->ambient = material.Ambient;
		cmd->diffuse = material.Diffuse;
		cmd->specular = material.Specular;
		cmd->shininess = material.Shininess;
		cmd->useTextureMaps = material.UseTextureMaps;
		cmd->diffuseMap = material.DiffuseMap;
		cmd->specularMap = material.SpecularMap;
		
		// PBR material properties
		cmd->enablePBR = (material.Type == MaterialType::PBR);
		cmd->baseColorFactor = material.BaseColorFactor;
		cmd->emissiveFactor = material.EmissiveFactor;
		cmd->metallicFactor = material.MetallicFactor;
		cmd->roughnessFactor = material.RoughnessFactor;
		cmd->normalScale = material.NormalScale;
		cmd->occlusionStrength = material.OcclusionStrength;
		cmd->enableIBL = material.EnableIBL;
		
		// PBR texture references
		cmd->albedoMap = material.AlbedoMap;
		cmd->metallicRoughnessMap = material.MetallicRoughnessMap;
		cmd->normalMap = material.NormalMap;
		cmd->aoMap = material.AOMap;
		cmd->emissiveMap = material.EmissiveMap;
		cmd->environmentMap = material.EnvironmentMap;
		cmd->irradianceMap = material.IrradianceMap;
		cmd->prefilterMap = material.PrefilterMap;
		cmd->brdfLutMap = material.BRDFLutMap;
		
		cmd->shader = material.Shader ? material.Shader : s_Data.LightingShader;
		cmd->renderState = Ref<RenderState>::Create();
		packet->SetCommandType(cmd->header.type);
		packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));
		return packet;
	}

	CommandPacket* Renderer3D::DrawLightCube(const glm::mat4& modelMatrix)
	{
		OLO_PROFILE_FUNCTION();
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawLightCube: ScenePass is null!");
			return nullptr;
		}
		CommandPacket* packet = CreateDrawCall<DrawMeshCommand>();
		auto* cmd = packet->GetCommandData<DrawMeshCommand>();
		cmd->header.type = CommandType::DrawMesh;
		cmd->mesh = s_Data.CubeMesh;
		cmd->vertexArray = s_Data.CubeMesh->GetVertexArray();
		cmd->indexCount = s_Data.CubeMesh->GetIndexCount();
		cmd->transform = modelMatrix;
		cmd->shader = s_Data.LightCubeShader;
		
		// Legacy material properties
		cmd->ambient = glm::vec3(1.0f);
		cmd->diffuse = glm::vec3(1.0f);
		cmd->specular = glm::vec3(1.0f);
		cmd->shininess = 32.0f;
		cmd->useTextureMaps = false;
		cmd->diffuseMap = nullptr;
		cmd->specularMap = nullptr;
		
		// PBR material properties (default values for light cube)
		cmd->enablePBR = false;
		cmd->baseColorFactor = glm::vec4(1.0f);
		cmd->emissiveFactor = glm::vec4(0.0f);
		cmd->metallicFactor = 0.0f;
		cmd->roughnessFactor = 1.0f;
		cmd->normalScale = 1.0f;
		cmd->occlusionStrength = 1.0f;
		cmd->enableIBL = false;
		cmd->albedoMap = nullptr;
		cmd->metallicRoughnessMap = nullptr;
		cmd->normalMap = nullptr;
		cmd->aoMap = nullptr;
		cmd->emissiveMap = nullptr;
		cmd->environmentMap = nullptr;
		cmd->irradianceMap = nullptr;
		cmd->prefilterMap = nullptr;
		cmd->brdfLutMap = nullptr;
		
		cmd->renderState = Ref<RenderState>::Create();
		packet->SetCommandType(cmd->header.type);
		packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));
		return packet;
	}

	CommandPacket* Renderer3D::DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic)
	{
		return DrawMesh(s_Data.CubeMesh, modelMatrix, material, isStatic);
	}

	void Renderer3D::UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection)
	{
		OLO_PROFILE_FUNCTION();
        
		ShaderBindingLayout::CameraUBO cameraData;
		cameraData.ViewProjection = projection * view;
		cameraData.View = view;
		cameraData.Projection = projection;
		cameraData.Position = s_Data.ViewPos;
		cameraData._padding0 = 0.0f;
		
		constexpr u32 expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
		static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch");
		
		s_Data.CameraUBO->SetData(&cameraData, expectedSize);
	}
	
	void Renderer3D::UpdateLightPropertiesUBO()
	{
		OLO_PROFILE_FUNCTION();
		
		if (s_Data.LightPropertiesUBO)
		{
			ShaderBindingLayout::LightUBO lightData;
			auto lightType = std::to_underlying(s_Data.SceneLight.Type);
			
			lightData.LightPosition = glm::vec4(s_Data.SceneLight.Position, 1.0f);
			lightData.LightDirection = glm::vec4(s_Data.SceneLight.Direction, 0.0f);
			lightData.LightAmbient = glm::vec4(s_Data.SceneLight.Ambient, 0.0f);
			lightData.LightDiffuse = glm::vec4(s_Data.SceneLight.Diffuse, 0.0f);
			lightData.LightSpecular = glm::vec4(s_Data.SceneLight.Specular, 0.0f);
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
			
			s_Data.LightPropertiesUBO->SetData(&lightData, sizeof(ShaderBindingLayout::LightUBO));
		}
	}
	
	void Renderer3D::SetupRenderGraph(u32 width, u32 height)
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Setting up Renderer3D RenderGraph with dimensions: {}x{}", width, height);

		if (width == 0 || height == 0)
		{
			OLO_CORE_WARN("Invalid dimensions for RenderGraph: {}x{}", width, height);
			return;
		}
		
		s_Data.RGraph->Init(width, height);
		
		FramebufferSpecification scenePassSpec;
		scenePassSpec.Width = width;
		scenePassSpec.Height = height;
		scenePassSpec.Samples = 1;
		scenePassSpec.Attachments = {
			FramebufferTextureFormat::RGBA8,
			FramebufferTextureFormat::Depth
		};
		
		FramebufferSpecification finalPassSpec;
		finalPassSpec.Width = width;
		finalPassSpec.Height = height;
		
		s_Data.ScenePass = Ref<SceneRenderPass>::Create();
		s_Data.ScenePass->SetName("ScenePass");
		s_Data.ScenePass->Init(scenePassSpec);
		
		s_Data.FinalPass = Ref<FinalRenderPass>::Create();
		s_Data.FinalPass->SetName("FinalPass");
		s_Data.FinalPass->Init(finalPassSpec);
		
		s_Data.RGraph->AddPass(s_Data.ScenePass);
		s_Data.RGraph->AddPass(s_Data.FinalPass);
		
		s_Data.RGraph->ConnectPass("ScenePass", "FinalPass");
		
		s_Data.FinalPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
		OLO_CORE_INFO("Renderer3D: Connected scene pass framebuffer to final pass input");
		
		s_Data.RGraph->SetFinalPass("FinalPass");
	}
	
	void Renderer3D::OnWindowResize(u32 width, u32 height)
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Renderer3D::OnWindowResize: Resizing to {}x{}", width, height);
		
		if (s_Data.RGraph)
		{
			s_Data.RGraph->Resize(width, height);
		}
		else
		{
			OLO_CORE_WARN("Renderer3D::OnWindowResize: No render graph available!");
		}
	}

	CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: ScenePass is null!");
			return nullptr;
		}
		
		s_Data.Stats.TotalMeshes++;
		
		if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
		{
			if (!IsVisibleInFrustum(mesh, modelMatrix))
			{
				s_Data.Stats.CulledMeshes++;
				return nullptr;
			}
		}
		
		if (!mesh || !mesh->GetMeshSource())
		{
			OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Invalid mesh or mesh source!");
			return nullptr;
		}

		auto meshSource = mesh->GetMeshSource();
		
		static bool s_FirstRun = true;
		if (s_FirstRun)
		{
			OLO_CORE_INFO("Renderer3D::DrawAnimatedMesh: First animated mesh with {} bone influences", meshSource->GetBoneInfluences().size());
			s_FirstRun = false;
		}
		
		if (!meshSource->HasBoneInfluences())
		{
			OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: Mesh has no bone influences (size: {}), falling back to regular mesh rendering", 
						   meshSource->GetBoneInfluences().size());
			return DrawMesh(mesh, modelMatrix, material, isStatic);
		}

		Ref<Shader> shaderToUse;
		if (material.Shader)
		{
			shaderToUse = material.Shader;
		}
		else if (material.Type == MaterialType::PBR)
		{
			shaderToUse = s_Data.PBRSkinnedShader;
		}
		else
		{
			shaderToUse = s_Data.SkinnedLightingShader;
		}
		
		if (!shaderToUse)
		{
			OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: Preferred shader not available, falling back to Lighting3D");
			shaderToUse = s_Data.LightingShader;
		}
		if (!shaderToUse)
		{
			OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: No shader available!");
			return nullptr;
		}
		
		if (boneMatrices.empty())
		{
			OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: No bone matrices provided, using identity matrices");
		}
		
		// Use unified DrawMeshCommand for bone matrix handling
		CommandPacket* packet = CreateDrawCall<DrawMeshCommand>();
		auto* cmd = packet->GetCommandData<DrawMeshCommand>();
		
		cmd->header.type = CommandType::DrawMesh;
		cmd->isAnimatedMesh = true;
		
		cmd->mesh = mesh;
		
		// Check if VAO is valid before proceeding
		auto vertexArray = mesh->GetVertexArray();
		if (!vertexArray)
		{
			OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Mesh has null VAO (Vertex Array Object)!");
			return nullptr;
		}
		cmd->vertexArray = vertexArray;
		
		cmd->indexCount = mesh->GetIndexCount();
		cmd->transform = modelMatrix;
		
		// Legacy material properties (for backward compatibility)
		cmd->ambient = material.Ambient;
		cmd->diffuse = material.Diffuse;
		cmd->specular = material.Specular;
		cmd->shininess = material.Shininess;
		cmd->useTextureMaps = material.UseTextureMaps;
		cmd->diffuseMap = material.DiffuseMap;
		cmd->specularMap = material.SpecularMap;
		
		// PBR material properties
		cmd->enablePBR = (material.Type == MaterialType::PBR);
		cmd->baseColorFactor = material.BaseColorFactor;
		cmd->emissiveFactor = material.EmissiveFactor;
		cmd->metallicFactor = material.MetallicFactor;
		cmd->roughnessFactor = material.RoughnessFactor;
		cmd->normalScale = material.NormalScale;
		cmd->occlusionStrength = material.OcclusionStrength;
		cmd->enableIBL = material.EnableIBL;
		
		// PBR texture references
		cmd->albedoMap = material.AlbedoMap;
		cmd->metallicRoughnessMap = material.MetallicRoughnessMap;
		cmd->normalMap = material.NormalMap;
		cmd->aoMap = material.AOMap;
		cmd->emissiveMap = material.EmissiveMap;
		cmd->environmentMap = material.EnvironmentMap;
		cmd->irradianceMap = material.IrradianceMap;
		cmd->prefilterMap = material.PrefilterMap;
		cmd->brdfLutMap = material.BRDFLutMap;
		
		cmd->shader = shaderToUse;
		cmd->renderState = Ref<RenderState>::Create();
		
		// Set bone matrices reference for GPU skinning (avoid copying large data)
		if (!boneMatrices.empty())
		{
			cmd->boneMatrices = std::span<const glm::mat4>(boneMatrices.data(), boneMatrices.size());
		}
		else
		{
			cmd->boneMatrices = std::span<const glm::mat4>();
		}
		
		static bool s_LoggedBoneMatrices = false;
		if (!s_LoggedBoneMatrices && !boneMatrices.empty())
		{
			OLO_CORE_INFO("DrawAnimatedMesh: Setting {} bone matrices for GPU skinning", boneMatrices.size());
			s_LoggedBoneMatrices = true;
		}
		
		packet->SetCommandType(cmd->header.type);
		packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));
		
		return packet;
	}

	void Renderer3D::ApplyGlobalResources()
	{
		OLO_PROFILE_FUNCTION();
		
		const auto& shaderRegistries = s_Data.ShaderRegistries;
		
		for (const auto& [shaderID, registry] : shaderRegistries)
		{
			if (registry)
			{
				const auto& globalResources = s_Data.GlobalResourceRegistry.GetBoundResources();
				for (const auto& [resourceName, resource] : globalResources)
				{
					if (registry->GetBindingInfo(resourceName) != nullptr)
					{
						ShaderResourceInput input;
						if (std::holds_alternative<Ref<UniformBuffer>>(resource))
						{
							input = ShaderResourceInput(std::get<Ref<UniformBuffer>>(resource));
						}
						else if (std::holds_alternative<Ref<Texture2D>>(resource))
						{
							input = ShaderResourceInput(std::get<Ref<Texture2D>>(resource));
						}
						else if (std::holds_alternative<Ref<TextureCubemap>>(resource))
						{
							input = ShaderResourceInput(std::get<Ref<TextureCubemap>>(resource));
						}
						
						if (input.Type != ShaderResourceType::None)
						{
							registry->SetResource(resourceName, input);
						}
					}
				}
			}
		}
	}

	void Renderer3D::RenderAnimatedMeshes(const Ref<Scene>& scene, const Material& defaultMaterial)
	{
		OLO_PROFILE_FUNCTION();

		static bool s_FirstRun = true;
		if (s_FirstRun)
		{
			OLO_CORE_INFO("Renderer3D::RenderAnimatedMeshes: Starting animated mesh rendering");
			s_FirstRun = false;
		}

		if (!scene)
		{
			OLO_CORE_WARN("Renderer3D::RenderAnimatedMeshes: Scene is null");
			return;
		}
		
		// Get view and optimize with single loop to avoid unnecessary iteration
		auto view = scene->GetAllEntitiesWith<MeshComponent, SkeletonComponent, TransformComponent>();
		static sizet s_EntityCount = 0;
		sizet currentEntityCount = 0;
		
		// Single loop optimization: count and process entities together
		for (auto entityID : view)
		{
			Entity entity = { entityID, scene.get() };
			s_Data.Stats.TotalAnimatedMeshes++;
			currentEntityCount++;

			RenderAnimatedMesh(entity, defaultMaterial, scene.get());
		}
		
		// Log stats only when count changes to reduce logging overhead
		static bool loggedStats = false;
		if (!loggedStats || currentEntityCount != s_EntityCount)
		{
			OLO_CORE_INFO("RenderAnimatedMeshes: Found {} animated entities", currentEntityCount);
			loggedStats = true;
			s_EntityCount = currentEntityCount;
		}
	}

	void Renderer3D::RenderAnimatedMesh(Entity entity, const Material& defaultMaterial, const Scene* scene)
	{
		OLO_PROFILE_FUNCTION();

		if (!entity.HasComponent<MeshComponent>() || 
			!entity.HasComponent<SkeletonComponent>() ||
			!entity.HasComponent<TransformComponent>())
		{
			s_Data.Stats.SkippedAnimatedMeshes++;
			return;
		}

		auto& meshComp = entity.GetComponent<MeshComponent>();
		auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
		auto& transformComp = entity.GetComponent<TransformComponent>();

		if (!meshComp.m_MeshSource || !skeletonComp.m_Skeleton)
		{
			OLO_CORE_WARN("Renderer3D::RenderAnimatedMesh: Entity {} has invalid mesh or skeleton", 
						 entity.GetComponent<TagComponent>().Tag);
			s_Data.Stats.SkippedAnimatedMeshes++;
			return;
		}

		glm::mat4 worldTransform = transformComp.GetTransform();

		// Get bone matrices from the skeleton
		const auto& boneMatrices = skeletonComp.m_Skeleton->m_FinalBoneMatrices;

		// Use MaterialComponent if available, otherwise use default material
		Material material = defaultMaterial;
		if (entity.HasComponent<MaterialComponent>())
		{
			material = entity.GetComponent<MaterialComponent>().m_Material;
		}

		// Find and render all child entities with SubmeshComponent
		bool renderedAnySubmesh = false;
		const auto& relationshipComponent = entity.GetComponent<RelationshipComponent>();
		for (const UUID& childUUID : relationshipComponent.m_Children)
		{
			Entity submeshEntity = scene->TryGetEntityWithUUID(childUUID);
			if (submeshEntity && submeshEntity.HasComponent<SubmeshComponent>())
			{
				auto& submeshComponent = submeshEntity.GetComponent<SubmeshComponent>();
				if (submeshComponent.m_Mesh && submeshComponent.m_Visible)
				{
					// Use MaterialComponent if available on submesh, otherwise use the parent's material
					Material submeshMaterial = material;
					if (submeshEntity.HasComponent<MaterialComponent>())
					{
						submeshMaterial = submeshEntity.GetComponent<MaterialComponent>().m_Material;
					}

					// Use the new MeshSource with bone influences directly
					auto* packet = DrawAnimatedMesh(
						submeshComponent.m_Mesh,
						worldTransform,
						submeshMaterial,
						boneMatrices,
						false
					);

					if (packet)
					{
						SubmitPacket(packet);
						renderedAnySubmesh = true;
					}
				}
			}
		}

		// Fallback: if no submesh entities found, create a mesh from the first submesh
		if (!renderedAnySubmesh)
		{
			if (meshComp.m_MeshSource->GetSubmeshes().size() > 0)
			{
				auto mesh = Ref<Mesh>::Create(meshComp.m_MeshSource, 0);
				
				auto* packet = DrawAnimatedMesh(
					mesh,
					worldTransform,
					material,
					boneMatrices,
					false
				);

				if (packet)
				{
					SubmitPacket(packet);
					renderedAnySubmesh = true;
				}
			}
		}

		if (renderedAnySubmesh)
		{
			s_Data.Stats.RenderedAnimatedMeshes++;
		}
	}

	ShaderResourceRegistry* Renderer3D::GetShaderRegistry(u32 shaderID)
	{
		auto it = s_Data.ShaderRegistries.find(shaderID);
		return it != s_Data.ShaderRegistries.end() ? it->second : nullptr;
	}

	void Renderer3D::RegisterShaderRegistry(u32 shaderID, ShaderResourceRegistry* registry)
	{
		if (registry)
		{
			s_Data.ShaderRegistries[shaderID] = registry;
			OLO_CORE_TRACE("Renderer3D: Registered shader registry for shader ID: {0}", shaderID);
		}
	}

	void Renderer3D::UnregisterShaderRegistry(u32 shaderID)
	{
		auto it = s_Data.ShaderRegistries.find(shaderID);
		if (it != s_Data.ShaderRegistries.end())
		{
			s_Data.ShaderRegistries.erase(it);
			OLO_CORE_TRACE("Renderer3D: Unregistered shader registry for shader ID: {0}", shaderID);
		}
	}

	const std::unordered_map<u32, ShaderResourceRegistry*>& Renderer3D::GetShaderRegistries()
	{
		return s_Data.ShaderRegistries;
	}

	void Renderer3D::ApplyResourceBindings(u32 shaderID)
	{
		auto* registry = GetShaderRegistry(shaderID);
		if (registry)
		{
			registry->ApplyBindings();
		}
	}

	ShaderLibrary& Renderer3D::GetShaderLibrary()
	{
		return m_ShaderLibrary;
	}

	CommandPacket* Renderer3D::DrawSkybox(const Ref<TextureCubemap>& skyboxTexture)
	{
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkybox: ScenePass is null!");
			return nullptr;
		}

		if (!skyboxTexture)
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkybox: Skybox texture is null!");
			return nullptr;
		}

		if (!s_Data.SkyboxMesh || !s_Data.SkyboxShader)
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkybox: Skybox mesh or shader not initialized!");
			return nullptr;
		}

		CommandPacket* packet = CreateDrawCall<DrawSkyboxCommand>();
		auto* cmd = packet->GetCommandData<DrawSkyboxCommand>();
		cmd->header.type = CommandType::DrawSkybox;
		cmd->mesh = s_Data.SkyboxMesh;
		cmd->vertexArray = s_Data.SkyboxMesh->GetVertexArray();
		cmd->indexCount = s_Data.SkyboxMesh->GetIndexCount();
		cmd->transform = glm::mat4(1.0f); // Identity matrix for skybox
		cmd->shader = s_Data.SkyboxShader;
		cmd->skyboxTexture = skyboxTexture;
		
		cmd->renderState = Ref<RenderState>::Create();
		cmd->renderState->Depth.TestEnabled = true;
		cmd->renderState->Depth.Function = GL_LEQUAL; // Important for skybox
		cmd->renderState->Depth.WriteMask = false; // Don't write to depth buffer
		cmd->renderState->Culling.Enabled = false; // Don't cull faces for skybox
		
		packet->SetCommandType(cmd->header.type);
		packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));
		
		return packet;
	}

	CommandPacket* Renderer3D::DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color, f32 thickness)
	{
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawLine: ScenePass is null!");
			return nullptr;
		}

		// Use a highly emissive material for skeleton visualization
		Material material{};
		material.Type = MaterialType::PBR;
		material.BaseColorFactor = glm::vec4(color, 1.0f);
		material.MetallicFactor = 0.0f;
		material.RoughnessFactor = 1.0f;
		material.EmissiveFactor = glm::vec4(color * 5.0f, 1.0f); // Very bright emissive for visibility through surfaces
        
		if (!s_Data.LineQuadMesh)
		{
			OLO_CORE_WARN("Renderer3D::DrawLine: LineQuadMesh not initialized");
			return nullptr;
		}

		// Build transform: translate to start, rotate to align +X with (end-start), scale X to length and Y to thickness
		const glm::vec3 seg = end - start;
		const float length = glm::length(seg);
		if (length <= 0.0001f)
			return nullptr;

		// Convert UI thickness to world thickness
		const float worldThickness = thickness * 0.005f; // matches previous visual scale

		// Compute rotation from +X axis to segment direction
		const glm::vec3 dir = seg / length;
		const glm::vec3 xAxis = glm::vec3(1,0,0);
		glm::mat4 rot(1.0f);
		const float dot = glm::clamp(glm::dot(xAxis, dir), -1.0f, 1.0f);
		if (dot < 0.9999f)
		{
			if (dot < -0.9999f) // Direction is opposite to +X axis (antiparallel)
			{
				// Use Y axis for 180-degree rotation to avoid zero cross product
				rot = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0,1,0));
			}
			else
			{
				glm::vec3 axis = glm::normalize(glm::cross(xAxis, dir));
				float angle = acosf(dot);
				rot = glm::rotate(glm::mat4(1.0f), angle, axis);
			}
		}
		// Scale: X=length, Y=worldThickness, Z=1
		glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(length, worldThickness, 1.0f));
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), start) * rot * scale;

		auto* packet = DrawMesh(s_Data.LineQuadMesh, transform, material);
		
		// Modify render state to ensure skeleton visibility
		if (packet)
		{
			auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
			if (drawCmd && drawCmd->renderState)
			{
				// Disable depth test so skeleton always shows on top
				drawCmd->renderState->Depth.TestEnabled = false;
				// Add some polygon offset to push skeleton forward
				drawCmd->renderState->PolygonOffset.Enabled = true;
				drawCmd->renderState->PolygonOffset.Factor = -2.0f;
				drawCmd->renderState->PolygonOffset.Units = -2.0f;
			}
		}
		
		return packet;
	}

	CommandPacket* Renderer3D::DrawSphere(const glm::vec3& position, f32 radius, const glm::vec3& color)
	{
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawSphere: ScenePass is null!");
			return nullptr;
		}

		// Create transform matrix for the sphere
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(radius));
		
		// Use a highly emissive material for skeleton joints
		Material material{};
		material.Type = MaterialType::PBR;
		material.BaseColorFactor = glm::vec4(color, 1.0f);
		material.MetallicFactor = 0.0f;
		material.RoughnessFactor = 0.8f;
		material.EmissiveFactor = glm::vec4(color * 3.0f, 1.0f); // Very bright emission for visibility through surfaces
		
		CommandPacket* packet = nullptr;
		
		// Use the cube mesh as a simple sphere approximation for now
		// TODO: Create a proper sphere mesh in the renderer data
		if (s_Data.CubeMesh)
		{
			packet = DrawMesh(s_Data.CubeMesh, transform, material);
		}
		else
		{
			OLO_CORE_WARN("Renderer3D::DrawSphere: No sphere mesh available, using fallback");
			return nullptr;
		}
		
		// Modify render state to ensure skeleton joint visibility
		if (packet)
		{
			auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
			if (drawCmd && drawCmd->renderState)
			{
				// Disable depth test so skeleton joints always show on top
				drawCmd->renderState->Depth.TestEnabled = false;
				// Add polygon offset to push joints forward
				drawCmd->renderState->PolygonOffset.Enabled = true;
				drawCmd->renderState->PolygonOffset.Factor = -2.0f;
				drawCmd->renderState->PolygonOffset.Units = -2.0f;
			}
		}
		
		return packet;
	}

	void Renderer3D::DrawSkeleton(const Skeleton& skeleton, const glm::mat4& modelMatrix, 
								  bool showBones, bool showJoints, f32 jointSize, f32 boneThickness)
	{
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkeleton: ScenePass is null!");
			return;
		}

		if (skeleton.m_GlobalTransforms.empty() || skeleton.m_ParentIndices.empty())
		{
			OLO_CORE_WARN("Renderer3D::DrawSkeleton: Empty skeleton data");
			return;
		}

		if (skeleton.m_GlobalTransforms.size() != skeleton.m_ParentIndices.size())
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkeleton: Skeleton transforms and parents size mismatch");
			return;
		}

		// Colors for visualization
		const glm::vec3 boneColor(1.0f, 0.5f, 0.0f);  // Bright orange for bones
		const glm::vec3 jointColor(0.0f, 1.0f, 0.0f); // Bright green for joints

		// Debug: Log skeleton rendering attempt
		static int debugCount = 0;
		if (debugCount < 5) // Only log first 5 attempts to avoid spam
		{
			OLO_CORE_INFO("DrawSkeleton Debug #{}: showJoints={}, showBones={}, jointSize={}, boneThickness={}", 
						  debugCount, showJoints, showBones, jointSize, boneThickness);
			OLO_CORE_INFO("  Skeleton size: {}, SkyboxMesh available: {}", 
						  skeleton.m_GlobalTransforms.size(), (s_Data.SkyboxMesh != nullptr));
			debugCount++;
		}

		// Draw joints
		if (showJoints)
		{
			for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
			{
				glm::vec3 jointPosition = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[i] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
				
				// Debug: Log first few joint positions
				if (debugCount <= 5 && i < 3)
				{
					OLO_CORE_INFO("  Joint {}: world position ({:.2f}, {:.2f}, {:.2f})", 
								  i, jointPosition.x, jointPosition.y, jointPosition.z);
				}
				
				auto* spherePacket = DrawSphere(jointPosition, jointSize, jointColor);
				if (spherePacket)
				{
					SubmitPacket(spherePacket);
					if (debugCount <= 5 && i < 3)
					{
						OLO_CORE_INFO("  Joint {} sphere packet submitted successfully", i);
					}
				}
				else if (debugCount <= 5 && i < 3)
				{
					OLO_CORE_WARN("  Joint {} sphere packet failed to create", i);
				}
			}
		}

		// Draw bones (connections between joints)
		if (showBones)
		{
			for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
			{
				i32 parentIndex = skeleton.m_ParentIndices[i];
				if (parentIndex >= 0 && parentIndex < static_cast<i32>(skeleton.m_GlobalTransforms.size()))
				{
					glm::vec3 childPosition = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[i] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
					glm::vec3 parentPosition = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[parentIndex] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
					
					// Calculate bone length to filter out unreasonable connections
					f32 boneLength = glm::length(childPosition - parentPosition);
					
					// Only draw bones that are reasonable length (filter out very long connections)
					// For a human-sized model, bones longer than 2 units are probably incorrect connections
					const f32 maxReasonableBoneLength = 2.0f;
					if (boneLength > 0.001f && boneLength < maxReasonableBoneLength)
					{
						auto* linePacket = DrawLine(parentPosition, childPosition, boneColor, boneThickness);
						if (linePacket)
						{
							SubmitPacket(linePacket);
						}
					}
				}
			}
		}
	}
}
