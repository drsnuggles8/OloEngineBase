#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MSDFData.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
	// Forward declare the Statistics struct
	struct Statistics
	{
		uint32_t TotalMeshes = 0;
		uint32_t CulledMeshes = 0;
		
		void Reset() { TotalMeshes = 0; CulledMeshes = 0; }
	};

	struct Renderer3DData
	{
		Ref<Mesh> CubeMesh;
		Ref<Mesh> QuadMesh;
		Ref<Shader> LightCubeShader;
		Ref<Shader> LightingShader;
		Ref<Shader> QuadShader;
		Ref<UniformBuffer> UBO;
		Ref<UniformBuffer> LightPropertiesBuffer;
		Ref<UniformBuffer> TextureFlagBuffer;

		glm::mat4 ViewProjectionMatrix;

		Light SceneLight;
		glm::vec3 ViewPos;

		Frustum ViewFrustum;
		bool FrustumCullingEnabled = true;
		bool DynamicCullingEnabled = false;
		Renderer3D::Statistics Stats;
	};

	// Static member definitions
	Renderer3D::Renderer3DData Renderer3D::s_Data;
	ShaderLibrary Renderer3D::m_ShaderLibrary;

	void Renderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Initializing Renderer3D");

		s_Data.CubeMesh = Mesh::CreateCube();
		s_Data.QuadMesh = Mesh::CreatePlane(1.0f, 1.0f);

		m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
		m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");
		m_ShaderLibrary.Load("assets/shaders/Renderer3D_Quad.glsl");

		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");
		s_Data.QuadShader = m_ShaderLibrary.Get("Renderer3D_Quad");

		s_Data.UBO = UniformBuffer::Create(sizeof(glm::mat4) * 2, 0);

		s_Data.LightPropertiesBuffer = UniformBuffer::Create(sizeof(glm::vec4) * 12, 1);

		s_Data.TextureFlagBuffer = UniformBuffer::Create(sizeof(int), 2);

		s_Data.SceneLight.Position = glm::vec3(1.2f, 1.0f, 2.0f);
		s_Data.SceneLight.Ambient = glm::vec3(0.2f, 0.2f, 0.2f);
		s_Data.SceneLight.Diffuse = glm::vec3(0.5f, 0.5f, 0.5f);
		s_Data.SceneLight.Specular = glm::vec3(1.0f, 1.0f, 1.0f);

		s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);
		
		// Initialize statistics
		s_Data.Stats.Reset();

		// Initialize the render queue
		RenderQueue::Init();
		OLO_CORE_INFO("Renderer3D initialization complete");
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
		OLO_CORE_INFO("Shutting down Renderer3D");
		RenderQueue::Shutdown();
		OLO_CORE_INFO("Renderer3D shutdown complete");
	}

	void Renderer3D::BeginScene(const glm::mat4& viewProjectionMatrix)
	{
		s_Data.ViewProjectionMatrix = viewProjectionMatrix;
		
		// Update the view frustum for culling
		s_Data.ViewFrustum.Update(viewProjectionMatrix);
		
		// Reset statistics for this frame
		s_Data.Stats.Reset();
		
		RenderQueue::BeginScene(viewProjectionMatrix);
	}

	void Renderer3D::EndScene()
	{
		RenderQueue::EndScene();
	}

	void Renderer3D::SetLight(const Light& light)
	{
		s_Data.SceneLight = light;
	}

	void Renderer3D::SetViewPosition(const glm::vec3& position)
	{
		s_Data.ViewPos = position;
	}
	
	// Frustum culling methods
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
		RenderQueue::ResetStats();
	}

	bool Renderer3D::IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		// Get the transformed bounding sphere from the mesh
		BoundingSphere sphere = mesh->GetTransformedBoundingSphere(transform);
		
		// Add a larger margin to the radius to prevent popping (40% increase)
		sphere.Radius *= 1.4f;
		
		// Check if the sphere is visible in the frustum
		return s_Data.ViewFrustum.IsBoundingSphereVisible(sphere);
	}
	
	bool Renderer3D::IsVisibleInFrustum(const BoundingSphere& sphere)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		// Add a larger margin to the radius to prevent popping (40% increase)
		BoundingSphere expandedSphere = sphere;
		expandedSphere.Radius *= 1.4f;
		
		// Check if the sphere is visible in the frustum
		return s_Data.ViewFrustum.IsBoundingSphereVisible(expandedSphere);
	}
	
	bool Renderer3D::IsVisibleInFrustum(const BoundingBox& box)
	{
		OLO_PROFILE_FUNCTION();
		
		if (!s_Data.FrustumCullingEnabled)
			return true;
		
		// Check if the box is visible in the frustum
		return s_Data.ViewFrustum.IsBoundingBoxVisible(box);
	}

	void Renderer3D::DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic)
	{
		DrawMesh(s_Data.CubeMesh, modelMatrix, material, isStatic);
	}

	void Renderer3D::DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
	{
		RenderQueue::SubmitQuad(modelMatrix, texture);
	}

	void Renderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic)
	{
		OLO_PROFILE_FUNCTION();
		
		// Track total meshes for statistics
		s_Data.Stats.TotalMeshes++;
		
		// Perform frustum culling if enabled
		if (s_Data.FrustumCullingEnabled)
		{
			// Only perform culling if the object is static or dynamic culling is enabled
			if (isStatic || s_Data.DynamicCullingEnabled)
			{
				// Use a more robust visibility check with a larger safety margin
				if (!IsVisibleInFrustum(mesh, modelMatrix))
				{
					s_Data.Stats.CulledMeshes++;
					return; // Skip this mesh
				}
			}
		}
		
		// Submit to render queue if visible
		RenderQueue::SubmitMesh(mesh, modelMatrix, material, isStatic);
	}

	void Renderer3D::DrawLightCube(const glm::mat4& modelMatrix)
	{
		OLO_PROFILE_FUNCTION();

		s_Data.LightCubeShader->Bind();

		UpdateTransformUBO(modelMatrix);

		s_Data.CubeMesh->Draw();
	}

	void Renderer3D::RenderMeshInternal(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material)
	{
		OLO_PROFILE_FUNCTION();
		
		// Ensure the shader is bound first
		s_Data.LightingShader->Bind();
		
		// Update uniform buffers in a consistent order
		UpdateTransformUBO(modelMatrix);
		UpdateLightPropertiesUBO(material);
		UpdateTextureFlag(material);

		// Bind textures if needed
		if (material.UseTextureMaps)
		{
			if (material.DiffuseMap)
				material.DiffuseMap->Bind(0);
			
			if (material.SpecularMap)
				material.SpecularMap->Bind(1);
		}

		// Draw the mesh
		mesh->Draw();
	}
	
	void Renderer3D::RenderMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material)
	{
		OLO_PROFILE_FUNCTION();

		// Ensure the shader is bound first
		s_Data.LightingShader->Bind();
		
		// We only need to update the view projection matrix once
		s_Data.UBO->SetData(&s_Data.ViewProjectionMatrix, sizeof(glm::mat4));
		
		// Update uniform buffers in a consistent order
		UpdateLightPropertiesUBO(material);
		UpdateTextureFlag(material);

		// Bind textures if needed
		if (material.UseTextureMaps)
		{
			if (material.DiffuseMap)
				material.DiffuseMap->Bind(0);
			
			if (material.SpecularMap)
				material.SpecularMap->Bind(1);
		}

		// Draw the mesh with instancing
		mesh->GetVertexArray()->Bind();
		RenderCommand::DrawIndexedInstanced(mesh->GetVertexArray(), 0, static_cast<u32>(transforms.size()));
	}

	void Renderer3D::RenderQuadInternal(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
	{
		RenderCommand::EnableBlending();
		RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		RenderCommand::SetDepthMask(false);

		s_Data.QuadShader->Bind();
		UpdateTransformUBO(modelMatrix);
		texture->Bind(0);
		s_Data.QuadMesh->Draw();

		RenderCommand::SetDepthMask(true);
		RenderCommand::SetBlendFunc(GL_ONE, GL_ZERO);
	}

	void Renderer3D::UpdateTransformUBO(const glm::mat4& modelMatrix)
	{
		struct TransformMatrices
		{
			glm::mat4 ViewProjection;
			glm::mat4 Model;
		};

		// Create a copy of the model matrix to ensure it's stable
		glm::mat4 stableModelMatrix = modelMatrix;
		
		TransformMatrices matrices;
		matrices.ViewProjection = s_Data.ViewProjectionMatrix;
		matrices.Model = stableModelMatrix;

		s_Data.UBO->SetData(&matrices, sizeof(TransformMatrices));
	}
	
	void Renderer3D::UpdateTransformsUBO(const std::vector<glm::mat4>& transforms)
	{
		// For instanced rendering, we only update the transforms
		// The view-projection matrix is updated separately
		s_Data.UBO->SetData(transforms.data(), sizeof(glm::mat4) * transforms.size(), sizeof(glm::mat4));
	}

	void Renderer3D::UpdateLightPropertiesUBO(const Material& material)
	{
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

		lightData.MaterialAmbient = glm::vec4(material.Ambient, 0.0f);
		lightData.MaterialDiffuse = glm::vec4(material.Diffuse, 0.0f);
		lightData.MaterialSpecular = glm::vec4(material.Specular, material.Shininess);
		lightData.Padding1 = glm::vec4(0.0f);

		auto lightType = std::to_underlying(s_Data.SceneLight.Type);
		lightData.LightPosition = glm::vec4(s_Data.SceneLight.Position, 0.0f);
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

		lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<float>(lightType));

		s_Data.LightPropertiesBuffer->SetData(&lightData, sizeof(LightPropertiesData));
	}

	void Renderer3D::UpdateTextureFlag(const Material& material)
	{
		int useTextureMaps = material.UseTextureMaps ? 1 : 0;

		s_Data.TextureFlagBuffer->SetData(&useTextureMaps, sizeof(int));
	}
}
