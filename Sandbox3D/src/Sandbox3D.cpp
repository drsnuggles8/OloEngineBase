#include <format>

#include <OloEnginePCH.h>
#include "Sandbox3D.h"
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"

Sandbox3D::Sandbox3D()
	: Layer("Sandbox3D"),
	m_CameraController(45.0f, 1280.0f / 720.0f, 0.1f, 1000.0f)
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
	m_Light.Type = OloEngine::LightType::Point; // Default to point light
	m_Light.Position = glm::vec3(1.2f, 1.0f, 2.0f);
	m_Light.Direction = glm::vec3(0.0f, -1.0f, 0.0f); // Points downward
	m_Light.Ambient = glm::vec3(0.2f);
	m_Light.Diffuse = glm::vec3(0.5f);
	m_Light.Specular = glm::vec3(1.0f);
	
	// Point light attenuation defaults
	m_Light.Constant = 1.0f;
	m_Light.Linear = 0.09f;
	m_Light.Quadratic = 0.032f;
	
	// Spotlight defaults
	m_Light.CutOff = glm::cos(glm::radians(m_SpotlightInnerAngle));
	m_Light.OuterCutOff = glm::cos(glm::radians(m_SpotlightOuterAngle));
}

void Sandbox3D::OnAttach()
{
	OLO_PROFILE_FUNCTION();

	// Create 3D meshes
	m_CubeMesh = OloEngine::Mesh::CreateCube();
	m_SphereMesh = OloEngine::Mesh::CreateSphere();
	m_PlaneMesh = OloEngine::Mesh::CreatePlane(5.0f, 5.0f);

	// Load backpack model
	m_BackpackModel = OloEngine::CreateRef<OloEngine::Model>("assets/backpack/backpack.obj");

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

	// Animate the light position in a circular pattern (only for point and spot lights)
	if (m_AnimateLight && m_Light.Type != OloEngine::LightType::Directional)
	{
		m_LightAnimTime += ts;
		float radius = 3.0f;
		m_Light.Position.x = std::cos(m_LightAnimTime) * radius;
		m_Light.Position.z = std::sin(m_LightAnimTime) * radius;

		// For spotlights, make them always point toward the center
		if (m_Light.Type == OloEngine::LightType::Spot)
		{
			m_Light.Direction = -glm::normalize(m_Light.Position);
		}

		OloEngine::Renderer3D::SetLight(m_Light);
	}

	// Render setup
	{
		OLO_PROFILE_SCOPE("Renderer Prep");
		OloEngine::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		OloEngine::RenderCommand::Clear();
	}

	{
		OLO_PROFILE_SCOPE("Renderer Draw");
		OloEngine::Renderer3D::BeginScene(m_CameraController.GetCamera().GetViewProjection());
	}

	// Draw ground plane
	{
		auto planeMatrix = glm::mat4(1.0f);
		planeMatrix = glm::translate(planeMatrix, glm::vec3(0.0f, -1.0f, 0.0f));
		OloEngine::Material planeMaterial;
		planeMaterial.Ambient = glm::vec3(0.1f);
		planeMaterial.Diffuse = glm::vec3(0.3f);
		planeMaterial.Specular = glm::vec3(0.2f);
		planeMaterial.Shininess = 8.0f;
		OloEngine::Renderer3D::DrawMesh(m_PlaneMesh, planeMatrix, planeMaterial);
	}

	// Draw backpack model
	{
		auto modelMatrix = glm::mat4(1.0f);
		modelMatrix = glm::translate(modelMatrix, glm::vec3(0.0f, 1.0f, -2.0f)); // Raise it up and move it back
		modelMatrix = glm::scale(modelMatrix, glm::vec3(0.5f)); // Scale down to reasonable size
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
		m_BackpackModel->Draw(modelMatrix, m_TexturedMaterial);
	}

	// Draw cubes with stencil testing
	{
		// Enable stencil testing and set stencil function
		OloEngine::RenderCommand::EnableStencilTest();
		OloEngine::RenderCommand::SetStencilFunc(GL_ALWAYS, 1, 0xFF);
		OloEngine::RenderCommand::SetStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

		// Draw the center blue cube and write to the stencil buffer
		OloEngine::RenderCommand::SetStencilMask(0xFF);
		OloEngine::RenderCommand::ClearStencil();
		auto modelMatrix = glm::mat4(1.0f);
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
		modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
		OloEngine::Renderer3D::DrawMesh(m_CubeMesh, modelMatrix, m_BlueMaterial);

		// Disable writing to the stencil buffer
		OloEngine::RenderCommand::SetStencilMask(0x00);

		// Draw the outline
		OloEngine::RenderCommand::SetStencilFunc(GL_NOTEQUAL, 1, 0xFF);
		OloEngine::RenderCommand::SetLineWidth(3.0f);
		auto outlineMatrix = glm::scale(modelMatrix, glm::vec3(1.1f));
		OloEngine::Material outlineMaterial;
		outlineMaterial.Ambient = glm::vec3(1.0f, 1.0f, 0.0f); // Yellow outline
		outlineMaterial.Diffuse = glm::vec3(1.0f, 1.0f, 0.0f);
		outlineMaterial.Specular = glm::vec3(0.0f);
		outlineMaterial.Shininess = 1.0f;
		OloEngine::Renderer3D::DrawMesh(m_CubeMesh, outlineMatrix, outlineMaterial);

		// Reset stencil settings
		OloEngine::RenderCommand::SetStencilFunc(GL_ALWAYS, 1, 0xFF);
		OloEngine::RenderCommand::SetStencilMask(0xFF);
		OloEngine::RenderCommand::DisableStencilTest();
	}

	// Draw other objects
	switch (m_PrimitiveTypeIndex)
	{
		case 0: // Cubes
			// Draw right red cube
		{
			auto redCubeMatrix = glm::mat4(1.0f);
			redCubeMatrix = glm::translate(redCubeMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
			redCubeMatrix = glm::rotate(redCubeMatrix, glm::radians(m_RotationAngleY * 1.5f), glm::vec3(0.0f, 1.0f, 0.0f));
			OloEngine::Renderer3D::DrawMesh(m_CubeMesh, redCubeMatrix, m_RedMaterial);
		}

		// Draw left green cube
		{
			auto greenCubeMatrix = glm::mat4(1.0f);
			greenCubeMatrix = glm::translate(greenCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
			greenCubeMatrix = glm::rotate(greenCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
			OloEngine::Renderer3D::DrawMesh(m_CubeMesh, greenCubeMatrix, m_GreenMaterial);
		}
		break;

		case 1: // Spheres
			// Draw center blue sphere
		{
			auto modelMatrix = glm::mat4(1.0f);
			OloEngine::Renderer3D::DrawMesh(m_SphereMesh, modelMatrix, m_BlueMaterial);
		}

		// Draw right red sphere
		{
			auto redSphereMatrix = glm::mat4(1.0f);
			redSphereMatrix = glm::translate(redSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
			OloEngine::Renderer3D::DrawMesh(m_SphereMesh, redSphereMatrix, m_RedMaterial);
		}

		// Draw left green sphere
		{
			auto greenSphereMatrix = glm::mat4(1.0f);
			greenSphereMatrix = glm::translate(greenSphereMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
			OloEngine::Renderer3D::DrawMesh(m_SphereMesh, greenSphereMatrix, m_GreenMaterial);
		}
		break;

		case 2: // Mixed
		default:
			// Draw mixed shapes
		{
			// Draw right red sphere
			auto redSphereMatrix = glm::mat4(1.0f);
			redSphereMatrix = glm::translate(redSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
			OloEngine::Renderer3D::DrawMesh(m_SphereMesh, redSphereMatrix, m_RedMaterial);

			// Draw left green cube
			auto greenCubeMatrix = glm::mat4(1.0f);
			greenCubeMatrix = glm::translate(greenCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
			greenCubeMatrix = glm::rotate(greenCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
			OloEngine::Renderer3D::DrawMesh(m_CubeMesh, greenCubeMatrix, m_GreenMaterial);
		}
		break;
	}

	// Textured sphere (shared across all modes)
	{
		auto sphereMatrix = glm::mat4(1.0f);
		sphereMatrix = glm::translate(sphereMatrix, glm::vec3(0.0f, 2.0f, 0.0f));
		sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleX * 0.8f), glm::vec3(1.0f, 0.0f, 0.0f));
		sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleY * 0.8f), glm::vec3(0.0f, 1.0f, 0.0f));
		OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, m_TexturedMaterial);
	}

	// Light cube (only for point and spot lights)
	if (m_Light.Type != OloEngine::LightType::Directional)
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
	
	// Add scene object selection
	ImGui::Text("Scene Objects");
	ImGui::Combo("Primitive Types", &m_PrimitiveTypeIndex, m_PrimitiveNames, 3);
	ImGui::Separator();

	// Light type selection
	ImGui::Text("Light Type");
	if (bool lightTypeChanged = ImGui::Combo("##LightType", &m_LightTypeIndex, m_LightTypeNames, 3); lightTypeChanged)
	{
		// Update light type
		m_Light.Type = static_cast<OloEngine::LightType>(m_LightTypeIndex);

		// Disable animation for directional lights
		if (m_Light.Type == OloEngine::LightType::Directional && m_AnimateLight)
		{
			m_AnimateLight = false;
		}

		OloEngine::Renderer3D::SetLight(m_Light);
	}

	// Show different UI controls based on light type
	ImGui::Separator();
	ImGui::Text("Light Properties");
	
	using enum OloEngine::LightType;
	switch (m_Light.Type)
	{
		case Directional:
			RenderDirectionalLightUI();
			break;

		case Point:
			// Only show animation toggle for positional lights
			ImGui::Checkbox("Animate Light", &m_AnimateLight);
			RenderPointLightUI();
			break;

		case Spot:
			// Only show animation toggle for positional lights
			ImGui::Checkbox("Animate Light", &m_AnimateLight);
			RenderSpotlightUI();
			break;
	}

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
		ImGui::SliderFloat("##TexturedShininess", &currentMaterial->Shininess, 1.0f, 128.0f);
		
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
		ImGui::ColorEdit3(std::format("Ambient##Material{}", m_SelectedMaterial).c_str(), glm::value_ptr(currentMaterial->Ambient));
		ImGui::ColorEdit3(std::format("Diffuse##Material{}", m_SelectedMaterial).c_str(), glm::value_ptr(currentMaterial->Diffuse));
		ImGui::ColorEdit3(std::format("Specular##Material{}", m_SelectedMaterial).c_str(), glm::value_ptr(currentMaterial->Specular));
		ImGui::SliderFloat(std::format("Shininess##Material{}", m_SelectedMaterial).c_str(), &currentMaterial->Shininess, 1.0f, 128.0f);
	}

	ImGui::End();
}

void Sandbox3D::RenderDirectionalLightUI()
{
	ImGui::Text("Directional Light");
	
	// Direction control
	if (bool directionChanged = ImGui::DragFloat3("Direction##DirLight", glm::value_ptr(m_Light.Direction), 0.01f); directionChanged)
	{
		// Normalize direction
		if (glm::length(m_Light.Direction) > 0.0f)
		{
			m_Light.Direction = glm::normalize(m_Light.Direction);
		}
		else
		{
			m_Light.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
		}

		OloEngine::Renderer3D::SetLight(m_Light);
	}
	
	// Light colors
	bool lightChanged = false;
	lightChanged |= ImGui::ColorEdit3("Ambient##DirLight", glm::value_ptr(m_Light.Ambient));
	lightChanged |= ImGui::ColorEdit3("Diffuse##DirLight", glm::value_ptr(m_Light.Diffuse));
	lightChanged |= ImGui::ColorEdit3("Specular##DirLight", glm::value_ptr(m_Light.Specular));

	if (lightChanged)
	{
		OloEngine::Renderer3D::SetLight(m_Light);
	}
}

void Sandbox3D::RenderPointLightUI()
{
	ImGui::Text("Point Light");
	
	if (!m_AnimateLight)
	{
		// Position control (only if not animating)
		bool positionChanged = ImGui::DragFloat3("Position##PointLight", glm::value_ptr(m_Light.Position), 0.1f);
		if (positionChanged)
		{
			OloEngine::Renderer3D::SetLight(m_Light);
		}
	}
	
	// Light colors
	bool lightChanged = false;
	lightChanged |= ImGui::ColorEdit3("Ambient##PointLight", glm::value_ptr(m_Light.Ambient));
	lightChanged |= ImGui::ColorEdit3("Diffuse##PointLight", glm::value_ptr(m_Light.Diffuse));
	lightChanged |= ImGui::ColorEdit3("Specular##PointLight", glm::value_ptr(m_Light.Specular));
	
	// Attenuation factors
	ImGui::Text("Attenuation Factors");
	lightChanged |= ImGui::DragFloat("Constant##PointLight", &m_Light.Constant, 0.01f, 0.1f, 10.0f);
	lightChanged |= ImGui::DragFloat("Linear##PointLight", &m_Light.Linear, 0.001f, 0.0f, 1.0f);
	lightChanged |= ImGui::DragFloat("Quadratic##PointLight", &m_Light.Quadratic, 0.0001f, 0.0f, 1.0f);

	if (lightChanged)
	{
		OloEngine::Renderer3D::SetLight(m_Light);
	}
}

void Sandbox3D::RenderSpotlightUI()
{
	ImGui::Text("Spotlight");
	
	if (!m_AnimateLight)
	{
		// Position control (only if not animating)
		if (bool positionChanged = ImGui::DragFloat3("Position##Spotlight", glm::value_ptr(m_Light.Position), 0.1f); positionChanged)
		{
			OloEngine::Renderer3D::SetLight(m_Light);
		}
		
		// Direction control (only if not animating)
		if (bool directionChanged = ImGui::DragFloat3("Direction##Spotlight", glm::value_ptr(m_Light.Direction), 0.01f); directionChanged)
		{
			// Normalize direction
			if (glm::length(m_Light.Direction) > 0.0f)
			{
				m_Light.Direction = glm::normalize(m_Light.Direction);
			}
			else
			{
				m_Light.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
			}

			OloEngine::Renderer3D::SetLight(m_Light);
		}
	}
	else
	{
		ImGui::Text("Light Direction: Auto (points to center)");
	}
	
	// Light colors
	bool lightChanged = false;
	lightChanged |= ImGui::ColorEdit3("Ambient##Spotlight", glm::value_ptr(m_Light.Ambient));
	lightChanged |= ImGui::ColorEdit3("Diffuse##Spotlight", glm::value_ptr(m_Light.Diffuse));
	lightChanged |= ImGui::ColorEdit3("Specular##Spotlight", glm::value_ptr(m_Light.Specular));
	
	// Attenuation factors
	ImGui::Text("Attenuation Factors");
	lightChanged |= ImGui::DragFloat("Constant##Spotlight", &m_Light.Constant, 0.01f, 0.1f, 10.0f);
	lightChanged |= ImGui::DragFloat("Linear##Spotlight", &m_Light.Linear, 0.001f, 0.0f, 1.0f);
	lightChanged |= ImGui::DragFloat("Quadratic##Spotlight", &m_Light.Quadratic, 0.0001f, 0.0f, 1.0f);
	
	// Spotlight cutoff angles
	ImGui::Text("Spotlight Angles");
	bool cutoffChanged = false;
	cutoffChanged |= ImGui::SliderFloat("Inner Cone", &m_SpotlightInnerAngle, 0.0f, 90.0f);
	cutoffChanged |= ImGui::SliderFloat("Outer Cone", &m_SpotlightOuterAngle, 0.0f, 90.0f);
	
	if (cutoffChanged)
	{
		// Make sure inner angle is less than or equal to outer angle
		m_SpotlightInnerAngle = std::min(m_SpotlightInnerAngle, m_SpotlightOuterAngle);
		
		// Convert angles to cosines
		m_Light.CutOff = glm::cos(glm::radians(m_SpotlightInnerAngle));
		m_Light.OuterCutOff = glm::cos(glm::radians(m_SpotlightOuterAngle));
		
		lightChanged = true;
	}

	if (lightChanged)
	{
		OloEngine::Renderer3D::SetLight(m_Light);
	}
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
