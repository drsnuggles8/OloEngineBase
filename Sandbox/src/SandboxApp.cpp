#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "Sandbox2D.h"
#include "ExampleLayer.h"

class Sandbox : public OloEngine::Application
{
public:
	Sandbox()
	{
		// PushLayer(new ExampleLayer());
		PushLayer(new Sandbox2D());
	}

	~Sandbox()
	{
	}
};

OloEngine::Application* OloEngine::CreateApplication()
{
	return new Sandbox();
}