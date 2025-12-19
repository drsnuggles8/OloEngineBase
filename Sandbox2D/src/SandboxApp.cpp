#include <OloEngine.h>
#include <OloEngine/Core/EntryPoint.h>

#include "Sandbox2D.h"

class Sandbox : public OloEngine::Application
{
  public:
    Sandbox(const OloEngine::ApplicationSpecification& specification)
        : OloEngine::Application(specification)
    {
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
