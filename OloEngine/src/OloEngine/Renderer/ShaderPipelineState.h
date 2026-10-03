#pragma once

#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // The backend pipeline objects built from one shader (issue #607), shared
    // by Shader and ComputeShader. A backend that relinks a program in place
    // (OpenGL) has nothing to track and answers Tracked = false: a successful
    // Reload() is the whole answer there. A backend with a PSO cache (Vulkan)
    // answers how many pipelines its last successful reload invalidated, how
    // many have been built since, and whether any creation since has failed,
    // so a reload is reported done only once a pipeline reflects it.
    struct ShaderPipelineState
    {
        bool Tracked = false;
        u32 InvalidatedByLastReload = 0;
        u32 Live = 0;
        bool CreationFailed = false;
        FString CreationFailure; // reason, when CreationFailed
    };
} // namespace OloEngine
