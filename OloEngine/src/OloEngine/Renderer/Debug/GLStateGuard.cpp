// =============================================================================
// GLStateGuard.cpp
//
// Backend-neutral shell (#691, ADR 0011 §1.6): the raw-GL snapshot
// capture and apply live in Platform/OpenGL/OpenGLStateGuard.cpp, reached
// through the free-function seam declared in StateGuardBackend.h. This TU
// keeps the backend gates, the CPU-side diff, and the guard's policy /
// finalisation logic — and stays free of <glad/gl.h> and Platform/ includes.
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Debug/StateGuardBackend.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include <algorithm>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        void AppendIfDifferent(std::vector<std::string>& out, std::string_view field, i64 a, i64 b)
        {
            if (a == b)
                return;
            std::ostringstream oss;
            oss << field << ": " << a << " -> " << b;
            out.emplace_back(oss.str());
        }

        void AppendIfDifferentBool(std::vector<std::string>& out, std::string_view field, bool a, bool b)
        {
            if (a == b)
                return;
            std::ostringstream oss;
            oss << field << ": " << (a ? "true" : "false") << " -> " << (b ? "true" : "false");
            out.emplace_back(oss.str());
        }
    } // namespace

    // -------------------------------------------------------------------------
    // GLStateSnapshot
    // -------------------------------------------------------------------------

    GLStateSnapshot GLStateSnapshot::Capture()
    {
        OLO_PROFILE_FUNCTION();

        // Same backend gate as the guard's ctor and ApplyCore (#691): every
        // read behind the seam is a raw glad call, and this entry point is
        // PUBLIC — the ctor's inertness promise has to hold here too, or a
        // direct Capture()/DetectLeaks() sails past it into null function
        // pointers.
        if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
        {
            return {};
        }

        return Detail::CaptureGLState();
    }

    std::vector<std::string> GLStateSnapshot::DiffAgainst(const GLStateSnapshot& other) const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<std::string> diffs;

        AppendIfDifferentBool(diffs, "DepthTest", m_DepthTest, other.m_DepthTest);
        AppendIfDifferentBool(diffs, "DepthMask", m_DepthMask, other.m_DepthMask);
        AppendIfDifferent(diffs, "DepthFunc", m_DepthFunc, other.m_DepthFunc);

        AppendIfDifferentBool(diffs, "Blend", m_Blend, other.m_Blend);
        AppendIfDifferent(diffs, "BlendSrcRgb", m_BlendSrcRgb, other.m_BlendSrcRgb);
        AppendIfDifferent(diffs, "BlendDstRgb", m_BlendDstRgb, other.m_BlendDstRgb);
        AppendIfDifferent(diffs, "BlendSrcAlpha", m_BlendSrcAlpha, other.m_BlendSrcAlpha);
        AppendIfDifferent(diffs, "BlendDstAlpha", m_BlendDstAlpha, other.m_BlendDstAlpha);
        AppendIfDifferent(diffs, "BlendEqRgb", m_BlendEqRgb, other.m_BlendEqRgb);
        AppendIfDifferent(diffs, "BlendEqAlpha", m_BlendEqAlpha, other.m_BlendEqAlpha);

        AppendIfDifferentBool(diffs, "StencilTest", m_StencilTest, other.m_StencilTest);
        AppendIfDifferent(diffs, "StencilFunc", m_StencilFunc, other.m_StencilFunc);
        AppendIfDifferent(diffs, "StencilRef", m_StencilRef, other.m_StencilRef);
        AppendIfDifferent(diffs, "StencilMask", static_cast<i64>(m_StencilMask), static_cast<i64>(other.m_StencilMask));
        AppendIfDifferent(diffs, "StencilBackFunc", m_StencilBackFunc, other.m_StencilBackFunc);
        AppendIfDifferent(diffs, "StencilBackRef", m_StencilBackRef, other.m_StencilBackRef);
        AppendIfDifferent(diffs, "StencilBackValueMask", static_cast<i64>(m_StencilBackValueMask), static_cast<i64>(other.m_StencilBackValueMask));
        AppendIfDifferent(diffs, "StencilWriteMask", static_cast<i64>(m_StencilWriteMask), static_cast<i64>(other.m_StencilWriteMask));
        AppendIfDifferent(diffs, "StencilBackWriteMask", static_cast<i64>(m_StencilBackWriteMask), static_cast<i64>(other.m_StencilBackWriteMask));
        AppendIfDifferent(diffs, "StencilFail", m_StencilFail, other.m_StencilFail);
        AppendIfDifferent(diffs, "StencilPassDepthFail", m_StencilPassDepthFail, other.m_StencilPassDepthFail);
        AppendIfDifferent(diffs, "StencilPassDepthPass", m_StencilPassDepthPass, other.m_StencilPassDepthPass);
        AppendIfDifferent(diffs, "StencilBackFail", m_StencilBackFail, other.m_StencilBackFail);
        AppendIfDifferent(diffs, "StencilBackPassDepthFail", m_StencilBackPassDepthFail, other.m_StencilBackPassDepthFail);
        AppendIfDifferent(diffs, "StencilBackPassDepthPass", m_StencilBackPassDepthPass, other.m_StencilBackPassDepthPass);

        AppendIfDifferentBool(diffs, "CullFace", m_CullFace, other.m_CullFace);
        AppendIfDifferent(diffs, "CullFaceMode", m_CullFaceMode, other.m_CullFaceMode);
        AppendIfDifferent(diffs, "FrontFace", m_FrontFace, other.m_FrontFace);

        AppendIfDifferentBool(diffs, "ScissorTest", m_ScissorTest, other.m_ScissorTest);
        AppendIfDifferent(diffs, "PolygonMode[Front]", m_PolygonMode[0], other.m_PolygonMode[0]);
        AppendIfDifferent(diffs, "PolygonMode[Back]", m_PolygonMode[1], other.m_PolygonMode[1]);

        for (u32 i = 0; i < 4; ++i)
        {
            std::ostringstream f;
            f << "Viewport[" << i << "]";
            AppendIfDifferent(diffs, f.str(), m_Viewport[i], other.m_Viewport[i]);
        }
        for (u32 i = 0; i < 4; ++i)
        {
            std::ostringstream f;
            f << "Scissor[" << i << "]";
            AppendIfDifferent(diffs, f.str(), m_Scissor[i], other.m_Scissor[i]);
        }

        AppendIfDifferent(diffs, "DrawFBO", static_cast<i64>(m_FboDraw), static_cast<i64>(other.m_FboDraw));
        AppendIfDifferent(diffs, "ReadFBO", static_cast<i64>(m_FboRead), static_cast<i64>(other.m_FboRead));
        AppendIfDifferent(diffs, "ActiveProgram", static_cast<i64>(m_ActiveProgram), static_cast<i64>(other.m_ActiveProgram));
        AppendIfDifferent(diffs, "VAO", static_cast<i64>(m_Vao), static_cast<i64>(other.m_Vao));
        AppendIfDifferent(diffs, "ActiveTextureUnit", static_cast<i64>(m_ActiveTextureUnit), static_cast<i64>(other.m_ActiveTextureUnit));

        // Honour the captured driver limits from both snapshots. Taking the
        // max of the two means a capture taken on a context that reports
        // fewer slots won't mask a binding that showed up in the other.
        const u32 textureDiffLimit = std::max(m_CapturedTextureSlotLimit, other.m_CapturedTextureSlotLimit);
        const u32 uboDiffLimit = std::max(m_CapturedUboSlotLimit, other.m_CapturedUboSlotLimit);

        for (u32 i = 0; i < textureDiffLimit; ++i)
        {
            std::ostringstream f2d;
            f2d << "Texture2D[" << i << "]";
            AppendIfDifferent(diffs, f2d.str(), static_cast<i64>(m_Textures2D[i]), static_cast<i64>(other.m_Textures2D[i]));
            std::ostringstream farr;
            farr << "Texture2DArray[" << i << "]";
            AppendIfDifferent(diffs, farr.str(), static_cast<i64>(m_Textures2DArray[i]), static_cast<i64>(other.m_Textures2DArray[i]));
            std::ostringstream fcube;
            fcube << "TextureCubeMap[" << i << "]";
            AppendIfDifferent(diffs, fcube.str(), static_cast<i64>(m_TexturesCubeMap[i]), static_cast<i64>(other.m_TexturesCubeMap[i]));
        }
        for (u32 i = 0; i < uboDiffLimit; ++i)
        {
            std::ostringstream f;
            f << "UBO[" << i << "]";
            AppendIfDifferent(diffs, f.str(), static_cast<i64>(m_UniformBuffers[i]), static_cast<i64>(other.m_UniformBuffers[i]));
        }

        return diffs;
    }

    void GLStateSnapshot::ApplyCore() const
    {
        OLO_PROFILE_FUNCTION();

        // Same backend gate as the GLStateGuard constructor, and for the same
        // reason: this is a GL-STATE instrument. The ctor's guard covers the
        // ctor/dtor path, but a pass may call ApplyCore() DIRECTLY on the
        // guard's entry snapshot to roll its own reconfiguration back before
        // the dtor's diff runs — PlanarReflectionRenderPass does exactly that
        // (#691). On a non-GL backend that snapshot is the
        // default-constructed one the inert ctor left behind, so the raw GL
        // calls behind the seam are at best meaningless and at worst fatal: in
        // a Vulkan-only process the glad pointers are NULL, and mid-suite (a GL
        // context alive alongside the Vulkan device) they would stomp the GL
        // context's real state with a snapshot that was never captured.
        if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
        {
            return;
        }

        Detail::ApplyGLStateCore(*this);
    }

    // -------------------------------------------------------------------------
    // GLStateGuard
    // -------------------------------------------------------------------------

    GLStateGuard::GLStateGuard(std::string_view passName, Policy policy)
        : m_PassName(passName), m_Policy(policy)
    {
        // The guard is a GL-STATE instrument: on any other backend there is
        // no GL state to capture and the glad pointers may be NULL (a
        // Vulkan-only process never loads GL), so Capture() would fault.
        // Finalise immediately — ctor and dtor become inert (#691
        // DeferredLighting / FluidComposite / Overdraw are the first
        // guard-carrying pass bodies to execute on the Vulkan backend).
        if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
        {
            m_Finalized = true;
            return;
        }

        // Note: we always capture on construction regardless of Policy.
        // Policy::Ignore still needs a valid m_EntryState because callers may
        // opt back into diff work via DetectLeaks() (see
        // GLStateGuardTest.EmptyRegionHasNoLeaks). The destructor alone is
        // what Policy::Ignore suppresses.
        m_EntryState = GLStateSnapshot::Capture();
    }

    GLStateGuard::~GLStateGuard()
    {
        OLO_PROFILE_FUNCTION();

        // Destructor must not throw: Capture() performs GL calls, DiffAgainst()
        // allocates std::string / std::ostringstream, and the logger macros
        // may throw from fmt formatting. Swallow any exception — a leaked GL
        // state diff is a debugging aid, not worth terminating the process.
        try
        {
            if (m_Finalized || m_Policy == Policy::Ignore)
                return;

            const GLStateSnapshot exit = GLStateSnapshot::Capture();
            if (const auto diffs = m_EntryState.DiffAgainst(exit); !diffs.empty())
            {
                // Policy::Restore is the "trust me to clean up" path —
                // mutations escaping into the destructor are EXPECTED for
                // passes that bind transient FBs/shaders/VAOs without
                // restoring them. The auto-rollback below makes it correct.
                // Trace-level logging keeps the diagnostic available for
                // when someone is actively leak-hunting (filter logs to
                // TRACE) without spamming WARN every frame. Log/Assert
                // policies remain loud since they signal explicit
                // "this should not happen" contracts.
                if (m_Policy == Policy::Restore)
                {
                    OLO_CORE_TRACE("GLStateGuard[{}]: {} state mutation(s) escaped the pass (restoring):", m_PassName, diffs.size());
                    for (const auto& d : diffs)
                        OLO_CORE_TRACE("    {}", d);
                }
                else
                {
                    OLO_CORE_ERROR("GLStateGuard[{}]: {} state mutation(s) escaped the pass:", m_PassName, diffs.size());
                    for (const auto& d : diffs)
                        OLO_CORE_ERROR("    {}", d);

                    if (m_Policy == Policy::Assert)
                    {
                        OLO_CORE_ASSERT(false, "GLStateGuard detected uncontained state mutation");
                    }
                }
            }

            if (m_Policy == Policy::Restore)
                m_EntryState.ApplyCore();
        }
        catch (...)
        {
            // Intentionally swallow — see comment above.
        }
    }

    std::vector<std::string> GLStateGuard::DetectLeaks()
    {
        OLO_PROFILE_FUNCTION();

        // Flip m_Finalized up-front so if Capture()/DiffAgainst() throw,
        // the destructor won't try the same work again during unwinding.
        m_Finalized = true;
        const GLStateSnapshot exit = GLStateSnapshot::Capture();
        return m_EntryState.DiffAgainst(exit);
    }
} // namespace OloEngine
