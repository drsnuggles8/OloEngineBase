#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    // RAII restore for an immediate-mode off-screen capture — a face bake that
    // binds its own framebuffer, sets its own viewport / cull / depth state and
    // draws outside the render graph (ImpostorBaker, ReflectionProbeBaker).
    //
    // Why a guard and not "the next draw re-applies its state anyway":
    //
    // * VIEWPORT — the real one. A tiled bake leaves it at the last tile's
    //   sub-rect. On GL the next framebuffer Bind re-issues glViewport; on
    //   Vulkan VulkanFramebuffer::Bind records nothing, the draw front-end
    //   reuses ctx.RecordedViewport whenever its width is non-zero, and
    //   SceneRenderPass never sets one — so a mid-frame bake made ScenePass
    //   rasterize the whole G-Buffer into a 256x256 corner for that frame.
    //   Observable only once the impostor bake actually drew on Vulkan (#1264).
    // * CULL / RENDER-STATE CACHE — parity with the sibling bakes, and
    //   robustness rather than a live bug: CommandDispatch::ApplyPODRenderState
    //   early-outs when a draw's renderStateIndex equals the last applied, but
    //   ResetState() at BeginScene already invalidates that cache before any
    //   mid-frame bake runs. InvalidateRenderStateCache() keeps this true for a
    //   bake reached from anywhere else, and must run BEFORE any re-bind or
    //   BindUBOIfNeeded thinks nothing changed.
    //
    // Runs on every exit path, including an ASSERT_* return inside a test.
    // Deliberately not GLStateGuard — see
    // docs/agent-rules/render-pass-published-state.md.
    struct CaptureStateGuard
    {
        Ref<Framebuffer>& m_Fbo;
        Viewport m_PrevViewport;
        // A capture that rewrote the engine camera UBO (probe bakes) must put
        // it back; one that used its own UBO binding (the impostor bake) must
        // not touch it.
        bool m_RestoreCameraUBO;

        CaptureStateGuard(Ref<Framebuffer>& fbo, bool restoreCameraUBO)
            : m_Fbo(fbo), m_PrevViewport(RenderCommand::GetViewport()), m_RestoreCameraUBO(restoreCameraUBO)
        {
        }

        CaptureStateGuard(const CaptureStateGuard&) = delete;
        CaptureStateGuard& operator=(const CaptureStateGuard&) = delete;

        ~CaptureStateGuard()
        {
            m_Fbo->Unbind();
            RenderCommand::EnableCulling();
            RenderCommand::SetViewport(m_PrevViewport.x, m_PrevViewport.y,
                                       m_PrevViewport.width, m_PrevViewport.height);
            CommandDispatch::InvalidateRenderStateCache();
            if (m_RestoreCameraUBO)
            {
                CommandDispatch::UploadCameraUBO();
            }
        }
    };
} // namespace OloEngine
