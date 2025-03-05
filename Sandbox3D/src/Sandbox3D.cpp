#include <OloEnginePCH.h>
#include "Sandbox3D.h"
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

Sandbox3D::Sandbox3D()
	: Layer("Sandbox3D"),
	m_CameraController(45.0f, 1280.0f / 720.0f, 0.1f, 1000.0f),
	m_RotationAngleY(0.0f),
	m_RotationAngleX(0.0f),
	m_RotationEnabled(true),
	m_WasSpacePressed(false)
{}

void Sandbox3D::OnAttach()
{
	OLO_PROFILE_FUNCTION();

	//m_CheckerboardTexture = OloEngine::Texture2D::Create("assets/textures/Checkerboard.png");
}

void Sandbox3D::OnDetach()
{
	OLO_PROFILE_FUNCTION();
}

void Sandbox3D::OnUpdate(const OloEngine::Timestep ts)
{
	OLO_PROFILE_FUNCTION();

	// Update camera
	m_CameraController.OnUpdate(ts);

	// Toggle rotation on spacebar press
	bool spacePressed = OloEngine::Input::IsKeyPressed(OloEngine::Key::Space);
	if (spacePressed && !m_WasSpacePressed)
		m_RotationEnabled = !m_RotationEnabled;
	m_WasSpacePressed = spacePressed;

	// Rotate only if enabled
	if (m_RotationEnabled)
	{
		m_RotationAngleY += ts * 45.0f; // 45 deg/s around Y
		m_RotationAngleX += ts * 30.0f; // 30 deg/s around X

		// Keep angles in [0, 360)
		if (m_RotationAngleY > 360.0f)  m_RotationAngleY -= 360.0f;
		if (m_RotationAngleX > 360.0f)  m_RotationAngleX -= 360.0f;
	}

	// Render setup
	{
		OLO_PROFILE_SCOPE("Renderer Prep");
		OloEngine::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		OloEngine::RenderCommand::Clear();
	}

	OLO_PROFILE_SCOPE("Renderer Draw");
	OloEngine::Renderer3D::BeginScene(m_CameraController.GetCamera().GetViewProjection());

	// Define a common white light color
	glm::vec3 lightColor(1.0f, 1.0f, 1.0f);

	// Cube 1: Blue cube (rotating around both axes)
	{
		auto modelMatrix = glm::mat4(1.0f);
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::vec3 objectColor(0.0f, 0.0f, 1.0f); // Blue
		OloEngine::Renderer3D::DrawCube(modelMatrix, objectColor, lightColor);
	}

	// Cube 2: Red cube (translated right and rotating)
	{
		auto modelMatrix = glm::mat4(1.0f);
		// Translate to the right by 2 units
		modelMatrix = glm::translate(modelMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
		// Apply a different rotation speed
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY * 1.5f), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::vec3 objectColor(1.0f, 0.0f, 0.0f); // Red
		OloEngine::Renderer3D::DrawCube(modelMatrix, objectColor, lightColor);
	}

	// Cube 3: Green cube (translated left and rotating)
	{
		auto modelMatrix = glm::mat4(1.0f);
		// Translate to the left by 2 units
		modelMatrix = glm::translate(modelMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
		// Apply a different rotation speed/axis
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
		glm::vec3 objectColor(0.0f, 1.0f, 0.0f); // Green
		OloEngine::Renderer3D::DrawCube(modelMatrix, objectColor, lightColor);
	}

	{
		auto lightCubeModelMatrix = glm::mat4(1.0f);
		lightCubeModelMatrix = glm::translate(lightCubeModelMatrix, glm::vec3(1.2f, 1.0f, 2.0f));
		lightCubeModelMatrix = glm::scale(lightCubeModelMatrix, glm::vec3(0.2f));
		OloEngine::Renderer3D::DrawLightCube(lightCubeModelMatrix);
	}

	OloEngine::Renderer3D::EndScene();
}

void Sandbox3D::OnImGuiRender()
{
	OLO_PROFILE_FUNCTION();
}

void Sandbox3D::OnEvent(OloEngine::Event& e)
{
	m_CameraController.OnEvent(e);

	if (e.GetEventType() == OloEngine::EventType::KeyPressed)
	{
		auto const& keyEvent = static_cast<OloEngine::KeyPressedEvent&>(e);
		if (keyEvent.GetKeyCode() == OloEngine::Key::Escape)
		{
			OloEngine::Application::Get().Close();
		}
	}
}
