#include "OloEnginePCH.h"
#include "OloEngine.h"
#include "OloEngine/Core/EntryPoint.h"

#include "EditorLayer.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace OloEngine
{
    class OloEngineEditor : public Application
    {
      public:
        explicit OloEngineEditor(const ApplicationSpecification& spec, bool pushEditorLayer = true)
            : Application(spec)
        {
            // In `--smoke-test` mode the EditorLayer is skipped: it drives the
            // GL/ImGui editor UI which needs a real OpenGL 4.6 context, and the
            // app runs window-less there. See CreateApplication below.
            //
            // #691 Phase 7 (Final): the layer is pushed on BOTH backends now.
            // Under --rhi=vulkan it drives the scene and the render graph
            // exactly as on GL, and the graph's FinalPass blits to the
            // swapchain — so the window shows the rendered scene FULL SCREEN.
            // What is missing there is the editor UI itself: ImGui runs
            // platform-only (imgui_impl_opengl3 is GL-bound until Phase 8), so
            // panel logic executes but nothing is painted over the scene.
            if (pushEditorLayer)
            {
                PushLayer(std::make_unique<EditorLayer>());
                if (Renderer::GetAPI() != RendererAPI::API::OpenGL)
                {
                    OLO_CORE_INFO("[RHI] EditorLayer under --rhi=vulkan: full editor session — the scene "
                                  "renders through the graph and the ImGui editor UI composites over the "
                                  "swapchain via the Vulkan ImGui backend (#691 Phase 8)");
                }
            }
        }

        ~OloEngineEditor() final = default;
    };

    Application* CreateApplication(ApplicationCommandLineArgs const args)
    {
        ApplicationSpecification spec;
        spec.Name = "OloEditor";
        spec.IsEditor = true;
        spec.CommandLineArgs = args;

        // `--smoke-test`: validate that the binary launches and resolves all its
        // statically-imported runtime DLLs (FFmpeg etc.), then exit cleanly. The
        // missing-DLL bug class this guards against (issue #303) kills the process
        // in the OS loader before main() is even reached, so simply reaching a
        // clean exit proves the DLLs are present. The editor's GL/ImGui path needs
        // a real OpenGL 4.6 context that CI runners don't have, so smoke mode runs
        // window-less (IsHeadless) and skips the EditorLayer — mirroring the repo's
        // "degrade gracefully where there's no GL" convention.
        if (args.Contains("--smoke-test"))
        {
            spec.IsHeadless = true;
            spec.SmokeTestTickLimit = SmokeTestTickCount;
            return new OloEngineEditor(spec, /*pushEditorLayer=*/false);
        }

        return new OloEngineEditor(spec);
    }
} // namespace OloEngine
