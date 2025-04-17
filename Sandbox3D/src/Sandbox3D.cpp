#include <format>

#include <OloEnginePCH.h>
#include "Sandbox3D.h"
#include <imgui.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Renderer3D.h"

Sandbox3D::Sandbox3D()
	: Layer("Sandbox3D"),
	m_CameraController(45.0f, 1280.0f / 720.0f, 0.1f, 1000.0f)
{
	// Initialize materials with metallic properties
	m_GoldMaterial.Ambient = glm::vec3(0.24725f, 0.1995f, 0.0745f);
	m_GoldMaterial.Diffuse = glm::vec3(0.75164f, 0.60648f, 0.22648f);
	m_GoldMaterial.Specular = glm::vec3(0.628281f, 0.555802f, 0.366065f);
	m_GoldMaterial.Shininess = 51.2f;

	m_SilverMaterial.Ambient = glm::vec3(0.19225f, 0.19225f, 0.19225f);
	m_SilverMaterial.Diffuse = glm::vec3(0.50754f, 0.50754f, 0.50754f);
	m_SilverMaterial.Specular = glm::vec3(0.508273f, 0.508273f, 0.508273f);
	m_SilverMaterial.Shininess = 76.8f;

	m_ChromeMaterial.Ambient = glm::vec3(0.25f, 0.25f, 0.25f);
	m_ChromeMaterial.Diffuse = glm::vec3(0.4f, 0.4f, 0.4f);
	m_ChromeMaterial.Specular = glm::vec3(0.774597f, 0.774597f, 0.774597f);
	m_ChromeMaterial.Shininess = 96.0f;

	// Initialize textured material
	m_TexturedMaterial.Ambient = glm::vec3(0.1f);
	m_TexturedMaterial.Diffuse = glm::vec3(1.0f);
	m_TexturedMaterial.Specular = glm::vec3(1.0f);
	m_TexturedMaterial.Shininess = 64.0f;
	m_TexturedMaterial.UseTextureMaps = true;              // Enable texture mapping

	// Initialize light with default values
	m_Light.Type = OloEngine::LightType::Point;
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
    m_PlaneMesh = OloEngine::Mesh::CreatePlane(25.0f, 25.0f);
    
    // Load backpack model
    m_BackpackModel = OloEngine::CreateRef<OloEngine::Model>("assets/backpack/backpack.obj");
    
    // Load textures
    m_DiffuseMap = OloEngine::Texture2D::Create("assets/textures/container2.png");
    m_SpecularMap = OloEngine::Texture2D::Create("assets/textures/container2_specular.png");
    m_GrassTexture = OloEngine::Texture2D::Create("assets/textures/grass.png");
    
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

	m_FrameTime = ts.GetMilliseconds();
	m_FPS = 1.0f / ts.GetSeconds();

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
		f32 radius = 3.0f;
		m_Light.Position.x = std::cos(m_LightAnimTime) * radius;
		m_Light.Position.z = std::sin(m_LightAnimTime) * radius;

		// For spotlights, make them always point toward the center
		if (m_Light.Type == OloEngine::LightType::Spot)
		{
			m_Light.Direction = -glm::normalize(m_Light.Position);
		}

		OloEngine::Renderer3D::SetLight(m_Light);
	}

	{
		OLO_PROFILE_SCOPE("Renderer Draw");
		OloEngine::Renderer3D::BeginScene(m_CameraController.GetCamera());

		// Draw ground plane
		{
			auto planeMatrix = glm::mat4(1.0f);
			planeMatrix = glm::translate(planeMatrix, glm::vec3(0.0f, -1.0f, 0.0f));
			OloEngine::Material planeMaterial;
			planeMaterial.Ambient = glm::vec3(0.1f);
			planeMaterial.Diffuse = glm::vec3(0.3f);
			planeMaterial.Specular = glm::vec3(0.2f);
			planeMaterial.Shininess = 8.0f;
			auto* cmd = OloEngine::Renderer3D::DrawMesh(m_PlaneMesh, planeMatrix, planeMaterial, true);
			OloEngine::Renderer3D::SubmitDrawCall(cmd);
		}

		// Draw a grass quad
		{
			auto grassMatrix = glm::mat4(1.0f);
			grassMatrix = glm::translate(grassMatrix, glm::vec3(0.0f, 0.5f, -1.0f));
			// Make it face the camera by rotating around X axis
			grassMatrix = glm::rotate(grassMatrix, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			auto* grassCmd = OloEngine::Renderer3D::DrawQuad(grassMatrix, m_GrassTexture);
			OloEngine::Renderer3D::SubmitDrawCall(grassCmd);
		}

		// Draw backpack model using command-based renderer
		{
			auto modelMatrix = glm::mat4(1.0f);
			modelMatrix = glm::translate(modelMatrix, glm::vec3(0.0f, 1.0f, -2.0f)); // Raise it up and move it back
			modelMatrix = glm::scale(modelMatrix, glm::vec3(0.5f)); // Scale down to reasonable size
			modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
			std::vector<OloEngine::DrawMeshCommand*> backpackDrawCommands;
			m_BackpackModel->GetDrawCommands(modelMatrix, m_TexturedMaterial, backpackDrawCommands);
			for (auto* cmd : backpackDrawCommands)
			{
				// Optionally configure per-mesh state here
				OloEngine::Renderer3D::SubmitDrawCall(cmd);
			}
		}

		// Draw cubes with stencil testing
		{
			auto modelMatrix = glm::mat4(1.0f);
			modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
			modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
			// Draw filled mesh (normal)
			auto* solidCmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, modelMatrix, m_GoldMaterial);
			if (solidCmd)
			{
				OloEngine::Renderer3D::SubmitDrawCall(solidCmd);
			}
			// Overlay wireframe
			OloEngine::Material wireMaterial;
			wireMaterial.Ambient = glm::vec3(0.0f);
			wireMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 0.0f);
			wireMaterial.Specular = glm::vec3(0.0f);
			wireMaterial.Shininess = 1.0f;
			auto* wireCmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, modelMatrix, wireMaterial);
			if (wireCmd)
			{
				wireCmd->renderState->PolygonMode.Mode = GL_LINE;
				wireCmd->renderState->LineWidth.Width = 2.5f; // Thicker line
				wireCmd->renderState->PolygonOffset.Enabled = true;
				wireCmd->renderState->PolygonOffset.Factor = -1.0f;
				wireCmd->renderState->PolygonOffset.Units = -1.0f;
				OloEngine::Renderer3D::SubmitDrawCall(wireCmd);
			}
		}

		// Draw other objects
		switch (m_PrimitiveTypeIndex)
		{
			case 0: // Cubes
				// Draw right silver cube
			{
				auto silverCubeMatrix = glm::mat4(1.0f);
				silverCubeMatrix = glm::translate(silverCubeMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
				silverCubeMatrix = glm::rotate(silverCubeMatrix, glm::radians(m_RotationAngleY * 1.5f), glm::vec3(0.0f, 1.0f, 0.0f));
				auto* cmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, silverCubeMatrix, m_SilverMaterial);
				if (cmd) OloEngine::Renderer3D::SubmitDrawCall(cmd);
			}

			// Draw left chrome cube
			{
				auto chromeCubeMatrix = glm::mat4(1.0f);
				chromeCubeMatrix = glm::translate(chromeCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
				chromeCubeMatrix = glm::rotate(chromeCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
				auto* cmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, chromeCubeMatrix, m_ChromeMaterial);
				if (cmd) OloEngine::Renderer3D::SubmitDrawCall(cmd);
			}
			break;

			case 1: // Spheres
				// Draw center gold sphere
			{
				auto modelMatrix = glm::mat4(1.0f);
				auto* cmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, modelMatrix, m_GoldMaterial);
				OloEngine::Renderer3D::SubmitDrawCall(cmd);
			}

			// Draw right silver sphere
			{
				auto silverSphereMatrix = glm::mat4(1.0f);
				silverSphereMatrix = glm::translate(silverSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
				auto* cmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, silverSphereMatrix, m_SilverMaterial);
				OloEngine::Renderer3D::SubmitDrawCall(cmd);
			}

			// Draw left chrome sphere
			{
				auto chromeSphereMatrix = glm::mat4(1.0f);
				chromeSphereMatrix = glm::translate(chromeSphereMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
				auto* cmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, chromeSphereMatrix, m_ChromeMaterial);
				OloEngine::Renderer3D::SubmitDrawCall(cmd);
			}
			break;

			case 2: // Mixed
			default:
			{
				// Draw right silver sphere
				auto silverSphereMatrix = glm::mat4(1.0f);
				silverSphereMatrix = glm::translate(silverSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
				auto* cmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, silverSphereMatrix, m_SilverMaterial);
				OloEngine::Renderer3D::SubmitDrawCall(cmd);

				// Draw left chrome cube
				auto chromeCubeMatrix = glm::mat4(1.0f);
				chromeCubeMatrix = glm::translate(chromeCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
				chromeCubeMatrix = glm::rotate(chromeCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
				auto* cmdChrome = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, chromeCubeMatrix, m_ChromeMaterial);
				OloEngine::Renderer3D::SubmitDrawCall(cmdChrome);
			}
			break;
		}

		// Textured sphere (shared across all modes)
		{
			auto sphereMatrix = glm::mat4(1.0f);
			sphereMatrix = glm::translate(sphereMatrix, glm::vec3(0.0f, 2.0f, 0.0f));
			sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleX * 0.8f), glm::vec3(1.0f, 0.0f, 0.0f));
			sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleY * 0.8f), glm::vec3(0.0f, 1.0f, 0.0f));
			auto* cmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, m_TexturedMaterial);
			if (cmd) OloEngine::Renderer3D::SubmitDrawCall(cmd);
				
		}

		// Light cube (only for point and spot lights)
		if (m_Light.Type != OloEngine::LightType::Directional)
		{
			auto lightCubeModelMatrix = glm::mat4(1.0f);
			lightCubeModelMatrix = glm::translate(lightCubeModelMatrix, m_Light.Position);
			lightCubeModelMatrix = glm::scale(lightCubeModelMatrix, glm::vec3(0.2f));
			auto* cmdLightCube = OloEngine::Renderer3D::DrawLightCube(lightCubeModelMatrix);
			OloEngine::Renderer3D::SubmitDrawCall(cmdLightCube);
		}

		// Draw our state test objects to demonstrate the new state system
		if (m_EnableStateTest)
		{
			RenderStateTestObjects(m_RotationAngleY);
		}
	}

	OloEngine::Renderer3D::EndScene();
}

