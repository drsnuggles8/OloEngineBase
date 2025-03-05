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

	// Lighting parameters
	float m_AmbientStrength = 0.1f;
	float m_SpecularStrength = 0.5f;
	float m_Shininess = 32.0f;
	glm::vec3 m_LightPosition = { 1.2f, 1.0f, 2.0f };

	// Animation variables
	float m_LightAnimTime = 0.0f;
	bool m_AnimateLight = true;

	bool m_RotationEnabled;
	bool m_WasSpacePressed;

	// Camera control toggle
	bool m_CameraMovementEnabled;
	bool m_WasTabPressed;
};
