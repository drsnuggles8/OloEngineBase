#pragma once

#include "OloEngine.h"
#include "OloEngine/Renderer/Camera/PerspectiveCameraController.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"

class Sandbox3D : public OloEngine::Layer
{
public:
	Sandbox3D();
	~Sandbox3D() final = default;

	void OnAttach() override;
	void OnDetach() override;
	void OnUpdate(OloEngine::Timestep ts) override;
	void OnImGuiRender() override;
	void OnEvent(OloEngine::Event& e) override;

private:
	// UI helper functions for different light types
	void RenderDirectionalLightUI();
	void RenderPointLightUI();
	void RenderSpotlightUI();

private:
	OloEngine::PerspectiveCameraController m_CameraController;

	// Mesh objects
	OloEngine::Ref<OloEngine::Mesh> m_CubeMesh;
	OloEngine::Ref<OloEngine::Mesh> m_SphereMesh;
	OloEngine::Ref<OloEngine::Mesh> m_PlaneMesh;
	
	// Model object
	OloEngine::Ref<OloEngine::Model> m_BackpackModel;
	
	// Texture resources
	OloEngine::Ref<OloEngine::Texture2D> m_DiffuseMap;
	OloEngine::Ref<OloEngine::Texture2D> m_SpecularMap;
	OloEngine::Ref<OloEngine::Texture2D> m_GrassMap;  // Added grass texture

	// Rotation animation state
	f32 m_RotationAngleY = 0.0f;
	f32 m_RotationAngleX = 0.0f;

	// Materials for different objects
	OloEngine::Material m_BlueMaterial;
	OloEngine::Material m_RedMaterial;
	OloEngine::Material m_GreenMaterial;
	OloEngine::Material m_TexturedMaterial;

	// Light properties
	OloEngine::Light m_Light;
	int m_LightTypeIndex = 1; // Default to point light
	const char* m_LightTypeNames[3] = { "Directional Light", "Point Light", "Spotlight" };
	
	// Material editor selection state
	int m_SelectedMaterial = 0;
	const char* m_MaterialNames[4] = { "Blue Cube", "Red Cube", "Green Cube", "Textured Sphere" };

	// Light animation state
	float m_LightAnimTime = 0.0f;
	bool m_AnimateLight = true;

	// Input state tracking
	bool m_RotationEnabled = true;
	bool m_WasSpacePressed = false;
	bool m_CameraMovementEnabled = true;
	bool m_WasTabPressed = false;
	
	// Spotlight properties
	float m_SpotlightInnerAngle = 12.5f;
	float m_SpotlightOuterAngle = 17.5f;
	
	// Object type selection
	int m_PrimitiveTypeIndex = 0;
	const char* m_PrimitiveNames[3] = { "Cubes", "Spheres", "Mixed" };

	// FPS
	float m_FrameTime = 0.0f;
	float m_FPS = 0.0f;
};
