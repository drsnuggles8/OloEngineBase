#pragma once

#include "OloEngine.h"

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
	OloEngine::OrthographicCameraController m_CameraController;

	OloEngine::Ref<OloEngine::VertexArray> m_CubeVA;
	OloEngine::Ref<OloEngine::Shader> m_Shader;
	OloEngine::Ref<OloEngine::Texture2D> m_CubeTexture;
	OloEngine::Ref<OloEngine::Texture2D> m_LogoTexture;

	glm::vec4 m_CubeColor = { 0.2f, 0.3f, 0.8f, 1.0f };
    float m_RotationAngleY;
    float m_RotationAngleX;
    bool m_UseTexture = true;
    int m_TextureIndex = 0;
};
