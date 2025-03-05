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
		//Ref<Shader> BasicShader;
		Ref<Shader> LightCubeShader;
		Ref<Shader> LightingShader;
		Ref<IndexBuffer> IndexBuffer;
		Ref<UniformBuffer> UBO;
		Ref<UniformBuffer> LightPropertiesBuffer;

		glm::mat4 ViewProjectionMatrix;
	};

	static Renderer3DData s_Data;

	ShaderLibrary Renderer3D::m_ShaderLibrary;

	void Renderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();

		s_Data.VertexArray = VertexArray::Create();

		f32 vertices[] = {
			// positions
			 0.5f,  0.5f,  0.5f,
			 0.5f, -0.5f,  0.5f,
			-0.5f, -0.5f,  0.5f,
			-0.5f,  0.5f,  0.5f,
			 0.5f,  0.5f, -0.5f,
			 0.5f, -0.5f, -0.5f,
			-0.5f, -0.5f, -0.5f,
			-0.5f,  0.5f, -0.5f
		};

		u32 indices[] = {
			// front face
			0, 1, 3, 1, 2, 3,
			// back face
			4, 5, 7, 5, 6, 7,
			// right face
			0, 1, 4, 1, 5, 4,
			// left face
			2, 3, 6, 3, 7, 6,
			// top face
			0, 3, 4, 3, 7, 4,
			// bottom face
			1, 2, 5, 2, 6, 5
		};

		s_Data.VertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
		s_Data.VertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" }
		});
		s_Data.VertexArray->AddVertexBuffer(s_Data.VertexBuffer);

		s_Data.IndexBuffer = IndexBuffer::Create(indices, sizeof(indices) / sizeof(u32));
		s_Data.VertexArray->SetIndexBuffer(s_Data.IndexBuffer);

		//m_ShaderLibrary.Load("assets/shaders/Basic3D.glsl");
		m_ShaderLibrary.Load("assets/shaders/LightCube.glsl");
		m_ShaderLibrary.Load("assets/shaders/Lighting3D.glsl");

		//s_Data.BasicShader = m_ShaderLibrary.Get("Basic3D");
		s_Data.LightCubeShader = m_ShaderLibrary.Get("LightCube");
		s_Data.LightingShader = m_ShaderLibrary.Get("Lighting3D");

		s_Data.UBO = UniformBuffer::Create(sizeof(glm::mat4) * 2, 0);
		s_Data.LightPropertiesBuffer = UniformBuffer::Create(sizeof(glm::vec4) * 2, 1);
	}

	void Renderer3D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();
	}

	void Renderer3D::BeginScene(const glm::mat4& viewProjectionMatrix)
	{
		s_Data.ViewProjectionMatrix = viewProjectionMatrix;
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

		UniformData objectColorData = { &paddedObjectColor, sizeof(glm::vec4), 0 };
		UniformData lightColorData = { &paddedLightColor, sizeof(glm::vec4), sizeof(glm::vec4) };
		s_Data.LightPropertiesBuffer->SetData(objectColorData);
		s_Data.LightPropertiesBuffer->SetData(lightColorData);

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
