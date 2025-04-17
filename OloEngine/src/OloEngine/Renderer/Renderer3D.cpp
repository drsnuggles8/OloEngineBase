#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Core/Application.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
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
		m_ShaderLibrary.Load("assets/shaders/Renderer3D_Quad.glsl");

		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");
		s_Data.QuadShader = m_ShaderLibrary.Get("Renderer3D_Quad");

		// Create all necessary UBOs
		s_Data.TransformUBO = UniformBuffer::Create(sizeof(glm::mat4) * 2, 0);  // Model + VP matrices
		s_Data.MaterialUBO = UniformBuffer::Create(sizeof(glm::vec4) * 4, 1);   // Material properties
		s_Data.TextureFlagUBO = UniformBuffer::Create(sizeof(int), 2);          // Texture flags
		s_Data.CameraMatricesBuffer = UniformBuffer::Create(sizeof(glm::mat4) * 2, 3); // View and projection matrices
		s_Data.LightPropertiesUBO = UniformBuffer::Create(sizeof(glm::vec4) * 12, 1); // Binding point 1, not 4
		
		// Share UBOs with CommandDispatch
		CommandDispatch::SetSharedUBOs(
			s_Data.TransformUBO,
			s_Data.MaterialUBO, 
			s_Data.TextureFlagUBO,
			s_Data.CameraMatricesBuffer,
			s_Data.LightPropertiesUBO
		);
		
		OLO_CORE_INFO("Shared UBOs with CommandDispatch");

		// Initialize the default light
		s_Data.SceneLight.Position = glm::vec3(1.2f, 1.0f, 2.0f);
		s_Data.SceneLight.Ambient = glm::vec3(0.2f, 0.2f, 0.2f);
		s_Data.SceneLight.Diffuse = glm::vec3(0.5f, 0.5f, 0.5f);
		s_Data.SceneLight.Specular = glm::vec3(1.0f, 1.0f, 1.0f);

		s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);
		
		s_Data.Stats.Reset();
		
		// Initialize the render graph with command-based render passes
		Window& window = Application::Get().GetWindow();        
		s_Data.RGraph = CreateRef<RenderGraph>();
		SetupRenderGraph(window.GetFramebufferWidth(), window.GetFramebufferHeight());
		
		OLO_CORE_INFO("Renderer3D initialization complete.");
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Shutting down Renderer3D.");
		
		// Shutdown the render graph
		if (s_Data.RGraph)
			s_Data.RGraph->Shutdown();
		
		OLO_CORE_INFO("Renderer3D shutdown complete.");
	}

	void Renderer3D::BeginScene(const PerspectiveCamera& camera)
	{
		OLO_PROFILE_FUNCTION();
		
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

		// Update the view frustum for culling
		s_Data.ViewFrustum.Update(s_Data.ViewProjectionMatrix);
		
		// Reset statistics for this frame
		s_Data.Stats.Reset();
		s_Data.CommandCounter = 0;
		
		// Update the camera matrices UBO
		UpdateCameraMatricesUBO(s_Data.ViewMatrix, s_Data.ProjectionMatrix);
		
		// Share the view-projection matrix with CommandDispatch
		CommandDispatch::SetViewProjectionMatrix(s_Data.ViewProjectionMatrix);
		CommandDispatch::SetSceneLight(s_Data.SceneLight);
    	CommandDispatch::SetViewPosition(s_Data.ViewPos);
		
		// Reset the command bucket for this frame
		s_Data.ScenePass->ResetCommandBucket();
		
		// Reset CommandDispatch state tracking
		CommandDispatch::ResetState();
		
		// Explicitly update light properties UBO
		if (s_Data.LightPropertiesUBO)
		{
			// Use a default material for the initial UBO update
			Material defaultMaterial;
			
			// Build light properties data exactly as Renderer3D does
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

			lightData.MaterialAmbient = glm::vec4(defaultMaterial.Ambient, 0.0f);
			lightData.MaterialDiffuse = glm::vec4(defaultMaterial.Diffuse, 0.0f);
			lightData.MaterialSpecular = glm::vec4(defaultMaterial.Specular, defaultMaterial.Shininess);
			lightData.Padding1 = glm::vec4(0.0f);

			auto lightType = std::to_underlying(s_Data.SceneLight.Type);
			lightData.LightPosition = glm::vec4(s_Data.SceneLight.Position, 1.0f); // Use 1.0 for w to indicate position, not direction
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

			// Update the UBO directly
			s_Data.LightPropertiesUBO->SetData(&lightData, sizeof(LightPropertiesData));
		}
	}

	void Renderer3D::EndScene()
	{
		OLO_PROFILE_FUNCTION();
        
        // Make sure we have a valid render graph
        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("Renderer3D::EndScene: Render graph is null!");
            return;
        }
        
        // Ensure the final pass has the scene pass's framebuffer as input
        if (s_Data.ScenePass && s_Data.FinalPass)
        {
            s_Data.FinalPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
        }
        
		// Execute the render graph (which will execute all passes in order)
		s_Data.RGraph->Execute();

		CommandAllocator* allocator = s_Data.ScenePass->GetCommandBucket().GetAllocator();
		CommandMemoryManager::ReturnAllocator(allocator);
		s_Data.ScenePass->GetCommandBucket().SetAllocator(nullptr);
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
		return s_Data.FrustumCullingEnabled;
	}
	
	void Renderer3D::EnableDynamicCulling(bool enable)
	{
		s_Data.DynamicCullingEnabled = enable;
	}
	
	bool Renderer3D::IsDynamicCullingEnabled()
	{
		return s_Data.DynamicCullingEnabled;
	}
	
	const Frustum& Renderer3D::GetViewFrustum()
	{
		return s_Data.ViewFrustum;
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
		sphere.Radius *= 1.3f; // Safety margin to prevent popping
		
		return s_Data.ViewFrustum.IsBoundingSphereVisible(sphere);
	}
	
	bool Renderer3D::IsVisibleInFrustum(const BoundingSphere& sphere)
	{
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		BoundingSphere expandedSphere = sphere;
		expandedSphere.Radius *= 1.3f; // Safety margin to prevent popping
		
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
        
		struct CameraMatrices
		{
			glm::mat4 Projection;
			glm::mat4 View;
		};
		
		CameraMatrices matrices;
		matrices.Projection = projection;
		matrices.View = view;
		
		s_Data.CameraMatricesBuffer->SetData(&matrices, sizeof(CameraMatrices));
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
		
		// Create the framebuffer specification for our scene pass
		FramebufferSpecification scenePassSpec;
		scenePassSpec.Width = width;
		scenePassSpec.Height = height;
		scenePassSpec.Samples = 1;
		scenePassSpec.Attachments = {
			FramebufferTextureFormat::RGBA8,       // Color attachment
			FramebufferTextureFormat::Depth        // Depth attachment
		};
		
		// Create the final pass spec
		FramebufferSpecification finalPassSpec;
		finalPassSpec.Width = width;
		finalPassSpec.Height = height;
		
		// Create the command-based passes
		s_Data.ScenePass = CreateRef<SceneRenderPass>();
		s_Data.ScenePass->SetName("ScenePass");
		s_Data.ScenePass->Init(scenePassSpec);
		
		s_Data.FinalPass = CreateRef<FinalRenderPass>();
		s_Data.FinalPass->SetName("FinalPass");
		s_Data.FinalPass->Init(finalPassSpec);
		
		// Add passes to the render graph
		s_Data.RGraph->AddPass(s_Data.ScenePass);
		s_Data.RGraph->AddPass(s_Data.FinalPass);
		
		// Connect passes (scene pass output -> final pass input)
		s_Data.RGraph->ConnectPass("ScenePass", "FinalPass");
		
		 // Explicitly set the input framebuffer for the final pass
		s_Data.FinalPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
		OLO_CORE_INFO("Renderer3D: Connected scene pass framebuffer to final pass input");
		
		// Set the final pass
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
}
