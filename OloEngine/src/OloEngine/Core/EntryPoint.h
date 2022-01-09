#pragma once
#include "OloEngine/Core/Core.h"
#include "OloEngine/Debug/Instrumentor.h"

#ifdef OLO_PLATFORM_WINDOWS

extern OloEngine::Application* OloEngine::CreateApplication();

int main(int argc, char** argv)
{
	OloEngine::Log::Init();
	
	OLO_PROFILE_BEGIN_SESSION("Startup", "OloProfile-Startup.json");
	auto app = OloEngine::CreateApplication();
	OLO_PROFILE_END_SESSION();

	OLO_PROFILE_BEGIN_SESSION("Runtime", "OloProfile-Runtime.json");
	app->Run();
	OLO_PROFILE_END_SESSION();

	OLO_PROFILE_BEGIN_SESSION("Startup", "OloProfile-Shutdown.json");
	delete app;
	OLO_PROFILE_END_SESSION();
}

#endif