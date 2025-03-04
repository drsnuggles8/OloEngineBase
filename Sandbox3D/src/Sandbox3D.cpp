#include <OloEnginePCH.h>
#include "Sandbox3D.h"
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

Sandbox3D::Sandbox3D()
	: Layer("Sandbox3D"), m_CameraController(45.0f, 1280.0f / 720.0f, 0.1f, 1000.0f), m_RotationAngleY(0.0f), m_RotationAngleX(0.0f)
{}

void Sandbox3D::OnAttach()
{
	OLO_PROFILE_FUNCTION();

	// Load the checkerboard texture
	m_CheckerboardTexture = OloEngine::Texture2D::Create("assets/textures/Checkerboard.png");
}

void Sandbox3D::OnDetach()
{
	OLO_PROFILE_FUNCTION();
}

void Sandbox3D::OnUpdate(const OloEngine::Timestep ts)
{
	OLO_PROFILE_FUNCTION();

	// Update
	m_CameraController.OnUpdate(ts);

	// Update rotation angles based on time with different speeds for each axis
	m_RotationAngleY += ts * 45.0f; // 45 degrees per second around Y axis
	m_RotationAngleX += ts * 30.0f; // 30 degrees per second around X axis

	// Keep angles in the 0-360 range
	if (m_RotationAngleY > 360.0f)
		m_RotationAngleY -= 360.0f;
	if (m_RotationAngleX > 360.0f)
		m_RotationAngleX -= 360.0f;

	// Render
	{
		OLO_PROFILE_SCOPE("Renderer Prep");
		OloEngine::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		OloEngine::RenderCommand::Clear();
	}

	OLO_PROFILE_SCOPE("Renderer Draw");
	OloEngine::Renderer3D::BeginScene(m_CameraController.GetCamera().GetViewProjection());

	// Create a continuously rotating model matrix on multiple axes
	glm::mat4 modelMatrix = glm::mat4(1.0f);
	modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
	modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));

	// Draw a textured cube with the model matrix
	OloEngine::Renderer3D::Draw(modelMatrix, m_CheckerboardTexture);

	OloEngine::Renderer3D::EndScene();
}

void Sandbox3D::OnImGuiRender()
{
	OLO_PROFILE_FUNCTION();
}

void Sandbox3D::OnEvent(OloEngine::Event& e)
{
	m_CameraController.OnEvent(e);
}
