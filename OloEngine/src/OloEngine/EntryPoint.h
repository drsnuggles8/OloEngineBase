#pragma once

#ifdef OLO_PLATFORM_WINDOWS

extern OloEngine::Application* OloEngine::CreateApplication();

int main(int argc, char** argv)
{
	OloEngine::Log::Init();
	OLO_CORE_WARN("Initialized Log!");
	int a = 5;
	OLO_INFO("Hello! Var={0}", a);

	auto app = OloEngine::CreateApplication();
	app->Run();
	delete app;
}
#endif