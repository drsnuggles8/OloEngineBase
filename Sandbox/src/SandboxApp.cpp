// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "Sandbox2D.h"
#include "ExampleLayer.h"

class Sandbox : public OloEngine::Application
{
public:
	Sandbox(const OloEngine::ApplicationSpecification& specification)
		: OloEngine::Application(specification)
	{
		// PushLayer(new ExampleLayer());
		PushLayer(new Sandbox2D());
	}

	~Sandbox() final = default;
};

OloEngine::Application* OloEngine::CreateApplication(const OloEngine::ApplicationCommandLineArgs args)
{
	ApplicationSpecification spec;
	spec.Name = "Sandbox";
	spec.WorkingDirectory = "../OloEditor";
	spec.CommandLineArgs = args;

	return new Sandbox(spec);
}
