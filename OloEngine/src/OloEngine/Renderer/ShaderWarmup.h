#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>

namespace OloEngine
{
    class Window;

    // Renders a minimal loading screen while shaders compile asynchronously.
    // Uses a tiny "boot" shader compiled synchronously before any async loads begin.
    class ShaderWarmup
    {
      public:
        // Compile the boot shader (must be called on the GL thread, before async loads).
        static void Init();

        // Render a single progress frame using the boot shader.
        // Call between shader Load() calls to give visual feedback during
        // CPU-side SPIR-V compilation.  Also updates the window title.
        // phase: 0=2D shaders, 1=3D shaders, 2=post-process, 3=linking
        static void RenderProgressFrame(f32 progress, Window& window, std::string_view label = "shaders",
                                        i32 current = 0, i32 total = 0, i32 phase = 0);

        // Block in a mini render loop, displaying a progress bar, until all
        // shaders in the given ShaderLibrary are ready. Polls OS events to
        // keep the window responsive.
        static void RunWarmupScreen(class ShaderLibrary& library, Window& window);

        // Release boot shader resources.
        static void Shutdown();
    };
} // namespace OloEngine
