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
	m_LightAnimTime(0.0f),
	m_RotationEnabled(true),
	m_WasSpacePressed(false),
	m_CameraMovementEnabled(true),
	m_WasTabPressed(false)
{
	// Initialize materials with colors
	m_BlueMaterial.Ambient = glm::vec3(0.0f, 0.0f, 0.1f);  // Dark blue
	m_BlueMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 0.8f);  // Blue
	m_BlueMaterial.Specular = glm::vec3(0.5f);             // White-ish specular
	m_BlueMaterial.Shininess = 32.0f;                      // Medium shininess

	m_RedMaterial.Ambient = glm::vec3(0.1f, 0.0f, 0.0f);   // Dark red
	m_RedMaterial.Diffuse = glm::vec3(0.8f, 0.0f, 0.0f);   // Red
	m_RedMaterial.Specular = glm::vec3(0.7f);              // Brighter specular
	m_RedMaterial.Shininess = 64.0f;                       // Shinier

	m_GreenMaterial.Ambient = glm::vec3(0.0f, 0.1f, 0.0f); // Dark green
	m_GreenMaterial.Diffuse = glm::vec3(0.0f, 0.8f, 0.0f); // Green
	m_GreenMaterial.Specular = glm::vec3(0.3f);            // Duller specular
	m_GreenMaterial.Shininess = 16.0f;                     // Less shiny

	// Initialize textured material
	m_TexturedMaterial.Ambient = glm::vec3(0.1f);          // Ambient multiplier
	m_TexturedMaterial.Diffuse = glm::vec3(1.0f);          // Full diffuse texture color
	m_TexturedMaterial.Specular = glm::vec3(1.0f);         // Full specular texture color
	m_TexturedMaterial.Shininess = 64.0f;                  // Shininess
	m_TexturedMaterial.UseTextureMaps = true;              // Enable texture mapping

	// Initialize light with default values
	m_Light.Position = glm::vec3(1.2f, 1.0f, 2.0f);
	m_Light.Ambient = glm::vec3(0.2f);
	m_Light.Diffuse = glm::vec3(0.5f);
	m_Light.Specular = glm::vec3(1.0f);
}

