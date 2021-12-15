#pragma once

#ifdef OLO_PLATFORM_WINDOWS
extern OloEngine::Application* OloEngine::CreateApplication();

int main(int argc, char** argv)
{
	auto app = OloEngine::CreateApplication();
	app->Run();
	delete app;
}
#endif