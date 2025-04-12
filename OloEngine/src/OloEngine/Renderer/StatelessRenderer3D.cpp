#include "OloEnginePCH.h"
#include "OloEngine/Renderer/StatelessRenderer3D.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Passes/CommandSceneRenderPass.h"
#include "OloEngine/Renderer/Passes/CommandFinalRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Core/Application.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
	StatelessRenderer3D::StatelessRenderer3DData StatelessRenderer3D::s_Data;
	ShaderLibrary StatelessRenderer3D::m_ShaderLibrary;
	void StatelessRenderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Initializing StatelessRenderer3D.");

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
		
		OLO_CORE_INFO("StatelessRenderer3D initialization complete.");
	}

	void StatelessRenderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Shutting down StatelessRenderer3D.");
		
		// Shutdown the render graph
		if (s_Data.RGraph)
			s_Data.RGraph->Shutdown();
		
		OLO_CORE_INFO("StatelessRenderer3D shutdown complete.");
	}
	void StatelessRenderer3D::BeginScene(const PerspectiveCamera& camera)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::BeginScene: ScenePass is null!");
			return;
		}

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
		
		// Explicitly update light properties UBO just like Renderer3D does in its BeginScene
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

	void StatelessRenderer3D::EndScene()
	{
		OLO_PROFILE_FUNCTION();
        
        // Make sure we have a valid render graph
        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("StatelessRenderer3D::EndScene: Render graph is null!");
            return;
        }
        
        // Ensure the final pass has the scene pass's framebuffer as input
        if (s_Data.ScenePass && s_Data.FinalPass)
        {
            s_Data.FinalPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
        }
        
		// Execute the render graph (which will execute all passes in order)
		s_Data.RGraph->Execute();
	}

	void StatelessRenderer3D::SetLight(const Light& light)
	{
		s_Data.SceneLight = light;
	}

	void StatelessRenderer3D::SetViewPosition(const glm::vec3& position)
	{
		s_Data.ViewPos = position;
	}
	
	void StatelessRenderer3D::EnableFrustumCulling(bool enable)
	{
		s_Data.FrustumCullingEnabled = enable;
	}
	
	bool StatelessRenderer3D::IsFrustumCullingEnabled()
	{
		return s_Data.FrustumCullingEnabled;
	}
	
	void StatelessRenderer3D::EnableDynamicCulling(bool enable)
	{
		s_Data.DynamicCullingEnabled = enable;
	}
	
	bool StatelessRenderer3D::IsDynamicCullingEnabled()
	{
		return s_Data.DynamicCullingEnabled;
	}
	
	const Frustum& StatelessRenderer3D::GetViewFrustum()
	{
		return s_Data.ViewFrustum;
	}
	
	StatelessRenderer3D::Statistics StatelessRenderer3D::GetStats()
	{
		return s_Data.Stats;
	}
	
	void StatelessRenderer3D::ResetStats()
	{
		s_Data.Stats.Reset();
	}

	bool StatelessRenderer3D::IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform)
	{
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		BoundingSphere sphere = mesh->GetTransformedBoundingSphere(transform);
		sphere.Radius *= 1.3f; // Safety margin to prevent popping
		
		return s_Data.ViewFrustum.IsBoundingSphereVisible(sphere);
	}
	
	bool StatelessRenderer3D::IsVisibleInFrustum(const BoundingSphere& sphere)
	{
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		BoundingSphere expandedSphere = sphere;
		expandedSphere.Radius *= 1.3f; // Safety margin to prevent popping
		
		return s_Data.ViewFrustum.IsBoundingSphereVisible(expandedSphere);
	}
	
	bool StatelessRenderer3D::IsVisibleInFrustum(const BoundingBox& box)
	{
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		return s_Data.ViewFrustum.IsBoundingBoxVisible(box);
	}

	void StatelessRenderer3D::DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic)
	{
		DrawMesh(s_Data.CubeMesh, modelMatrix, material, isStatic);
	}	void StatelessRenderer3D::DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawQuad: ScenePass is null!");
			return;
		}

		if (!texture)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawQuad: No texture provided!");
			return;
		}

		if (!s_Data.QuadShader)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawQuad: Quad shader is not loaded!");
			return;
		}

		// Make absolutely sure we have a valid vertex array for the quad
		if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawQuad: Quad mesh or its vertex array is invalid!");
			// Recreate the mesh as a fallback
			s_Data.QuadMesh = Mesh::CreatePlane(1.0f, 1.0f);
			if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
			{
				return; // Still invalid, can't continue
			}
		}

		// Create the command structure directly
		DrawQuadCommand command;
		command.header.type = CommandType::DrawQuad;
		
		// Store a copy of the transform to ensure it remains stable
		command.transform = glm::mat4(modelMatrix);
		command.texture = texture;
		command.shader = s_Data.QuadShader;
		command.quadVA = s_Data.QuadMesh->GetVertexArray();
		
		// Add metadata for sorting and tracking - important for proper rendering
		PacketMetadata metadata;
		metadata.shaderKey = s_Data.QuadShader->GetRendererID();  // Use shader's renderer ID
		metadata.textureKey = texture->GetRendererID();           // Use texture's renderer ID
		metadata.executionOrder = s_Data.CommandCounter++;        // Keep track of ordering
		
		// Add depth sorting - will put transparent quads at the end for proper blending
		glm::vec3 position = glm::vec3(modelMatrix[3]);
		f32 distSqr = glm::distance2(s_Data.ViewPos, position);
		metadata.sortKey = *reinterpret_cast<u64*>(&distSqr);
		
		// Mark as transparent - CRITICAL for proper alpha handling
		metadata.isTransparent = true;
		metadata.dependsOnPrevious = false; // Don't need sequential execution
		metadata.debugName = "GrassQuad";   // Helpful for debugging
		
		OLO_CORE_TRACE("Submitting quad command with texture ID: {}, shader ID: {}", 
			texture->GetRendererID(), s_Data.QuadShader->GetRendererID());
		
		// Submit the command to the render pass
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}
	void StatelessRenderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawMesh: ScenePass is null!");
			return;
		}
			
		// Track statistics and perform frustum culling
		s_Data.Stats.TotalMeshes++;
		if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
		{
			if (!IsVisibleInFrustum(mesh, modelMatrix))
			{
				s_Data.Stats.CulledMeshes++;
				return;
			}
		}
		
		// Ensure we have a valid mesh reference
		if (!mesh || !mesh->GetVertexArray())
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawMesh: Invalid mesh or vertex array!");
			return;
		}
		
		// Select the appropriate shader - exactly as in Renderer3D
		Ref<Shader> shaderToUse = material.Shader ? material.Shader : s_Data.LightingShader;
		if (!shaderToUse)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawMesh: No shader available!");
			return;
		}
		
		// Create the command structure with a stable copy of the model matrix
		DrawMeshCommand command;
		command.header.type = CommandType::DrawMesh;
		
		// Fill in the mesh data using actual objects instead of IDs
		command.mesh = mesh;
		command.vertexArray = mesh->GetVertexArray(); 
		command.indexCount = mesh->GetIndexCount();
		command.transform = glm::mat4(modelMatrix); // Make a copy to ensure it remains stable
		
		// Fill in material properties
		command.ambient = material.Ambient;
		command.diffuse = material.Diffuse;
		command.specular = material.Specular;
		command.shininess = material.Shininess;
		command.useTextureMaps = material.UseTextureMaps;
		
		// Use actual texture and shader objects
		command.diffuseMap = material.DiffuseMap;
		command.specularMap = material.SpecularMap;
		command.shader = shaderToUse;
		
		// Create metadata for sorting/batching that exactly matches Renderer3D's RenderQueue approach
		PacketMetadata metadata;
		metadata.shaderKey = shaderToUse->GetRendererID();
		metadata.materialKey = material.CalculateKey();
		metadata.textureKey = material.DiffuseMap ? material.DiffuseMap->GetRendererID() : 0;
		
		// Use depth sorting similar to RenderQueue for consistent ordering
		glm::vec3 position = glm::vec3(modelMatrix[3]);
		f32 distSqr = glm::distance2(s_Data.ViewPos, position);
		metadata.sortKey = *reinterpret_cast<u64*>(&distSqr);
		metadata.executionOrder = s_Data.CommandCounter++;
		metadata.isStatic = isStatic;
		
		// Submit command via the CommandRenderPass's SubmitCommand method
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, 
		const Material& material, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();

		if (!s_Data.ScenePass)
		{
		OLO_CORE_ERROR("StatelessRenderer3D::DrawMeshInstanced: ScenePass is null!");
		return;
		}

		// Don't bother if there are no transforms
		if (transforms.empty())
		{
		OLO_CORE_WARN("StatelessRenderer3D::DrawMeshInstanced: No transforms provided");
		return;
		}

		// Track statistics and perform frustum culling on the first transform only for simplicity
		// In a more advanced implementation, you might want to check each instance
		s_Data.Stats.TotalMeshes += transforms.size();
		if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
		{
		if (!IsVisibleInFrustum(mesh, transforms[0]))
		{
		s_Data.Stats.CulledMeshes += transforms.size();
		return;
		}
		}

		// Create the command structure
		DrawMeshInstancedCommand command;
		command.header.type = CommandType::DrawMeshInstanced;
		// The dispatch function will be filled by the command system

		// Fill in the mesh data
		command.mesh = mesh;
		command.vertexArray = mesh->GetVertexArray();
		command.indexCount = mesh->GetIndexCount();
		command.instanceCount = static_cast<u32>(transforms.size());
		command.transforms = transforms;  // Store all transforms

		// Fill in material properties
		command.ambient = material.Ambient;
		command.diffuse = material.Diffuse;
		command.specular = material.Specular;
		command.shininess = material.Shininess;
		command.useTextureMaps = material.UseTextureMaps;

		// Add texture maps and shader
		command.diffuseMap = material.DiffuseMap;
		command.specularMap = material.SpecularMap;
		command.shader = material.Shader ? material.Shader : s_Data.LightingShader;

		// Create metadata for sorting/batching
		PacketMetadata metadata;
		metadata.shaderKey = command.shader->GetRendererID();
		metadata.materialKey = material.CalculateKey();
		metadata.textureKey = material.DiffuseMap ? material.DiffuseMap->GetRendererID() : 0;
		metadata.executionOrder = s_Data.CommandCounter++; // Maintain order if needed
		metadata.isStatic = isStatic;

		// Submit command via the CommandRenderPass's SubmitCommand method
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::DrawLightCube(const glm::mat4& modelMatrix)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawLightCube: ScenePass is null!");
			return;
		}
		
		// Create the command structure directly
		DrawMeshCommand command;
		command.header.type = CommandType::DrawMesh;
		// The dispatch function will be filled by the command system
		
		// Fill in the mesh data using actual objects
		command.mesh = s_Data.CubeMesh;
		command.vertexArray = s_Data.CubeMesh->GetVertexArray();
		command.indexCount = s_Data.CubeMesh->GetIndexCount();
		command.transform = modelMatrix;
		
		// Light cube uses a special shader with solid color
		command.shader = s_Data.LightCubeShader;
		
		// Light cubes are typically just a solid color, so we set simple material properties
		command.ambient = glm::vec3(1.0f);  // Full ambient for light sources
		command.diffuse = glm::vec3(1.0f);  // Full diffuse for light sources
		command.specular = glm::vec3(1.0f); // Full specular for light sources
		command.shininess = 32.0f;
		command.useTextureMaps = false;
		command.diffuseMap = nullptr;
		command.specularMap = nullptr;
		
		// Create metadata for sorting/batching
		PacketMetadata metadata;
		metadata.shaderKey = s_Data.LightCubeShader->GetRendererID();
		metadata.materialKey = 0; // Special zero key for light sources
		metadata.textureKey = 0;  // No textures for light cubes
		metadata.executionOrder = s_Data.CommandCounter++; // Maintain order if needed
		metadata.isStatic = false; // Lights can move
		
		// Submit command via the CommandRenderPass's SubmitCommand method
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection)
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
	
	void StatelessRenderer3D::SetupRenderGraph(u32 width, u32 height)
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Setting up StatelessRenderer3D RenderGraph with dimensions: {}x{}", width, height);

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
		s_Data.ScenePass = CreateRef<CommandSceneRenderPass>();
		s_Data.ScenePass->SetName("CommandScenePass");
		s_Data.ScenePass->Init(scenePassSpec);
		
		s_Data.FinalPass = CreateRef<CommandFinalRenderPass>();
		s_Data.FinalPass->SetName("CommandFinalPass");
		s_Data.FinalPass->Init(finalPassSpec);
		
		// Add passes to the render graph
		s_Data.RGraph->AddPass(s_Data.ScenePass);
		s_Data.RGraph->AddPass(s_Data.FinalPass);
		
		// Connect passes (scene pass output -> final pass input)
		s_Data.RGraph->ConnectPass("CommandScenePass", "CommandFinalPass");
		
		 // Explicitly set the input framebuffer for the final pass
		s_Data.FinalPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
		OLO_CORE_INFO("StatelessRenderer3D: Connected scene pass framebuffer to final pass input");
		
		// Set the final pass
		s_Data.RGraph->SetFinalPass("CommandFinalPass");
	}
	
	void StatelessRenderer3D::OnWindowResize(u32 width, u32 height)
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("StatelessRenderer3D::OnWindowResize: Resizing to {}x{}", width, height);
		
		if (s_Data.RGraph)
		{
			s_Data.RGraph->Resize(width, height);
		}
		else
		{
			OLO_CORE_WARN("StatelessRenderer3D::OnWindowResize: No render graph available!");
		}
	}

	// State management functions implementation
	void StatelessRenderer3D::SetPolygonMode(unsigned int face, unsigned int mode)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::SetPolygonMode: ScenePass is null!");
			return;
		}
		
		SetPolygonModeCommand command;
		command.header.type = CommandType::SetPolygonMode;
		command.face = face;
		command.mode = mode;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::SetLineWidth(float width)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::SetLineWidth: ScenePass is null!");
			return;
		}
		
		SetLineWidthCommand command;
		command.header.type = CommandType::SetLineWidth;
		command.width = width;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::EnableBlending()
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::EnableBlending: ScenePass is null!");
			return;
		}
		
		SetBlendStateCommand command;
		command.header.type = CommandType::SetBlendState;
		command.enabled = true;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::DisableBlending()
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DisableBlending: ScenePass is null!");
			return;
		}
		
		SetBlendStateCommand command;
		command.header.type = CommandType::SetBlendState;
		command.enabled = false;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::SetBlendFunc(unsigned int src, unsigned int dest)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::SetBlendFunc: ScenePass is null!");
			return;
		}
		
		SetBlendFuncCommand command;
		command.header.type = CommandType::SetBlendFunc;
		command.sourceFactor = src;
		command.destFactor = dest;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::SetColorMask(bool red, bool green, bool blue, bool alpha)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::SetColorMask: ScenePass is null!");
			return;
		}
		
		SetColorMaskCommand command;
		command.header.type = CommandType::SetColorMask;
		command.red = red;
		command.green = green;
		command.blue = blue;
		command.alpha = alpha;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::SetDepthMask(bool enabled)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::SetDepthMask: ScenePass is null!");
			return;
		}
		
		SetDepthMaskCommand command;
		command.header.type = CommandType::SetDepthMask;
		command.writeMask = enabled;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::EnableDepthTest()
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::EnableDepthTest: ScenePass is null!");
			return;
		}
		
		SetDepthTestCommand command;
		command.header.type = CommandType::SetDepthTest;
		command.enabled = true;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}

	void StatelessRenderer3D::DisableDepthTest()
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.ScenePass)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DisableDepthTest: ScenePass is null!");
			return;
		}
		
		SetDepthTestCommand command;
		command.header.type = CommandType::SetDepthTest;
		command.enabled = false;
		
		PacketMetadata metadata;
		metadata.executionOrder = s_Data.CommandCounter++;
		
		s_Data.ScenePass->SubmitCommand(command, metadata);
	}
}
