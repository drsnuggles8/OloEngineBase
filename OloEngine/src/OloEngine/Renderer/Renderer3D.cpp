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
		Ref<Shader> Shader;
		Ref<IndexBuffer> IndexBuffer;
		Ref<UniformBuffer> UniformBuffer;

		glm::mat4 ViewProjectionMatrix;
	};

	static Renderer3DData s_Data;

	void Renderer3D::Init()
	{
		OLO_PROFILE_FUNCTION();

		s_Data.VertexArray = VertexArray::Create();

		float vertices[] = {
			// positions          // colors
			 0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f, // top right front (red)
			 0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f, // bottom right front (red)
			-0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f, // bottom left front (red)
			-0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f, // top left front (red)
			 0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f, // top right back (green)
			 0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f, // bottom right back (green)
			-0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f, // bottom left back (green)
			-0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f, // top left back (green)
			 0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f, // top right front (blue)
			 0.5f, -0.5f,  0.5f,   0.0f, 0.0f, 1.0f, // bottom right front (blue)
			 0.5f, -0.5f, -0.5f,   0.0f, 0.0f, 1.0f, // bottom right back (blue)
			 0.5f,  0.5f, -0.5f,   0.0f, 0.0f, 1.0f, // top right back (blue)
			-0.5f,  0.5f,  0.5f,   1.0f, 1.0f, 0.0f, // top left front (yellow)
			-0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.0f, // bottom left front (yellow)
			-0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.0f, // bottom left back (yellow)
			-0.5f,  0.5f, -0.5f,   1.0f, 1.0f, 0.0f, // top left back (yellow)
			 0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 1.0f, // top right front (magenta)
			 0.5f,  0.5f, -0.5f,   1.0f, 0.0f, 1.0f, // top right back (magenta)
			-0.5f,  0.5f, -0.5f,   1.0f, 0.0f, 1.0f, // top left back (magenta)
			-0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 1.0f, // top left front (magenta)
			 0.5f, -0.5f,  0.5f,   0.0f, 1.0f, 1.0f, // bottom right front (cyan)
			 0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 1.0f, // bottom right back (cyan)
			-0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 1.0f, // bottom left back (cyan)
			-0.5f, -0.5f,  0.5f,   0.0f, 1.0f, 1.0f  // bottom left front (cyan)
		};

		uint32_t indices[] = {
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
			{ ShaderDataType::Float3, "a_Color" }
		});
		s_Data.VertexArray->AddVertexBuffer(s_Data.VertexBuffer);

		s_Data.IndexBuffer = IndexBuffer::Create(indices, sizeof(indices) / sizeof(u32));
		s_Data.VertexArray->SetIndexBuffer(s_Data.IndexBuffer);

		s_Data.Shader = Shader::Create("assets/shaders/Basic3D.glsl");

		s_Data.UniformBuffer = UniformBuffer::Create(sizeof(glm::mat4) * 2, 0);
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
	{
	}

	void Renderer3D::Draw(const glm::mat4& modelMatrix)
	{
		s_Data.Shader->Bind();

		// Update the UBO with the view projection and model matrices
		UniformData viewProjectionData = { &s_Data.ViewProjectionMatrix, sizeof(glm::mat4), 0 };
		UniformData modelData = { &modelMatrix, sizeof(glm::mat4), sizeof(glm::mat4) };
		s_Data.UniformBuffer->SetData(viewProjectionData);
		s_Data.UniformBuffer->SetData(modelData);

		s_Data.VertexArray->Bind();
		RenderCommand::DrawIndexed(s_Data.VertexArray);
	}
}
