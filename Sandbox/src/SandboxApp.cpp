#include <OloEngine.h>

class Sandbox : public OloEngine::Application
{
public:
	Sandbox()
	{

	}
	~Sandbox()
	{

	}

};

OloEngine::Application* OloEngine::CreateApplication()
{
	return new Sandbox();
}