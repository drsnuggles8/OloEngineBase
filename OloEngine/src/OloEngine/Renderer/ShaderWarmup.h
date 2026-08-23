#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class Window;
    class Shader;
    class ShaderLibrary;

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
        //
        // When `window` is nullptr (e.g. headless tests), this is a no-op.
        static void RenderProgressFrame(f32 progress, Window* window, std::string_view label = "shaders",
                                        i32 current = 0, i32 total = 0, i32 phase = 0);

        // Block in a mini render loop, displaying a progress bar, until all
        // shaders in the given ShaderLibrary are ready. Polls OS events to
        // keep the window responsive.
        //
        // When `window` is nullptr (e.g. headless tests), falls back to
        // library.FlushPendingShaders() (synchronous, no progress UI).
        static void RunWarmupScreen(class ShaderLibrary& library, Window* window);

        // Load every shader in `filepaths` into `library`, with the CPU-side
        // compile of independent shaders running in parallel across shaders
        // (issue #907 — see ShaderLibrary::PrepareParallel/FinalizeParallel).
        // The calling thread stays free to poll and redraw the progress bar
        // (and pump window events, via RenderProgressFrame -> PollEvents)
        // while that parallel CPU work runs on background tasks — same
        // "keep the window responsive while we wait" contract as
        // RunWarmupScreen above, just for the CPU tier instead of the GPU-
        // link tier. GL program creation/link itself still happens
        // sequentially back on this thread afterward, which MUST be the
        // render thread.
        //
        // When `window` is nullptr (e.g. headless tests), falls back to
        // library.LoadParallel() (synchronous, no progress UI, no polling).
        // `label`/`phase` are forwarded to RenderProgressFrame as-is.
        static std::vector<Ref<Shader>> LoadShadersParallel(
            ShaderLibrary& library, Window* window, const std::vector<std::string>& filepaths,
            std::string_view label, i32 phase);

        // Release boot shader resources.
        static void Shutdown();
    };
} // namespace OloEngine
