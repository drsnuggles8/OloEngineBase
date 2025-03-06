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
	OloEngine::PerspectiveCameraController m_CameraController;

	OloEngine::Ref<OloEngine::VertexArray> m_CubeVA;
	OloEngine::Ref<OloEngine::Shader> m_Shader;
	OloEngine::Ref<OloEngine::Texture2D> m_CheckerboardTexture;

	f32 m_RotationAngleY;
	f32 m_RotationAngleX;

	// Materials for the three cubes
	OloEngine::Material m_BlueMaterial;
	OloEngine::Material m_RedMaterial;
	OloEngine::Material m_GreenMaterial;

	// Light properties
	OloEngine::Light m_Light;
	
	// Legacy lighting parameters (for backward compatibility)
	float m_AmbientStrength = 0.1f;
	float m_SpecularStrength = 0.5f;
	float m_Shininess = 32.0f;

	// Selected material for editing
	int m_SelectedMaterial = 0;
	const char* m_MaterialNames[3] = { "Blue Cube", "Red Cube", "Green Cube" };

	// Animation variables
	float m_LightAnimTime = 0.0f;
	bool m_AnimateLight = true;

	bool m_RotationEnabled;
	bool m_WasSpacePressed;

	// Camera control toggle
	bool m_CameraMovementEnabled;
	bool m_WasTabPressed;
};
