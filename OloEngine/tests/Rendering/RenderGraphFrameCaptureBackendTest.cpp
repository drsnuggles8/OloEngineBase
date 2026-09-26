#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Debug/RenderGraphFrameCapture.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RendererAPI.h"

// OLO_TEST_LAYER: plumbing

// RenderGraphFrameCapture is OpenGL-only: its blit, backbuffer read-source
// selection and alpha swizzle are defined only in Platform/OpenGL
// (FrameCaptureBackend.h). The Render Graph Debugger auto-captures by default,
// so opening it on Vulkan installed the hook and the next pass executed a null GL
// entry point (found while verifying #607: EXCEPTION_ACCESS_VIOLATION at 0x0 in
// Detail::SetTextureAlphaSwizzleOne). These pin the refusal; no GPU needed, since
// the hook is refused before anything is captured.

namespace
{
    using namespace OloEngine;

    struct ScopedRendererApi
    {
        explicit ScopedRendererApi(RendererAPI::API api)
            : m_Previous(RendererAPI::GetAPI())
        {
            RendererAPI::SetAPI(api);
        }
        ~ScopedRendererApi()
        {
            RendererAPI::SetAPI(m_Previous);
        }
        RendererAPI::API m_Previous;
    };
} // namespace

TEST(RenderGraphFrameCaptureBackend, HookIsRefusedOffOpenGL)
{
    const ScopedRendererApi vulkan(RendererAPI::API::Vulkan);
    EXPECT_FALSE(RenderGraphFrameCapture::IsSupported());

    Ref<RenderGraph> graph = Ref<RenderGraph>::Create();
    RenderGraphFrameCapture capture;
    capture.InstallHook(graph.Raw());
    EXPECT_FALSE(capture.IsHookInstalled(graph.Raw()));
    EXPECT_FALSE(graph->HasPostPassHook());
}

TEST(RenderGraphFrameCaptureBackend, HookInstallsAndUninstallsOnOpenGL)
{
    const ScopedRendererApi openGL(RendererAPI::API::OpenGL);
    EXPECT_TRUE(RenderGraphFrameCapture::IsSupported());

    Ref<RenderGraph> graph = Ref<RenderGraph>::Create();
    RenderGraphFrameCapture capture;
    capture.InstallHook(graph.Raw());
    EXPECT_TRUE(capture.IsHookInstalled(graph.Raw()));
    EXPECT_TRUE(graph->HasPostPassHook());

    capture.InstallHook(nullptr);
    EXPECT_FALSE(graph->HasPostPassHook());
}
