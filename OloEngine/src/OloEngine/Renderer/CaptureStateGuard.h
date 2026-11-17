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
    // * CULL — the caller's, as the guard found it. These bakes draw with
    //   culling off; the guard used to end them with culling ON whatever it
    //   had been, and a bake called with culling off (a test, a tool, the first
    //   frames of a process) left it on for everything after. The sky bake drew
    //   its cube under that and lost half of the sky (#1352).
    // * RENDER-STATE CACHE — robustness rather than a live bug:
    //   CommandDispatch::ApplyPODRenderState early-outs when a draw's
    //   renderStateIndex equals the last applied, but ResetState() at
    //   BeginScene already invalidates that cache before any mid-frame bake
    //   runs. InvalidateRenderStateCache() keeps this true for a bake reached
    //   from anywhere else, and must run BEFORE any re-bind.
    //   (Uniform buffers need no such step: the dispatcher asks the backend
    //   which buffer occupies a binding point, issue #1688.)
    //
    // Runs on every exit path, including an ASSERT_* return inside a test.
    // Deliberately not GLStateGuard — see
    // docs/agent-rules/render-pass-published-state.md.
    struct CaptureStateGuard
    {
        Ref<Framebuffer>& m_Fbo;
        Viewport m_PrevViewport;
        bool m_PrevCulling;
        // A capture that rewrote the engine camera UBO (probe bakes) must put
        // it back; one that used its own UBO binding (the impostor bake) must
        // not touch it.
        bool m_RestoreCameraUBO;

        CaptureStateGuard(Ref<Framebuffer>& fbo, bool restoreCameraUBO)
            : m_Fbo(fbo), m_PrevViewport(RenderCommand::GetViewport()), m_PrevCulling(RenderCommand::IsCullingEnabled()),
              m_RestoreCameraUBO(restoreCameraUBO)
        {
        }

        CaptureStateGuard(const CaptureStateGuard&) = delete;
        CaptureStateGuard& operator=(const CaptureStateGuard&) = delete;

        ~CaptureStateGuard()
        {
            m_Fbo->Unbind();
            if (m_PrevCulling)
            {
                RenderCommand::EnableCulling();
            }
            else
            {
                RenderCommand::DisableCulling();
            }
            RenderCommand::SetViewport(m_PrevViewport.x, m_PrevViewport.y,
                                       m_PrevViewport.width, m_PrevViewport.height);
            CommandDispatch::InvalidateRenderStateCache();
            if (m_RestoreCameraUBO)
            {
                CommandDispatch::UploadCameraUBO();
            }
        }
    };

    // The fixed-function state of an immediate draw that covers its whole target
    // with one shape and nothing else: a cube seen from the inside (a sky bake,
    // an IBL convolution) or a fullscreen triangle (the BRDF table). Such a
    // draw needs no culling, no depth test and no blending, and it must not
    // READ them from whoever called it:
    //
    // * the skybox cube's faces do not all wind the same way seen from inside,
    //   so back-face culling drops half of them and front-face culling the
    //   other half -- the sky cube came out at 48% of its energy and the
    //   irradiance at 30%;
    // * the cube draws at the far plane, so a caller's depth func other than
    //   Less or LessOrEqual leaves the target at its clear colour;
    // * a caller's blend state mixes the result with that clear colour.
    //
    // Before this the bakes drew under whatever the previous draw of the
    // process had left, and were right only because that was culling off
    // (#1352). The caller's three switches go back on every exit.
    //
    // Blend is the GLOBAL flag. A per-attachment opinion
    // (RenderCommand::SetBlendStateForAttachment) outranks it by contract, so
    // this cannot turn blending off under a pass that holds one for the
    // target's attachment. Passes withdraw theirs before they return, and no
    // bake runs inside one.
    struct ScopedFullCoverDrawState
    {
        bool m_PrevCulling;
        bool m_PrevDepthTest;
        bool m_PrevBlend;

        ScopedFullCoverDrawState()
            : m_PrevCulling(RenderCommand::IsCullingEnabled()), m_PrevDepthTest(RenderCommand::IsDepthTestEnabled()),
              m_PrevBlend(RenderCommand::IsBlendEnabled())
        {
            RenderCommand::DisableCulling();
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetBlendState(false);
        }

        ScopedFullCoverDrawState(const ScopedFullCoverDrawState&) = delete;
        ScopedFullCoverDrawState& operator=(const ScopedFullCoverDrawState&) = delete;

        ~ScopedFullCoverDrawState()
        {
            if (m_PrevCulling)
            {
                RenderCommand::EnableCulling();
            }
            RenderCommand::SetDepthTest(m_PrevDepthTest);
            RenderCommand::SetBlendState(m_PrevBlend);
        }
    };
} // namespace OloEngine