void Sandbox3D::OnAttach()
{
	OLO_PROFILE_FUNCTION();

	// Load textures
	m_DiffuseMap = OloEngine::Texture2D::Create("assets/textures/container2.png");
	m_SpecularMap = OloEngine::Texture2D::Create("assets/textures/container2_specular.png");

	// Assign textures to the material
	m_TexturedMaterial.DiffuseMap = m_DiffuseMap;
	m_TexturedMaterial.SpecularMap = m_SpecularMap;

	// Set initial lighting parameters
	OloEngine::Renderer3D::SetLight(m_Light);
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
		m_Light.Position.x = std::cos(m_LightAnimTime) * radius;
		m_Light.Position.z = std::sin(m_LightAnimTime) * radius;
		OloEngine::Renderer3D::SetLight(m_Light);
	}

	// Render setup
	{
		OLO_PROFILE_SCOPE("Renderer Prep");
		OloEngine::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		OloEngine::RenderCommand::Clear();
	}

	OLO_PROFILE_SCOPE("Renderer Draw");
	OloEngine::Renderer3D::BeginScene(m_CameraController.GetCamera().GetViewProjection());

	// Cube 1: Blue cube (rotating around both axes)
	{
		auto modelMatrix = glm::mat4(1.0f);
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
		OloEngine::Renderer3D::DrawCube(modelMatrix, m_BlueMaterial);
	}

	// Cube 2: Red cube (translated right and rotating)
	{
		auto modelMatrix = glm::mat4(1.0f);
		// Translate to the right by 2 units
		modelMatrix = glm::translate(modelMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
		// Apply a different rotation speed
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY * 1.5f), glm::vec3(0.0f, 1.0f, 0.0f));
		OloEngine::Renderer3D::DrawCube(modelMatrix, m_RedMaterial);
	}

	// Cube 3: Green cube (translated left and rotating)
	{
		auto modelMatrix = glm::mat4(1.0f);
		// Translate to the left by 2 units
		modelMatrix = glm::translate(modelMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
		// Apply a different rotation speed/axis
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
		OloEngine::Renderer3D::DrawCube(modelMatrix, m_GreenMaterial);
	}

	// Cube 4: Textured cube (with diffuse and specular maps)
	{
		auto modelMatrix = glm::mat4(1.0f);
		// Position above
		modelMatrix = glm::translate(modelMatrix, glm::vec3(0.0f, 2.0f, 0.0f));
		// Apply a rotation
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX * 0.8f), glm::vec3(1.0f, 0.0f, 0.0f));
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY * 0.8f), glm::vec3(0.0f, 1.0f, 0.0f));
		OloEngine::Renderer3D::DrawCube(modelMatrix, m_TexturedMaterial);
	}

	// Light cube (moving in a circle)
	{
		auto lightCubeModelMatrix = glm::mat4(1.0f);
		lightCubeModelMatrix = glm::translate(lightCubeModelMatrix, m_Light.Position);
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

	// Material selection
	ImGui::Separator();
	ImGui::Text("Material Properties");
	ImGui::Combo("Select Material", &m_SelectedMaterial, m_MaterialNames, 4);

	// Get the selected material based on the combo box selection
	OloEngine::Material* currentMaterial;
	switch (m_SelectedMaterial)
	{
		case 0:
			currentMaterial = &m_BlueMaterial;
			break;
		case 1:
			currentMaterial = &m_RedMaterial;
			break;
		case 2:
			currentMaterial = &m_GreenMaterial;
			break;
		case 3:
			currentMaterial = &m_TexturedMaterial;
			break;
		default:
			currentMaterial = &m_BlueMaterial;
	}

	// Edit the selected material
	if (m_SelectedMaterial == 3) // Textured material
	{
		// For textured material, show the texture map toggle
		ImGui::Checkbox("Use Texture Maps", &currentMaterial->UseTextureMaps);
		ImGui::Text("Shininess");
		ImGui::SliderFloat("##Shininess", &currentMaterial->Shininess, 1.0f, 128.0f);
		
		if (m_DiffuseMap)
			ImGui::Text("Diffuse Map: Loaded");
		else
			ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Diffuse Map: Not Found!");
		
		if (m_SpecularMap)
			ImGui::Text("Specular Map: Loaded");
		else 
			ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Specular Map: Not Found!");
	}
	else
	{
		// For solid color materials, show the color controls
		ImGui::ColorEdit3("Ambient", glm::value_ptr(currentMaterial->Ambient));
		ImGui::ColorEdit3("Diffuse", glm::value_ptr(currentMaterial->Diffuse));
		ImGui::ColorEdit3("Specular", glm::value_ptr(currentMaterial->Specular));
		ImGui::SliderFloat("Shininess", &currentMaterial->Shininess, 1.0f, 128.0f);
	}

	// Light properties
	ImGui::Separator();
	ImGui::Text("Light Properties");

	// If light is not animating, allow manual positioning
	if (!m_AnimateLight)
	{
		if (ImGui::DragFloat3("Light Position", glm::value_ptr(m_Light.Position), 0.1f))
		{
			OloEngine::Renderer3D::SetLight(m_Light);
		}
	}

	// Edit light colors
	bool lightChanged = false;
	lightChanged |= ImGui::ColorEdit3("Light Ambient", glm::value_ptr(m_Light.Ambient));
	lightChanged |= ImGui::ColorEdit3("Light Diffuse", glm::value_ptr(m_Light.Diffuse));
	lightChanged |= ImGui::ColorEdit3("Light Specular", glm::value_ptr(m_Light.Specular));

	if (lightChanged)
	{
		OloEngine::Renderer3D::SetLight(m_Light);
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
