#pragma once
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Debug/CrashReporter.h"
#include "OloEngine/Debug/Instrumentor.h"

#ifdef OLO_PLATFORM_WINDOWS

extern OloEngine::Application* OloEngine::CreateApplication(ApplicationCommandLineArgs args);

int main(int argc, char** argv)
{
    OloEngine::Log::Init();
    OloEngine::CrashReporter::Init();

    int exitCode = EXIT_SUCCESS;

    OLO_PROFILE_BEGIN_SESSION("Startup", "OloProfile-Startup.json");
    auto* app = OloEngine::CreateApplication({ argc, argv });
    OLO_CORE_ASSERT(app, "Client application is null!");
    OLO_PROFILE_END_SESSION();

    try
    {
        OLO_PROFILE_BEGIN_SESSION("Runtime", "OloProfile-Runtime.json");
#ifdef OLO_HEADLESS
        app->RunHeadless();
#else
        if (app->IsHeadless())
        {
            app->RunHeadless();
        }
        else
        {
            app->Run();
        }
#endif
        OLO_PROFILE_END_SESSION();
    }
    catch (const std::exception& e)
    {
        OLO_PROFILE_END_SESSION();
        OloEngine::CrashReporter::ReportCaughtException(e);
        exitCode = EXIT_FAILURE;
    }
    catch (...)
    {
        OLO_PROFILE_END_SESSION();
        OloEngine::CrashReporter::ReportFatalError("Unknown exception caught in main loop");
        exitCode = EXIT_FAILURE;
    }

    OLO_PROFILE_BEGIN_SESSION("Shutdown", "OloProfile-Shutdown.json");
    delete app;
    OLO_PROFILE_END_SESSION();

    OloEngine::CrashReporter::Shutdown();

    return exitCode;
}

#endif
