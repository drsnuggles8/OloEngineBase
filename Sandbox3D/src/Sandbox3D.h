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
    f32 m_RotationAngleY;
    f32 m_RotationAngleX;
};
