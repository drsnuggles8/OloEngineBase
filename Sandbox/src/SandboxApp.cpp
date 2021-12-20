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
		if (OloEngine::Input::IsKeyPressed(OLO_KEY_TAB))
			OLO_TRACE("Tab key is pressed (poll)!");
	}

	void OnEvent(OloEngine::Event& event) override
	{
		if (event.GetEventType() == OloEngine::EventType::KeyPressed)
		{
			OloEngine::KeyPressedEvent& e = (OloEngine::KeyPressedEvent&)event;
			if (e.GetKeyCode() == OLO_KEY_TAB)
				OLO_TRACE("Tab key is pressed (event)!");
			OLO_TRACE("{0}", (char)e.GetKeyCode());
		}
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