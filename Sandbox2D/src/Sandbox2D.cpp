#include <OloEnginePCH.h>
#include "Sandbox2D.h"
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

Sandbox2D::Sandbox2D()
	: Layer("Sandbox2D"), m_CameraController(1280.0f / 720.0f), m_SquareColor({ 0.2f, 0.3f, 0.8f, 1.0f })
{
}

void Sandbox2D::OnAttach()
{
	OLO_PROFILE_FUNCTION();

	m_CheckerboardTexture = OloEngine::Texture2D::Create("assets/textures/Checkerboard.png");
	m_OtterTexture = OloEngine::Texture2D::Create("assets/textures/Otter.png");
}

void Sandbox2D::OnDetach()
{
	OLO_PROFILE_FUNCTION();
}

void Sandbox2D::OnUpdate(const OloEngine::Timestep ts)
{
	OLO_PROFILE_FUNCTION();

	// Update
	m_CameraController.OnUpdate(ts);

	// Render
	OloEngine::Renderer2D::ResetStats();
	{
		OLO_PROFILE_SCOPE("Renderer Prep");
		OloEngine::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		OloEngine::RenderCommand::Clear();
	}

	static f32 rotation = 0.0f;
	rotation += ts * 50.0f;

	m_Timer += ts;

	if (m_Timer > 5.0f) // Switch scene every 5 seconds
	{
		m_Timer = 0.0f;
		m_Scene = (m_Scene + 1) % 4; // We have 4 scenes
	}

	{
		OLO_PROFILE_SCOPE("Renderer Draw");
		OloEngine::Renderer2D::BeginScene(m_CameraController.GetCamera());

	switch (m_Scene)
	{
		case 0:
			// Test Stencil Buffer Operations
			OloEngine::RenderCommand::EnableStencilTest();
			OloEngine::RenderCommand::SetStencilFunc(GL_ALWAYS, 1, 0xFF);
			OloEngine::RenderCommand::SetStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
			OloEngine::RenderCommand::Clear();

			// Draw a quad to the stencil buffer
			OloEngine::Renderer2D::DrawQuad({ -1.0f, 0.0f }, { 0.8f, 0.8f }, { 0.8f, 0.2f, 0.3f, 1.0f });

			// Use the stencil buffer to mask another quad
			OloEngine::RenderCommand::SetStencilFunc(GL_EQUAL, 1, 0xFF);
			OloEngine::RenderCommand::SetStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			OloEngine::Renderer2D::DrawQuad({ -1.0f, 0.0f }, { 0.5f, 0.5f }, { 0.2f, 0.8f, 0.3f, 1.0f });

			OloEngine::RenderCommand::DisableStencilTest();
			break;

		case 1:
			// Test Polygon Mode
			OloEngine::RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			OloEngine::Renderer2D::DrawQuad({ 1.0f, 0.0f }, { 0.8f, 0.8f }, { 0.8f, 0.2f, 0.3f, 1.0f });
			OloEngine::RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			break;

		case 2:
			// Test Scissor Test
			OloEngine::RenderCommand::EnableScissorTest();
			OloEngine::RenderCommand::SetScissorBox(100, 100, 200, 200);
			OloEngine::Renderer2D::DrawQuad({ 0.0f, 0.0f, -0.1f }, { 20.0f, 20.0f }, m_CheckerboardTexture, 10.0f);
			OloEngine::RenderCommand::DisableScissorTest();
			break;

		case 3:
			// Draw other shapes and quads
			OloEngine::Renderer2D::DrawRotatedQuad({ -2.0f, 0.0f, 0.0f }, { 1.0f, 1.0f }, rotation, m_OtterTexture, 20.0f);

			std::vector<glm::vec3> hexagonVertices = {
					{ 0.0f, 3.0f, 0.1f },
					{ -0.5f, 2.5f, 0.1f },
					{  0.5f, 2.5f, 0.1f },
					{  0.75f, 3.0f, 0.1f },
					{  0.5f, 3.5f, 0.1f },
					{ -0.5f, 3.5f, 0.1f },
					{ -0.75f, 3.0f, 0.1f },
					{ -0.5f, 2.5f, 0.1f }
			};

			OloEngine::Renderer2D::DrawPolygon(hexagonVertices, { 0.2f, 0.8f, 0.3f, 1.0f }, 10);

			for (f32 y = -5.0f; y < 5.0f; y += 0.5f)
			{
				for (f32 x = -5.0f; x < 5.0f; x += 0.5f)
				{
					const glm::vec4 color = { (x + 5.0f) / 10.0f, 0.4f, (y + 5.0f) / 10.0f, 0.7f };
					OloEngine::Renderer2D::DrawQuad({ x, y }, { 0.45f, 0.45f }, color);
				}
			}
			break;
	}

	OloEngine::Renderer2D::EndScene();
	}
}

void Sandbox2D::OnImGuiRender()
{
	OLO_PROFILE_FUNCTION();

	ImGui::Begin("Settings");

	const auto stats = OloEngine::Renderer2D::GetStats();
	ImGui::Text("Renderer2D Stats:");
	ImGui::Text("Draw Calls: %d", stats.DrawCalls);
	ImGui::Text("Quads: %d", stats.QuadCount);
	ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
	ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

	ImGui::ColorEdit4("Square Color", glm::value_ptr(m_SquareColor));

	ImGui::Text("Scene will switch in: %.1f seconds", 5.0f - m_Timer);

	ImGui::End();
}

void Sandbox2D::OnEvent(OloEngine::Event& e)
{
	m_CameraController.OnEvent(e);
}
