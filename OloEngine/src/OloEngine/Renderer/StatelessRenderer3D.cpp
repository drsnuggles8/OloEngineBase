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

		s_Data.CubeMesh = Mesh::CreateCube();
		s_Data.QuadMesh = Mesh::CreatePlane(1.0f, 1.0f);

		m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
		m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");
		m_ShaderLibrary.Load("assets/shaders/Renderer3D_Quad.glsl");

		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");
		s_Data.QuadShader = m_ShaderLibrary.Get("Renderer3D_Quad");

		// Create a camera matrices UBO (view and projection matrices)
		s_Data.CameraMatricesBuffer = UniformBuffer::Create(sizeof(glm::mat4) * 2, 3);

		// Initialize the default light
		s_Data.SceneLight.Position = glm::vec3(1.2f, 1.0f, 2.0f);
		s_Data.SceneLight.Ambient = glm::vec3(0.2f, 0.2f, 0.2f);
		s_Data.SceneLight.Diffuse = glm::vec3(0.5f, 0.5f, 0.5f);
		s_Data.SceneLight.Specular = glm::vec3(1.0f, 1.0f, 1.0f);

		s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);
		
		s_Data.Stats.Reset();

		// Create the command allocator for command-based rendering
		s_Data.CommandAlloc = CreateRef<CommandAllocator>();
		
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
			
		// Release command allocator
		s_Data.CommandAlloc.reset();
		
		OLO_CORE_INFO("StatelessRenderer3D shutdown complete.");
	}

	void StatelessRenderer3D::BeginScene(const PerspectiveCamera& camera)
	{
		OLO_PROFILE_FUNCTION();
        
        // Check that we have our command allocator and CommandSceneRenderPass
        if (!s_Data.CommandAlloc || !s_Data.ScenePass)
        {
            OLO_CORE_ERROR("StatelessRenderer3D::BeginScene: Command allocator or ScenePass is null!");
            return;
        }

		s_Data.ViewMatrix = camera.GetView();
		s_Data.ProjectionMatrix = camera.GetProjection();
		s_Data.ViewProjectionMatrix = camera.GetViewProjection();

		// Update the view frustum for culling
		s_Data.ViewFrustum.Update(s_Data.ViewProjectionMatrix);
		
		// Reset statistics for this frame
		s_Data.Stats.Reset();
		
		// Update the camera matrices UBO
		UpdateCameraMatricesUBO(s_Data.ViewMatrix, s_Data.ProjectionMatrix);
		
		// Reset the command bucket for this frame - properly encapsulated
		s_Data.ScenePass->ResetCommandBucket();
		
		// Make sure scene pass has access to the command allocator - properly encapsulated
		s_Data.ScenePass->SetCommandAllocator(s_Data.CommandAlloc.get());
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
	}

	void StatelessRenderer3D::DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
	{
		OLO_PROFILE_FUNCTION();
        
		if (!s_Data.ScenePass || !s_Data.CommandAlloc)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawQuad: ScenePass or CommandAllocator is null!");
			return;
		}

		 // Add transparency settings for quads if texture has alpha channel
		if (texture->HasAlphaChannel())
		{
			// Submit a transparent quad with proper encapsulation
			s_Data.ScenePass->SubmitTransparentQuad(modelMatrix, texture, s_Data.QuadShader, *s_Data.CommandAlloc);
		}
		else
		{
			// Submit an opaque quad with proper encapsulation
			s_Data.ScenePass->SubmitQuad(modelMatrix, texture, *s_Data.CommandAlloc);
		}
	}

	void StatelessRenderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();
        
		if (!s_Data.ScenePass || !s_Data.CommandAlloc)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawMesh: ScenePass or CommandAllocator is null!");
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
		
		 // Add additional material properties from scene light
		Material materialWithLighting = material;
		materialWithLighting.LightPosition = s_Data.SceneLight.Position;
		materialWithLighting.ViewPosition = s_Data.ViewPos;
		
		 // Submit mesh with proper encapsulation
		s_Data.ScenePass->SubmitMesh(mesh, modelMatrix, materialWithLighting, *s_Data.CommandAlloc);
	}

	void StatelessRenderer3D::DrawLightCube(const glm::mat4& modelMatrix)
	{
		OLO_PROFILE_FUNCTION();
        
		if (!s_Data.ScenePass || !s_Data.CommandAlloc)
		{
			OLO_CORE_ERROR("StatelessRenderer3D::DrawLightCube: ScenePass or CommandAllocator is null!");
			return;
		 }

		 // Submit light cube with proper encapsulation
		s_Data.ScenePass->SubmitLightCube(s_Data.CubeMesh, modelMatrix, s_Data.LightCubeShader, *s_Data.CommandAlloc);
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
		s_Data.ScenePass->SetCommandAllocator(s_Data.CommandAlloc.get()); // Properly encapsulated
		
		s_Data.FinalPass = CreateRef<CommandFinalRenderPass>();
		s_Data.FinalPass->SetName("CommandFinalPass");
		s_Data.FinalPass->Init(finalPassSpec);
		
		// Add passes to the render graph
		s_Data.RGraph->AddPass(s_Data.ScenePass);
		s_Data.RGraph->AddPass(s_Data.FinalPass);
		
		// Connect passes (scene pass output -> final pass input)
		s_Data.RGraph->ConnectPass("CommandScenePass", "CommandFinalPass");
		
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
}