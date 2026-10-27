#pragma once

// =============================================================================
// TestRenderBackend — which RHI this test process renders scenes on (#1556).
//
// One process holds one backend: the Renderer3D statics do not survive a
// backend swap. The default process is OpenGL (a hidden GL 4.6 context from
// RenderPropertyFixture). `--olo-rhi=vulkan` makes a second process of the
// same binary that selects Vulkan before any test runs and renders the
// RendererAttachedTest family on a windowless VulkanContext
// (GraphicsContext::CreateWindowless): the same frame bracket a windowed
// session runs, against an offscreen target instead of a swapchain.
//
// In that process every scene frame must be recorded inside the context's
// frame bracket — a facade draw outside it is dropped — so the fixture runs
// each tick through RunFrame.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <string_view>

namespace OloEngine::Tests::TestRenderBackend
{
    // The windowless target's size. Only FinalRenderPass writes it; the
    // render graph's own targets are sized by RendererAttachedTest's
    // EnableRendering, as in the GL process.
    inline constexpr u32 kWindowlessTargetSize = 512u;

    // Called once from main(), after ParseTestOptions and before any test:
    // selects the --olo-rhi backend for the process. False (with the reason
    // on stderr) when the requested backend is not compiled into this binary.
    [[nodiscard]] bool SelectFromOptions();

    // True in a --olo-rhi=vulkan process.
    [[nodiscard]] bool IsVulkanProcess();

    // "GL_" or "Vulkan_": the <Backend> part of an evidence filename,
    // <Feature>_<Backend>_<Path>[_<Angle>].png (task-loop §2a), so the two
    // processes never write each other's cells.
    [[nodiscard]] const char* EvidenceBackendPrefix();
    // `feature` + "_" + EvidenceBackendPrefix(): "GroomSceneShadow_Vulkan_".
    [[nodiscard]] std::string EvidenceName(std::string_view feature);

    struct Admission
    {
        bool Available = false;
        // Why the context could not come up. Empty when Available.
        std::string Reason;
        // The device the context runs on, for the Vulkan coverage banner.
        std::string DeviceName;
    };

    // The Vulkan process's windowless context, brought up on the first call
    // (device probe, then GraphicsContext::CreateWindowless + Init) and the
    // cached verdict on every later one. The probe runs only before the
    // context exists: it re-initialises volk, which would drop a live
    // device's entry points. Only meaningful when IsVulkanProcess().
    [[nodiscard]] const Admission& EnsureWindowlessContext();

    struct FrameResult
    {
        // False when the frame bracket never ran the tick.
        bool Ran = false;
        // Vulkan only: validation-layer errors and VulkanRendererAPI
        // unimplemented-stub hits the frame added -- the Vulkan process's
        // counterpart of the GL error queue the GL tick drains.
        u64 NewValidationErrors = 0;
        u64 NewStubHits = 0;
    };

    // Runs `tick` as one frame. In the Vulkan process: inside the windowless
    // context's frame bracket (the frame callback of one SwapBuffers).
    // Elsewhere: inline.
    [[nodiscard]] FrameResult RunFrame(const std::function<void()>& tick);

    // Destroys the windowless context. main() calls it after
    // Renderer::Shutdown, which leaves the context to finish the teardown
    // (VulkanContext::RequestMemoryTrackerShutdown).
    void Shutdown();
} // namespace OloEngine::Tests::TestRenderBackend
