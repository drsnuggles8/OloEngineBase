#pragma once

#include "OloEngine.h"

class ExampleLayer : public OloEngine::Layer
{
public:
	ExampleLayer();
	virtual ~ExampleLayer() = default;

	virtual void OnAttach() override;
	virtual void OnDetach() override;

	void OnUpdate(OloEngine::Timestep ts) override;
	virtual void OnImGuiRender() override;
	void OnEvent(OloEngine::Event& e) override;
private:
	OloEngine::ShaderLibrary m_ShaderLibrary;
	OloEngine::Ref<OloEngine::Shader> m_Shader;
	OloEngine::Ref<OloEngine::VertexArray> m_VertexArray;

	OloEngine::Ref<OloEngine::Shader> m_FlatColorShader;
	OloEngine::Ref<OloEngine::VertexArray> m_SquareVA;

	OloEngine::Ref<OloEngine::Texture2D> m_Texture, m_ChernoLogoTexture;

	OloEngine::OrthographicCameraController m_CameraController;
	glm::vec3 m_SquareColor = { 0.2f, 0.3f, 0.8f };
};
