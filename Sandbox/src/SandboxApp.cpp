// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "Sandbox2D.h"
#include "ExampleLayer.h"

class Sandbox : public OloEngine::Application
{
public:
	Sandbox(const OloEngine::ApplicationCommandLineArgs args)
	{
		// PushLayer(new ExampleLayer());
		PushLayer(new Sandbox2D());
	}

	~Sandbox() final = default;
};

OloEngine::Application* OloEngine::CreateApplication(const OloEngine::ApplicationCommandLineArgs args)
{
	return new Sandbox(args);
}
