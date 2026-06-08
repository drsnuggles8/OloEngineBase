#pragma once
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Debug/CrashReporter.h"
#include "OloEngine/Debug/Instrumentor.h"

#if defined(OLO_PLATFORM_WINDOWS) || defined(OLO_PLATFORM_LINUX)

extern OloEngine::Application* OloEngine::CreateApplication(ApplicationCommandLineArgs args);

int main(int argc, char** argv)
{
    // Log is initialized automatically on first use (Meyer's singleton).
    // Force initialization here so log file + crash ring buffer are ready early.
    OloEngine::Log::Initialize();
    OloEngine::CrashReporter::Init();

    int exitCode = EXIT_SUCCESS;
    OloEngine::Application* app = nullptr;
    bool profileSessionActive = false;

    try
    {
        OLO_PROFILE_BEGIN_SESSION("Startup", "OloProfile-Startup.json");
        profileSessionActive = true;
        app = OloEngine::CreateApplication({ argc, argv });
        OLO_CORE_ASSERT(app, "Client application is null!");
        OLO_PROFILE_END_SESSION();
        profileSessionActive = false;

        OLO_PROFILE_BEGIN_SESSION("Runtime", "OloProfile-Runtime.json");
        profileSessionActive = true;
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
        // Launch-smoke-test mode (`--smoke-test`): the run loop auto-closes once
        // it has completed the configured ticks. If the app instead shut down
        // before that (e.g. a layer aborted startup), the launch did not fully
        // succeed — report it as a failure so CI / the in-suite test catch it.
        if (app->IsSmokeTest() && !app->SmokeTestPassed())
        {
            OLO_CORE_ERROR("[SmokeTest] FAILED — application shut down before completing startup validation.");
            exitCode = EXIT_FAILURE;
        }

        OLO_PROFILE_END_SESSION();
        profileSessionActive = false;
    }
    catch (const std::exception& e)
    {
        if (profileSessionActive)
        {
            OLO_PROFILE_END_SESSION();
            profileSessionActive = false;
        }
        OloEngine::CrashReporter::ReportCaughtException(e);
        exitCode = EXIT_FAILURE;
    }
    catch (...)
    {
        if (profileSessionActive)
        {
            OLO_PROFILE_END_SESSION();
            profileSessionActive = false;
        }
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
