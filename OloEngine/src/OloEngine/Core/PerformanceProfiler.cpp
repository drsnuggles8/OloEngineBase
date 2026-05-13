#include "OloEnginePCH.h"
#include "PerformanceProfiler.h"

#include "OloEngine/Core/Application.h"

namespace OloEngine
{
    PerformanceProfiler* GetGlobalPerformanceProfiler()
    {
        if (auto* app = Application::TryGet())
            return app->GetPerformanceProfiler();
        return nullptr;
    }
} // namespace OloEngine
