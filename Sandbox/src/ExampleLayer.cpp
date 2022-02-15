// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "ExampleLayer.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

ExampleLayer::ExampleLayer() 
	: Layer("ExampleLayer"), m_CameraController(1280.0f / 720.0f)
{
	m_VertexArray = OloEngine::VertexArray::Create();

	float vertices[3 * 7] = {
		-0.5f, -0.5f, 0.0f, 0.8f, 0.2f, 0.8f, 1.0f,
		 0.5f, -0.5f, 0.0f, 0.2f, 0.3f, 0.8f, 1.0f,
		 0.0f,  0.5f, 0.0f, 0.8f, 0.8f, 0.2f, 1.0f
	};

	OloEngine::Ref<OloEngine::VertexBuffer> vertexBuffer = OloEngine::VertexBuffer::Create(vertices, sizeof(vertices));
	OloEngine::BufferLayout layout = {
		{ OloEngine::ShaderDataType::Float3, "a_Position" },
		{ OloEngine::ShaderDataType::Float4, "a_Color" }
	};
	vertexBuffer->SetLayout(layout);
	m_VertexArray->AddVertexBuffer(vertexBuffer);

	uint32_t indices[3] = { 0, 1, 2 };
	OloEngine::Ref<OloEngine::IndexBuffer> indexBuffer = OloEngine::IndexBuffer::Create(indices, sizeof(indices) / sizeof(uint32_t));
	m_VertexArray->SetIndexBuffer(indexBuffer);

	m_SquareVA = OloEngine::VertexArray::Create();

	float squareVertices[5 * 4] = {
		-0.5f, -0.5f, 0.0f, 0.0f, 0.0f,
		 0.5f, -0.5f, 0.0f, 1.0f, 0.0f,
		 0.5f,  0.5f, 0.0f, 1.0f, 1.0f,
		-0.5f,  0.5f, 0.0f, 0.0f, 1.0f
	};

	OloEngine::Ref<OloEngine::VertexBuffer> squareVB = OloEngine::VertexBuffer::Create(squareVertices, sizeof(squareVertices));
	squareVB->SetLayout({
		{ OloEngine::ShaderDataType::Float3, "a_Position" },
		{ OloEngine::ShaderDataType::Float2, "a_TexCoord" }
		});
	m_SquareVA->AddVertexBuffer(squareVB);

	uint32_t squareIndices[6] = { 0, 1, 2, 2, 3, 0 };
	OloEngine::Ref<OloEngine::IndexBuffer> squareIB = OloEngine::IndexBuffer::Create(squareIndices, sizeof(squareIndices) / sizeof(uint32_t));
	m_SquareVA->SetIndexBuffer(squareIB);

	std::string vertexSrc = R"(
			#version 330 core
			
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec4 a_Color;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_Position;
			out vec4 v_Color;

			void main()
			{
				v_Position = a_Position;
				v_Color = a_Color;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);	
			}
		)";

	std::string fragmentSrc = R"(
			#version 330 core
			
			layout(location = 0) out vec4 color;

			in vec3 v_Position;
			in vec4 v_Color;

			void main()
			{
				color = vec4(v_Position * 0.5 + 0.5, 1.0);
				color = v_Color;
			}
		)";

	m_Shader = OloEngine::Shader::Create("VertexPosColor", vertexSrc, fragmentSrc);

	std::string flatColorShaderVertexSrc = R"(
			#version 330 core
			
			layout(location = 0) in vec3 a_Position;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_Position;

			void main()
			{
				v_Position = a_Position;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);	
			}
		)";

	std::string flatColorShaderFragmentSrc = R"(
			#version 330 core
			
			layout(location = 0) out vec4 color;

			in vec3 v_Position;
			
			uniform vec3 u_Color;

			void main()
			{
				color = vec4(u_Color, 1.0);
			}
		)";

	m_FlatColorShader = OloEngine::Shader::Create("FlatColor", flatColorShaderVertexSrc, flatColorShaderFragmentSrc);

	auto textureShader = m_ShaderLibrary.Load("assets/shaders/Texture.glsl");

	m_Texture = OloEngine::Texture2D::Create("assets/textures/Checkerboard.png");
	m_ChernoLogoTexture = OloEngine::Texture2D::Create("assets/textures/ChernoLogo.png");

	textureShader->Bind();
	textureShader->SetInt("u_Texture", 0);
}

void ExampleLayer::OnAttach()
{
}

void ExampleLayer::OnDetach()
{
}

void ExampleLayer::OnUpdate(OloEngine::Timestep ts)
{
	// Update
	m_CameraController.OnUpdate(ts);

	// Render
	OloEngine::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
	OloEngine::RenderCommand::Clear();

	OloEngine::Renderer::BeginScene(m_CameraController.GetCamera());

	glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(0.1f));

	m_FlatColorShader->Bind();
	m_FlatColorShader->SetFloat3("u_Color", m_SquareColor);

	for (int y = 0; y < 20; y++)
	{
		for (int x = 0; x < 20; x++)
		{
			glm::vec3 pos(x * 0.11f, y * 0.11f, 0.0f);
			glm::mat4 transform = glm::translate(glm::mat4(1.0f), pos) * scale;
			OloEngine::Renderer::Submit(m_FlatColorShader, m_SquareVA, transform);
		}
	}

	auto textureShader = m_ShaderLibrary.Get("Texture");

	m_Texture->Bind();
	OloEngine::Renderer::Submit(textureShader, m_SquareVA, glm::scale(glm::mat4(1.0f), glm::vec3(1.5f)));
	m_ChernoLogoTexture->Bind();
	OloEngine::Renderer::Submit(textureShader, m_SquareVA, glm::scale(glm::mat4(1.0f), glm::vec3(1.5f)));

	// Triangle
	// OloEngine::Renderer::Submit(m_Shader, m_VertexArray);

	OloEngine::Renderer::EndScene();
}

void ExampleLayer::OnImGuiRender()
{
	ImGui::Begin("Settings");
	ImGui::ColorEdit3("Square Color", glm::value_ptr(m_SquareColor));
	ImGui::End();
}

void ExampleLayer::OnEvent(OloEngine::Event& e)
{
	m_CameraController.OnEvent(e);
}
