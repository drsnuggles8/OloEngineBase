#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/SkinnedMesh.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

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

		s_Data.CubeMesh = Mesh::CreateCube();
		s_Data.QuadMesh = Mesh::CreatePlane(1.0f, 1.0f);
		m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
		m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");
		m_ShaderLibrary.Load("assets/shaders/SkinnedLighting3D_Simple.glsl");
		m_ShaderLibrary.Load("assets/shaders/Renderer3D_Quad.glsl");
		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");
		s_Data.SkinnedLightingShader = m_ShaderLibrary.Get("SkinnedLighting3D_Simple");
		s_Data.QuadShader = m_ShaderLibrary.Get("Renderer3D_Quad");
		
		s_Data.CameraUBO = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
		s_Data.LightPropertiesUBO = UniformBuffer::Create(ShaderBindingLayout::LightUBO::GetSize(), ShaderBindingLayout::UBO_LIGHTS);
		s_Data.MaterialUBO = UniformBuffer::Create(ShaderBindingLayout::MaterialUBO::GetSize(), ShaderBindingLayout::UBO_MATERIAL);
		s_Data.ModelMatrixUBO = UniformBuffer::Create(ShaderBindingLayout::ModelUBO::GetSize(), ShaderBindingLayout::UBO_MODEL);
		s_Data.BoneMatricesUBO = UniformBuffer::Create(ShaderBindingLayout::AnimationUBO::GetSize(), ShaderBindingLayout::UBO_ANIMATION);
		
		CommandDispatch::SetSharedUBOs(
			s_Data.CameraUBO,
			s_Data.MaterialUBO, 
			s_Data.LightPropertiesUBO,
			s_Data.BoneMatricesUBO,
			s_Data.ModelMatrixUBO
		);
		
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
		s_Data.RGraph = CreateRef<RenderGraph>();
		SetupRenderGraph(window.GetFramebufferWidth(), window.GetFramebufferHeight());		
		OLO_CORE_INFO("Renderer3D initialization complete.");
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Shutting down Renderer3D.");
		
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

		s_Data.ViewFrustum.Update(s_Data.ViewProjectionMatrix);
		
		s_Data.Stats.Reset();
		s_Data.CommandCounter = 0;
		
		UpdateCameraMatricesUBO(s_Data.ViewMatrix, s_Data.ProjectionMatrix);
		
		CommandDispatch::SetSceneLight(s_Data.SceneLight);
    	CommandDispatch::SetViewPosition(s_Data.ViewPos);
		
		s_Data.ScenePass->ResetCommandBucket();
		
		CommandDispatch::ResetState();
		
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
	
	bool Renderer3D::IsVisibleInFrustum(const Ref<SkinnedMesh>& mesh, const glm::mat4& transform)
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
		Ref<Shader> shaderToUse = material.Shader ? material.Shader : s_Data.LightingShader;
		if (!shaderToUse)
		{
			OLO_CORE_ERROR("Renderer3D::DrawMesh: No shader available!");
			return nullptr;
		}
		CommandPacket* packet = CreateDrawCall<DrawMeshCommand>();
		auto* cmd = packet->GetCommandData<DrawMeshCommand>();
		cmd->header.type = CommandType::DrawMesh;
		cmd->mesh = mesh;
		cmd->vertexArray = mesh->GetVertexArray();
		cmd->indexCount = mesh->GetIndexCount();
		cmd->transform = glm::mat4(modelMatrix);
		cmd->ambient = material.Ambient;
		cmd->diffuse = material.Diffuse;
		cmd->specular = material.Specular;
		cmd->shininess = material.Shininess;
		cmd->useTextureMaps = material.UseTextureMaps;
		cmd->diffuseMap = material.DiffuseMap;
		cmd->specularMap = material.SpecularMap;
		cmd->shader = shaderToUse;
		cmd->renderState = CreateRef<RenderState>();
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
			s_Data.QuadMesh = Mesh::CreatePlane(1.0f, 1.0f);
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
		cmd->renderState = CreateRef<RenderState>();
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
		cmd->ambient = material.Ambient;
		cmd->diffuse = material.Diffuse;
		cmd->specular = material.Specular;
		cmd->shininess = material.Shininess;
		cmd->useTextureMaps = material.UseTextureMaps;
		cmd->diffuseMap = material.DiffuseMap;
		cmd->specularMap = material.SpecularMap;
		cmd->shader = material.Shader ? material.Shader : s_Data.LightingShader;
		cmd->renderState = CreateRef<RenderState>();
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
		cmd->ambient = glm::vec3(1.0f);
		cmd->diffuse = glm::vec3(1.0f);
		cmd->specular = glm::vec3(1.0f);
		cmd->shininess = 32.0f;
		cmd->useTextureMaps = false;
		cmd->diffuseMap = nullptr;
		cmd->specularMap = nullptr;
		cmd->renderState = CreateRef<RenderState>();
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
		
		s_Data.ScenePass = CreateRef<SceneRenderPass>();
		s_Data.ScenePass->SetName("ScenePass");
		s_Data.ScenePass->Init(scenePassSpec);
		
		s_Data.FinalPass = CreateRef<FinalRenderPass>();
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

	CommandPacket* Renderer3D::DrawSkinnedMesh(const Ref<SkinnedMesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkinnedMesh: ScenePass is null!");
			return nullptr;
		}
		
		s_Data.Stats.TotalMeshes++;
		
		if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
		{
			if (!IsVisibleInFrustum(mesh, modelMatrix))
			{
				s_Data.Stats.CulledMeshes++;
				OLO_CORE_INFO("DrawSkinnedMesh: Mesh culled by frustum");
				return nullptr;
			}
		}
		
		if (!mesh || !mesh->GetVertexArray())
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkinnedMesh: Invalid mesh ({}) or vertex array ({})!", 
							(void*)mesh.get(), 
							mesh ? (void*)mesh->GetVertexArray().get() : nullptr);
			return nullptr;
		}

		
		Ref<Shader> shaderToUse = material.Shader ? material.Shader : s_Data.SkinnedLightingShader;
		if (!shaderToUse)
		{
			OLO_CORE_WARN("Renderer3D::DrawSkinnedMesh: SkinnedLighting shader not available, falling back to Lighting3D");
			shaderToUse = s_Data.LightingShader;
		}
		if (!shaderToUse)
		{
			OLO_CORE_ERROR("Renderer3D::DrawSkinnedMesh: No shader available!");
			return nullptr;
		}
		
		if (boneMatrices.empty())
		{
			OLO_CORE_WARN("Renderer3D::DrawSkinnedMesh: No bone matrices provided, using identity matrices");
		}
		
		CommandPacket* packet = CreateDrawCall<DrawSkinnedMeshCommand>();
		auto* cmd = packet->GetCommandData<DrawSkinnedMeshCommand>();
		
		cmd->header.type = CommandType::DrawSkinnedMesh;
		
		cmd->vertexArray = mesh->GetVertexArray();
		cmd->indexCount = mesh->GetIndexCount();
		cmd->modelMatrix = modelMatrix;
		
		cmd->ambient = material.Ambient;
		cmd->diffuse = material.Diffuse;
		cmd->specular = material.Specular;
		cmd->shininess = material.Shininess;
		cmd->useTextureMaps = material.UseTextureMaps;
		cmd->diffuseMap = material.DiffuseMap;
		cmd->specularMap = material.SpecularMap;
		
		cmd->shader = shaderToUse;
		cmd->renderState = CreateRef<RenderState>();
		
		cmd->boneMatrices = boneMatrices;
		
		packet->SetCommandType(cmd->header.type);
		packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));
		
		return packet;
	}

	void Renderer3D::ApplyGlobalResources()
	{
		OLO_PROFILE_FUNCTION();
		
		const auto& shaderRegistries = CommandDispatch::GetShaderRegistries();
		
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

		if (!scene)
		{
			OLO_CORE_WARN("Renderer3D::RenderAnimatedMeshes: Scene is null");
			return;
		}

		auto view = scene->GetAllEntitiesWith<AnimatedMeshComponent, SkeletonComponent, TransformComponent>();

		for (auto entityID : view)
		{
			Entity entity = { entityID, scene.get() };
			s_Data.Stats.TotalAnimatedMeshes++;

			RenderAnimatedMesh(entity, defaultMaterial);
		}
	}

	void Renderer3D::RenderAnimatedMesh(Entity entity, const Material& defaultMaterial)
	{
		OLO_PROFILE_FUNCTION();

		if (!entity.HasComponent<AnimatedMeshComponent>() || 
			!entity.HasComponent<SkeletonComponent>() ||
			!entity.HasComponent<TransformComponent>())
		{
			s_Data.Stats.SkippedAnimatedMeshes++;
			return;
		}

		auto& animatedMeshComp = entity.GetComponent<AnimatedMeshComponent>();
		auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
		auto& transformComp = entity.GetComponent<TransformComponent>();

		if (!animatedMeshComp.m_Mesh)
		{
			OLO_CORE_WARN("Renderer3D::RenderAnimatedMesh: Entity {} has invalid mesh", 
						 entity.GetComponent<TagComponent>().Tag);
			s_Data.Stats.SkippedAnimatedMeshes++;
			return;
		}

		glm::mat4 worldTransform = transformComp.GetTransform();

		Material material = defaultMaterial;

		const std::vector<glm::mat4>& boneMatrices = skeletonComp.m_FinalBoneMatrices;

		auto* packet = DrawSkinnedMesh(
			animatedMeshComp.m_Mesh,
			worldTransform,
			material,
			boneMatrices,
			false
		);

		if (packet)
		{
			SubmitPacket(packet);
			s_Data.Stats.RenderedAnimatedMeshes++;
		}
	}
}
