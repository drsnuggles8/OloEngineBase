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

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
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
	};

	static Renderer3DData s_Data;

	ShaderLibrary Renderer3D::m_ShaderLibrary;

	void Renderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();
		OLO_INFO("Initializing Renderer3D");

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

		// Initialize the render queue
		RenderQueue::Init();
		OLO_INFO("Renderer3D initialization complete");
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
		OLO_INFO("Shutting down Renderer3D");
		RenderQueue::Shutdown();
		OLO_INFO("Renderer3D shutdown complete");
	}

	void Renderer3D::BeginScene(const glm::mat4& viewProjectionMatrix)
	{
		OLO_TRACE("Renderer3D::BeginScene");
		s_Data.ViewProjectionMatrix = viewProjectionMatrix;
		RenderQueue::BeginScene(viewProjectionMatrix);
	}

	void Renderer3D::EndScene()
	{
		OLO_TRACE("Renderer3D::EndScene");
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

	void Renderer3D::DrawCube(const glm::mat4& modelMatrix, const Material& material)
	{
		OLO_TRACE("Renderer3D::DrawCube");
		DrawMesh(s_Data.CubeMesh, modelMatrix, material);
	}

	void Renderer3D::DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
	{
		OLO_TRACE("Renderer3D::DrawQuad");
		RenderQueue::SubmitQuad(modelMatrix, texture);
	}

	void Renderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material)
	{
		OLO_TRACE("Renderer3D::DrawMesh");
		RenderQueue::SubmitMesh(mesh, modelMatrix, material);
	}

	void Renderer3D::DrawLightCube(const glm::mat4& modelMatrix)
	{
		OLO_PROFILE_FUNCTION();
		OLO_TRACE("Renderer3D::DrawLightCube");

		s_Data.LightCubeShader->Bind();

		UpdateTransformUBO(modelMatrix);

		s_Data.CubeMesh->Draw();
	}

	void Renderer3D::RenderMeshInternal(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material)
	{
		OLO_TRACE("Renderer3D::RenderMeshInternal");
		s_Data.LightingShader->Bind();
		UpdateTransformUBO(modelMatrix);
		UpdateLightPropertiesUBO(material);
		UpdateTextureFlag(material);

		if (material.UseTextureMaps)
		{
			if (material.DiffuseMap)
				material.DiffuseMap->Bind(0);
			if (material.SpecularMap)
				material.SpecularMap->Bind(1);
		}

		mesh->Draw();
	}

	void Renderer3D::RenderQuadInternal(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
	{
		OLO_TRACE("Renderer3D::RenderQuadInternal");
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

		TransformMatrices matrices;
		matrices.ViewProjection = s_Data.ViewProjectionMatrix;
		matrices.Model = modelMatrix;

		s_Data.UBO->SetData(&matrices, sizeof(TransformMatrices));
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
