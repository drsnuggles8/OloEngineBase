#pragma once

#include "OloEngine.h"

class Sandbox2D : public OloEngine::Layer
{
public:
	Sandbox2D();
	~Sandbox2D() final = default;

	void OnAttach() override;
	void OnDetach() override;

	void OnUpdate(OloEngine::Timestep ts) override;
	void OnImGuiRender() override;
	void OnEvent(OloEngine::Event& e) override;
private:
	OloEngine::OrthographicCameraController m_CameraController;

	// Temp
	OloEngine::Ref<OloEngine::VertexArray> m_SquareVA;
	OloEngine::Ref<OloEngine::Shader> m_FlatColorShader;

	OloEngine::Ref<OloEngine::Texture2D> m_CheckerboardTexture;

	glm::vec4 m_SquareColor = { 0.2f, 0.3f, 0.8f, 1.0f };
};