void Sandbox3D::RenderGraphDebuggerUI()
{
    OLO_PROFILE_FUNCTION();
    
    if (m_RenderGraphDebuggerOpen)
    {
        // Get the renderer's active render graph
        auto renderGraph = OloEngine::Renderer3D::GetRenderGraph();
        if (renderGraph)
        {
            m_RenderGraphDebugger.RenderDebugView(renderGraph, &m_RenderGraphDebuggerOpen, "Render Graph");
        }
        else
        {
            ImGui::Begin("Render Graph", &m_RenderGraphDebuggerOpen);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No active render graph available!");
            if (ImGui::Button("Close"))
                m_RenderGraphDebuggerOpen = false;
            ImGui::End();
        }
    }
}

void Sandbox3D::OnImGuiRender()
{
    OLO_PROFILE_FUNCTION();

    // Render the RenderGraph debugger window if open
    RenderGraphDebuggerUI();

    ImGui::Begin("Lighting Settings");

    // Display frametime and FPS
    ImGui::Text("Frametime: %.2f ms", m_FrameTime);
    ImGui::Text("FPS: %.2f", m_FPS);
    
    // Add render graph button at the top
    if (ImGui::Button("Show Render Graph"))
    {
        m_RenderGraphDebuggerOpen = true;
    }
    
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

	// Add a section for frustum culling settings
	ImGui::Separator();
	ImGui::Text("Frustum Culling");
	ImGui::Indent();
	
	bool frustumCullingEnabled = OloEngine::Renderer3D::IsFrustumCullingEnabled();
	if (ImGui::Checkbox("Enable Frustum Culling", &frustumCullingEnabled))
	{
		OloEngine::Renderer3D::EnableFrustumCulling(frustumCullingEnabled);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted("Enables frustum culling to skip rendering objects outside the camera view.");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
	
	bool dynamicCullingEnabled = OloEngine::Renderer3D::IsDynamicCullingEnabled();
	if (ImGui::Checkbox("Cull Dynamic Objects", &dynamicCullingEnabled))
	{
		OloEngine::Renderer3D::EnableDynamicCulling(dynamicCullingEnabled);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted("Warning: Enabling this may cause visual glitches with rotating objects.");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
	
	if (ImGui::Button("Reset to Defaults"))
	{
		OloEngine::Renderer3D::EnableFrustumCulling(true);
		OloEngine::Renderer3D::EnableDynamicCulling(false);
	}
	
	ImGui::Text("Meshes: Total %d, Culled %d (%.1f%%)", 
		OloEngine::Renderer3D::GetStats().TotalMeshes, 
		OloEngine::Renderer3D::GetStats().CulledMeshes,
		OloEngine::Renderer3D::GetStats().TotalMeshes > 0 
			? 100.0f * OloEngine::Renderer3D::GetStats().CulledMeshes / OloEngine::Renderer3D::GetStats().TotalMeshes 
			: 0.0f);
	
	ImGui::Unindent();
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
			currentMaterial = &m_GoldMaterial;
			break;
		case 1:
			currentMaterial = &m_SilverMaterial;
			break;
		case 2:
			currentMaterial = &m_ChromeMaterial;
			break;
		case 3:
			currentMaterial = &m_TexturedMaterial;
			break;
		default:
			currentMaterial = &m_GoldMaterial;
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

	ImGui::Separator();
	ImGui::Text("State Management Test");
	ImGui::Checkbox("Enable State Test", &m_EnableStateTest);
	
	if (m_EnableStateTest)
	{
		ImGui::Combo("Test Mode", &m_StateTestMode, m_StateTestModes, 4);
		
		ImGui::Checkbox("Use Queued State Changes", &m_UseQueuedStateChanges);
		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::Text("This option doesn't do anything yet - we're always using the queue now");
			ImGui::EndTooltip();
		}
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

void Sandbox3D::RenderStateTestObjects(f32 rotationAngle)
{
    // Position our state test objects in a specific area
    const glm::vec3 stateTestPosition(0.0f, 3.0f, 3.0f);
    // Draw a marker sphere to indicate where the state test area is
    {
        auto markerMatrix = glm::mat4(1.0f);
        markerMatrix = glm::translate(markerMatrix, stateTestPosition + glm::vec3(0.0f, 1.0f, 0.0f));
        markerMatrix = glm::scale(markerMatrix, glm::vec3(0.2f));
        OloEngine::Material markerMaterial;
        markerMaterial.Ambient = glm::vec3(1.0f, 0.0f, 0.0f);
        markerMaterial.Diffuse = glm::vec3(1.0f, 0.0f, 0.0f);
        markerMaterial.Specular = glm::vec3(1.0f);
        markerMaterial.Shininess = 32.0f;
        auto* cmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, markerMatrix, markerMaterial);
        if (cmd) OloEngine::Renderer3D::SubmitDrawCall(cmd);
    }
    switch (m_StateTestMode)
    {
        case 0: // Wireframe mode
        {
            for (int i = 0; i < 3; i++)
            {
                auto cubeMatrix = glm::mat4(1.0f);
                cubeMatrix = glm::translate(cubeMatrix, stateTestPosition + glm::vec3(i - 1, 0.0f, 0.0f));
                cubeMatrix = glm::rotate(cubeMatrix, glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
                OloEngine::Material cubeMaterial;
                cubeMaterial.Ambient = glm::vec3(0.1f);
                cubeMaterial.Diffuse = glm::vec3((i + 1) * 0.25f, 0.5f, 0.7f);
                cubeMaterial.Specular = glm::vec3(0.5f);
                cubeMaterial.Shininess = 32.0f;
                auto* cmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, cubeMaterial);
				if (cmd)
				{
					cmd->renderState->PolygonMode.Mode = GL_LINE;
					cmd->renderState->LineWidth.Width = 2.0f + i;
					OloEngine::Renderer3D::SubmitDrawCall(cmd);
				}
                
            }
            break;
        }
        case 1: // Alpha blending mode
        {
            for (int i = 0; i < 3; i++)
            {
                auto sphereMatrix = glm::mat4(1.0f);
                sphereMatrix = glm::translate(sphereMatrix, stateTestPosition + glm::vec3((i - 1) * 0.5f, 0.0f, 0.0f));
                sphereMatrix = glm::scale(sphereMatrix, glm::vec3(0.6f));
                OloEngine::Material sphereMaterial;
                sphereMaterial.Ambient = glm::vec3(0.1f);
                switch (i) {
                    case 0: sphereMaterial.Diffuse = glm::vec3(1.0f, 0.0f, 0.0f); break;
                    case 1: sphereMaterial.Diffuse = glm::vec3(0.0f, 1.0f, 0.0f); break;
                    case 2: sphereMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 1.0f); break;
                }
                sphereMaterial.Specular = glm::vec3(0.5f);
                sphereMaterial.Shininess = 32.0f;
                auto* cmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, sphereMaterial);
                if (cmd) {
                    // Set alpha blend state per draw call
                    cmd->renderState->Blend.Enabled = true;
                    cmd->renderState->Blend.SrcFactor = GL_SRC_ALPHA;
                    cmd->renderState->Blend.DstFactor = GL_ONE_MINUS_SRC_ALPHA;
                    OloEngine::Renderer3D::SubmitDrawCall(cmd);
                }
            }
            break;
        }
        case 2: // Polygon offset test
        {
            auto cubeMatrix = glm::mat4(1.0f);
            cubeMatrix = glm::translate(cubeMatrix, stateTestPosition);
            cubeMatrix = glm::rotate(cubeMatrix, glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
            cubeMatrix = glm::scale(cubeMatrix, glm::vec3(0.8f));
            OloEngine::Material solidMaterial;
            solidMaterial.Ambient = glm::vec3(0.1f);
            solidMaterial.Diffuse = glm::vec3(0.7f, 0.7f, 0.2f);
            solidMaterial.Specular = glm::vec3(0.5f);
            solidMaterial.Shininess = 32.0f;
            auto* solidCmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, solidMaterial);
            if (solidCmd) {
                OloEngine::Renderer3D::SubmitDrawCall(solidCmd);
            }
            // Overlay wireframe
            OloEngine::Material wireMaterial;
            wireMaterial.Ambient = glm::vec3(0.0f);
            wireMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 0.0f);
            wireMaterial.Specular = glm::vec3(0.0f);
            wireMaterial.Shininess = 1.0f;
            auto* wireCmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, wireMaterial);
            if (wireCmd) {
                wireCmd->renderState->PolygonMode.Mode = GL_LINE;
                wireCmd->renderState->LineWidth.Width = 1.5f;
                wireCmd->renderState->PolygonOffset.Enabled = true;
                wireCmd->renderState->PolygonOffset.Factor = -1.0f;
                wireCmd->renderState->PolygonOffset.Units = -1.0f;
                OloEngine::Renderer3D::SubmitDrawCall(wireCmd);
            }
            break;
        }
        case 3: // Combined effects
        {
            // Example: central wireframe sphere
            auto sphereMatrix = glm::mat4(1.0f);
            sphereMatrix = glm::translate(sphereMatrix, stateTestPosition);
            sphereMatrix = glm::rotate(sphereMatrix, glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
            OloEngine::Material wireMaterial;
            wireMaterial.Ambient = glm::vec3(0.1f);
            wireMaterial.Diffuse = glm::vec3(1.0f, 1.0f, 0.0f);
            wireMaterial.Specular = glm::vec3(1.0f);
            wireMaterial.Shininess = 32.0f;
            auto* wireCmd = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, wireMaterial);
            if (wireCmd) {
                wireCmd->renderState->PolygonMode.Mode = GL_LINE;
                wireCmd->renderState->LineWidth.Width = 2.0f;
                OloEngine::Renderer3D::SubmitDrawCall(wireCmd);
            }
            // Transparent cubes around sphere
            for (int i = 0; i < 3; i++)
            {
                f32 angle = glm::radians(rotationAngle + i * 120.0f);
                glm::vec3 offset(std::cos(angle), 0.0f, std::sin(angle));
                auto cubeMatrix = glm::mat4(1.0f);
                cubeMatrix = glm::translate(cubeMatrix, stateTestPosition + offset * 1.5f);
                cubeMatrix = glm::rotate(cubeMatrix, angle, glm::vec3(0.0f, 1.0f, 0.0f));
                cubeMatrix = glm::scale(cubeMatrix, glm::vec3(0.4f));
                OloEngine::Material glassMaterial;
                glassMaterial.Ambient = glm::vec3(0.1f);
                switch (i) {
                    case 0: glassMaterial.Diffuse = glm::vec3(1.0f, 0.0f, 0.0f); break;
                    case 1: glassMaterial.Diffuse = glm::vec3(0.0f, 1.0f, 0.0f); break;
                    case 2: glassMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 1.0f); break;
                }
                glassMaterial.Specular = glm::vec3(0.8f);
                glassMaterial.Shininess = 64.0f;
                auto* glassCmd = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, glassMaterial);
                if (glassCmd) {
                    glassCmd->renderState->Blend.Enabled = true;
                    glassCmd->renderState->Blend.SrcFactor = GL_SRC_ALPHA;
                    glassCmd->renderState->Blend.DstFactor = GL_ONE_MINUS_SRC_ALPHA;
                    OloEngine::Renderer3D::SubmitDrawCall(glassCmd);
                }
            }
            break;
        }
    }
}
