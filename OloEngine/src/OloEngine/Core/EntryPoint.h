#pragma once
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Debug/Instrumentor.h"

#ifdef OLO_PLATFORM_WINDOWS

extern OloEngine::Application* OloEngine::CreateApplication(ApplicationCommandLineArgs args);

int main(int argc, char** argv)
{
	OloEngine::Log::Init();

	OLO_PROFILE_BEGIN_SESSION("Startup", "OloProfile-Startup.json");
	auto* app = OloEngine::CreateApplication({ argc, argv });
	OLO_PROFILE_END_SESSION();

	OLO_PROFILE_BEGIN_SESSION("Runtime", "OloProfile-Runtime.json");
	app->Run();
	OLO_PROFILE_END_SESSION();

	OLO_PROFILE_BEGIN_SESSION("Shutdown", "OloProfile-Shutdown.json");
	delete app;
	OLO_PROFILE_END_SESSION();
}

#endif
