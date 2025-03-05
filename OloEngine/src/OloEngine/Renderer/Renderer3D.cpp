#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MSDFData.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
	struct Renderer3DData
	{
		Ref<VertexArray> VertexArray;
		Ref<VertexBuffer> VertexBuffer;
		Ref<Shader> LightCubeShader;
		Ref<Shader> LightingShader;
		Ref<IndexBuffer> IndexBuffer;
		Ref<UniformBuffer> UBO;
		Ref<UniformBuffer> LightPropertiesBuffer;

		glm::mat4 ViewProjectionMatrix;
		glm::vec3 LightPos;
		glm::vec3 ViewPos;
		float AmbientStrength;
		float SpecularStrength;
		float Shininess;
	};

	static Renderer3DData s_Data;

	ShaderLibrary Renderer3D::m_ShaderLibrary;

	void Renderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();

		s_Data.VertexArray = VertexArray::Create();

		// Cube vertices with position and normals
		f32 vertices[] = {
			// positions             // normals
			// Front face
			 0.5f,  0.5f,  0.5f,     0.0f,  0.0f,  1.0f,
			 0.5f, -0.5f,  0.5f,     0.0f,  0.0f,  1.0f,
			-0.5f, -0.5f,  0.5f,     0.0f,  0.0f,  1.0f,
			-0.5f,  0.5f,  0.5f,     0.0f,  0.0f,  1.0f,

			// Back face
			 0.5f,  0.5f, -0.5f,     0.0f,  0.0f, -1.0f,
			 0.5f, -0.5f, -0.5f,     0.0f,  0.0f, -1.0f,
			-0.5f, -0.5f, -0.5f,     0.0f,  0.0f, -1.0f,
			-0.5f,  0.5f, -0.5f,     0.0f,  0.0f, -1.0f,

			// Right face
			 0.5f,  0.5f,  0.5f,     1.0f,  0.0f,  0.0f,
			 0.5f, -0.5f,  0.5f,     1.0f,  0.0f,  0.0f,
			 0.5f, -0.5f, -0.5f,     1.0f,  0.0f,  0.0f,
			 0.5f,  0.5f, -0.5f,     1.0f,  0.0f,  0.0f,

			 // Left face
			 -0.5f,  0.5f,  0.5f,    -1.0f,  0.0f,  0.0f,
			 -0.5f, -0.5f,  0.5f,    -1.0f,  0.0f,  0.0f,
			 -0.5f, -0.5f, -0.5f,    -1.0f,  0.0f,  0.0f,
			 -0.5f,  0.5f, -0.5f,    -1.0f,  0.0f,  0.0f,

			 // Top face
			  0.5f,  0.5f,  0.5f,     0.0f,  1.0f,  0.0f,
			  0.5f,  0.5f, -0.5f,     0.0f,  1.0f,  0.0f,
			 -0.5f,  0.5f, -0.5f,     0.0f,  1.0f,  0.0f,
			 -0.5f,  0.5f,  0.5f,     0.0f,  1.0f,  0.0f,

			 // Bottom face
			  0.5f, -0.5f,  0.5f,     0.0f, -1.0f,  0.0f,
			  0.5f, -0.5f, -0.5f,     0.0f, -1.0f,  0.0f,
			 -0.5f, -0.5f, -0.5f,     0.0f, -1.0f,  0.0f,
			 -0.5f, -0.5f,  0.5f,     0.0f, -1.0f,  0.0f
		};

		u32 indices[] = {
			// front face
			0, 1, 3, 1, 2, 3,
			// back face
			4, 5, 7, 5, 6, 7,
			// right face
			8, 9, 11, 9, 10, 11,
			// left face
			12, 13, 15, 13, 14, 15,
			// top face
			16, 17, 19, 17, 18, 19,
			// bottom face
			20, 21, 23, 21, 22, 23
		};

		s_Data.VertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
		s_Data.VertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float3, "a_Normal" }
		});
		s_Data.VertexArray->AddVertexBuffer(s_Data.VertexBuffer);

		s_Data.IndexBuffer = IndexBuffer::Create(indices, sizeof(indices) / sizeof(u32));
		s_Data.VertexArray->SetIndexBuffer(s_Data.IndexBuffer);

		m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
		m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");

		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");

		s_Data.UBO = UniformBuffer::Create(sizeof(glm::mat4) * 2, 0);

		// 4 vec4s for object color, light color, light position, view position, and lighting parameters
		s_Data.LightPropertiesBuffer = UniformBuffer::Create(sizeof(glm::vec4) * 5, 1);

		// Set default lighting parameters
		s_Data.LightPos = glm::vec3(1.2f, 1.0f, 2.0f);
		s_Data.ViewPos = glm::vec3(0.0f, 0.0f, 3.0f);
		s_Data.AmbientStrength = 0.1f;
		s_Data.SpecularStrength = 0.5f;
		s_Data.Shininess = 32.0f;
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
	}

	void Renderer3D::BeginScene(const glm::mat4& viewProjectionMatrix)
	{
		s_Data.ViewProjectionMatrix = viewProjectionMatrix;
	}

	void Renderer3D::SetLightPosition(const glm::vec3& position)
	{
		s_Data.LightPos = position;
	}

	void Renderer3D::SetViewPosition(const glm::vec3& position)
	{
		s_Data.ViewPos = position;
	}

	void Renderer3D::SetLightingParameters(float ambientStrength, float specularStrength, float shininess)
	{
		s_Data.AmbientStrength = ambientStrength;
		s_Data.SpecularStrength = specularStrength;
		s_Data.Shininess = shininess;
	}

	void Renderer3D::EndScene()
	{}

	void Renderer3D::DrawCube(const glm::mat4& modelMatrix, const glm::vec3& objectColor, const glm::vec3& lightColor)
	{
		s_Data.LightingShader->Bind();

		// Update the UBO with the view projection and model matrices
		UniformData viewProjectionData = { &s_Data.ViewProjectionMatrix, sizeof(glm::mat4), 0 };
		UniformData modelData = { &modelMatrix, sizeof(glm::mat4), sizeof(glm::mat4) };
		s_Data.UBO->SetData(viewProjectionData);
		s_Data.UBO->SetData(modelData);

		// Update the LightProperties UBO
		glm::vec4 paddedObjectColor(objectColor, 1.0f);
		glm::vec4 paddedLightColor(lightColor, 1.0f);
		glm::vec4 paddedLightPos(s_Data.LightPos, 1.0f);
		glm::vec4 paddedViewPos(s_Data.ViewPos, 1.0f);
		glm::vec4 lightingParams(s_Data.AmbientStrength, s_Data.SpecularStrength, s_Data.Shininess, 0.0f);

		UniformData objectColorData = { &paddedObjectColor, sizeof(glm::vec4), 0 };
		UniformData lightColorData = { &paddedLightColor, sizeof(glm::vec4), sizeof(glm::vec4) };
		UniformData lightPosData = { &paddedLightPos, sizeof(glm::vec4), sizeof(glm::vec4) * 2 };
		UniformData viewPosData = { &paddedViewPos, sizeof(glm::vec4), sizeof(glm::vec4) * 3 };
		UniformData lightingParamsData = { &lightingParams, sizeof(glm::vec4), sizeof(glm::vec4) * 4 };

		s_Data.LightPropertiesBuffer->SetData(objectColorData);
		s_Data.LightPropertiesBuffer->SetData(lightColorData);
		s_Data.LightPropertiesBuffer->SetData(lightPosData);
		s_Data.LightPropertiesBuffer->SetData(viewPosData);
		s_Data.LightPropertiesBuffer->SetData(lightingParamsData);

		s_Data.VertexArray->Bind();
		RenderCommand::DrawIndexed(s_Data.VertexArray);
	}

	void Renderer3D::DrawLightCube(const glm::mat4& modelMatrix)
	{
		s_Data.LightCubeShader->Bind();

		// Update the UBO with the view projection and model matrices
		UniformData viewProjectionData = { &s_Data.ViewProjectionMatrix, sizeof(glm::mat4), 0 };
		UniformData modelData = { &modelMatrix, sizeof(glm::mat4), sizeof(glm::mat4) };
		s_Data.UBO->SetData(viewProjectionData);
		s_Data.UBO->SetData(modelData);

		s_Data.VertexArray->Bind();
		RenderCommand::DrawIndexed(s_Data.VertexArray);
	}
}
