#include <OloEngine.h>

class ExampleLayer : public OloEngine::Layer
{
public:
	ExampleLayer()
		: Layer("Example")
	{
	}

	void OnUpdate() override
	{
		OLO_INFO("ExampleLayer::Update");
	}

	void OnEvent(OloEngine::Event& event) override
	{
		OLO_TRACE("{0}", event);
	}
};


class Sandbox : public OloEngine::Application
{
public:
	Sandbox()
	{
		PushLayer(new ExampleLayer());
		PushOverlay(new OloEngine::ImGuiLayer());
	}
	~Sandbox()
	{

	}

};

OloEngine::Application* OloEngine::CreateApplication()
{
	return new Sandbox();
}