#pragma once

#include "OloEngine.h"
#include "OloEngine/Renderer/Camera/PerspectiveCameraController.h"

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

	// Rendering resources
	OloEngine::Ref<OloEngine::VertexArray> m_CubeVA;
	OloEngine::Ref<OloEngine::Shader> m_Shader;
	OloEngine::Ref<OloEngine::Texture2D> m_CheckerboardTexture;
	
	// Lighting texture maps
	OloEngine::Ref<OloEngine::Texture2D> m_DiffuseMap;
	OloEngine::Ref<OloEngine::Texture2D> m_SpecularMap;

	// Rotation animation state
	f32 m_RotationAngleY;
	f32 m_RotationAngleX;

	// Materials for different cubes
	OloEngine::Material m_BlueMaterial;
	OloEngine::Material m_RedMaterial;
	OloEngine::Material m_GreenMaterial;
	OloEngine::Material m_TexturedMaterial;

	// Light properties
	OloEngine::Light m_Light;
	int m_LightTypeIndex = 1; // Default to point light
	const char* m_LightTypeNames[3] = { "Directional Light", "Point Light", "Spotlight" };
	
	// Legacy lighting parameters
	float m_AmbientStrength = 0.1f;
	float m_SpecularStrength = 0.5f;
	float m_Shininess = 32.0f;

	// Material editor selection state
	int m_SelectedMaterial = 0;
	const char* m_MaterialNames[4] = { "Blue Cube", "Red Cube", "Green Cube", "Textured Cube" };

	// Light animation state
	float m_LightAnimTime = 0.0f;
	bool m_AnimateLight = true;

	// Input state tracking
	bool m_RotationEnabled;
	bool m_WasSpacePressed;
	bool m_CameraMovementEnabled;
	bool m_WasTabPressed;
	
	// Rendering options
	bool m_UseTextureMaps = true;
	
	// Spotlight properties
	float m_SpotlightInnerAngle = 12.5f;
	float m_SpotlightOuterAngle = 17.5f;
};
