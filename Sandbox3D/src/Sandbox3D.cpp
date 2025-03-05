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
	m_WasSpacePressed(false),
	m_LightAnimTime(0.0f),
	m_CameraMovementEnabled(true),
	m_WasTabPressed(false)
{}

void Sandbox3D::OnAttach()
{
	OLO_PROFILE_FUNCTION();

	// Set initial lighting parameters
	OloEngine::Renderer3D::SetLightingParameters(m_AmbientStrength, m_SpecularStrength, m_Shininess);
	OloEngine::Renderer3D::SetLightPosition(m_LightPosition);
}

void Sandbox3D::OnDetach()
{
	OLO_PROFILE_FUNCTION();
}

void Sandbox3D::OnUpdate(const OloEngine::Timestep ts)
{
	OLO_PROFILE_FUNCTION();

	// Update camera only if camera movement is enabled
	if (m_CameraMovementEnabled)
	{
		m_CameraController.OnUpdate(ts);
	}

	// Check for Tab key press to toggle camera movement
	bool tabPressed = OloEngine::Input::IsKeyPressed(OloEngine::Key::Tab);
	if (tabPressed && !m_WasTabPressed)
	{
		m_CameraMovementEnabled = !m_CameraMovementEnabled;
		// Show a message when camera mode changes
		if (m_CameraMovementEnabled)
			OLO_INFO("Camera movement enabled");
		else
			OLO_INFO("Camera movement disabled - UI mode active");
	}
	m_WasTabPressed = tabPressed;

	// Update view position for specular highlights
	OloEngine::Renderer3D::SetViewPosition(m_CameraController.GetCamera().GetPosition());

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

	// Animate the light position in a circular pattern
	if (m_AnimateLight)
	{
		m_LightAnimTime += ts;
		float radius = 3.0f;
		m_LightPosition.x = std::cos(m_LightAnimTime) * radius;
		m_LightPosition.z = std::sin(m_LightAnimTime) * radius;
		OloEngine::Renderer3D::SetLightPosition(m_LightPosition);
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

	// Light cube (moving in a circle)
	{
		auto lightCubeModelMatrix = glm::mat4(1.0f);
		lightCubeModelMatrix = glm::translate(lightCubeModelMatrix, m_LightPosition);
		lightCubeModelMatrix = glm::scale(lightCubeModelMatrix, glm::vec3(0.2f));
		OloEngine::Renderer3D::DrawLightCube(lightCubeModelMatrix);
	}

	OloEngine::Renderer3D::EndScene();
}

void Sandbox3D::OnImGuiRender()
{
	OLO_PROFILE_FUNCTION();

	ImGui::Begin("Lighting Settings");

	// Add camera control status indicator
	if (!m_CameraMovementEnabled)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Camera Movement: DISABLED");
		ImGui::Text("Press TAB to re-enable camera movement");
		ImGui::Separator();
	}

	// Light animation toggle
	ImGui::Checkbox("Animate Light", &m_AnimateLight);

	// If light is not animating, allow manual positioning
	if (!m_AnimateLight)
	{
		if (ImGui::DragFloat3("Light Position", glm::value_ptr(m_LightPosition), 0.1f))
		{
			OloEngine::Renderer3D::SetLightPosition(m_LightPosition);
		}
	}

	// Phong model parameters
	bool parametersChanged = false;
	parametersChanged |= ImGui::SliderFloat("Ambient Strength", &m_AmbientStrength, 0.0f, 1.0f);
	parametersChanged |= ImGui::SliderFloat("Specular Strength", &m_SpecularStrength, 0.0f, 1.0f);
	parametersChanged |= ImGui::SliderFloat("Shininess", &m_Shininess, 1.0f, 128.0f);

	if (parametersChanged)
	{
		OloEngine::Renderer3D::SetLightingParameters(m_AmbientStrength, m_SpecularStrength, m_Shininess);
	}

	ImGui::End();
}

void Sandbox3D::OnEvent(OloEngine::Event& e)
{
	// Only process camera events if camera movement is enabled
	if (m_CameraMovementEnabled)
	{
		m_CameraController.OnEvent(e);
	}

	if (e.GetEventType() == OloEngine::EventType::KeyPressed)
	{
		auto const& keyEvent = static_cast<OloEngine::KeyPressedEvent&>(e);
		if (keyEvent.GetKeyCode() == OloEngine::Key::Escape)
		{
			OloEngine::Application::Get().Close();
		}
	}
}
