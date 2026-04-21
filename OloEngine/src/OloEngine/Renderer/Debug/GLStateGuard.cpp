// =============================================================================
// GLStateGuard.cpp
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"

#include "OloEngine/Core/Log.h"

#include <glad/gl.h>

#include <algorithm>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        bool GlGetBoolean(GLenum cap)
        {
            GLboolean v = GL_FALSE;
            ::glGetBooleanv(cap, &v);
            return v != GL_FALSE;
        }

        i32 GlGetInt(GLenum name)
        {
            GLint v = 0;
            ::glGetIntegerv(name, &v);
            return static_cast<i32>(v);
        }

        u32 GlGetUInt(GLenum name)
        {
            GLint v = 0;
            ::glGetIntegerv(name, &v);
            return static_cast<u32>(v);
        }

        u32 GlGetIndexedUInt(GLenum name, u32 index)
        {
            GLint v = 0;
            ::glGetIntegeri_v(name, static_cast<GLuint>(index), &v);
            return static_cast<u32>(v);
        }

        u32 GlGetTextureBindingAtUnit(u32 unit, GLenum target)
        {
            GLint prevActive = 0;
            ::glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
            ::glActiveTexture(GL_TEXTURE0 + unit);
            GLint bound = 0;
            ::glGetIntegerv(target, &bound);
            ::glActiveTexture(static_cast<GLenum>(prevActive));
            return static_cast<u32>(bound);
        }

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

        GLStateSnapshot s;

        s.m_DepthTest = GlGetBoolean(GL_DEPTH_TEST);
        GLboolean dm = GL_TRUE;
        ::glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        s.m_DepthMask = dm != GL_FALSE;
        s.m_DepthFunc = GlGetInt(GL_DEPTH_FUNC);

        s.m_Blend = GlGetBoolean(GL_BLEND);
        s.m_BlendSrcRgb = GlGetInt(GL_BLEND_SRC_RGB);
        s.m_BlendDstRgb = GlGetInt(GL_BLEND_DST_RGB);
        s.m_BlendSrcAlpha = GlGetInt(GL_BLEND_SRC_ALPHA);
        s.m_BlendDstAlpha = GlGetInt(GL_BLEND_DST_ALPHA);
        s.m_BlendEqRgb = GlGetInt(GL_BLEND_EQUATION_RGB);
        s.m_BlendEqAlpha = GlGetInt(GL_BLEND_EQUATION_ALPHA);

        s.m_StencilTest = GlGetBoolean(GL_STENCIL_TEST);
        s.m_StencilFunc = GlGetInt(GL_STENCIL_FUNC);
        s.m_StencilRef = GlGetInt(GL_STENCIL_REF);
        s.m_StencilMask = static_cast<u32>(GlGetInt(GL_STENCIL_VALUE_MASK));

        s.m_CullFace = GlGetBoolean(GL_CULL_FACE);
        s.m_CullFaceMode = GlGetInt(GL_CULL_FACE_MODE);
        s.m_FrontFace = GlGetInt(GL_FRONT_FACE);

        s.m_ScissorTest = GlGetBoolean(GL_SCISSOR_TEST);

        // GL_POLYGON_MODE writes TWO ints (front, back) per the GL 4.6 spec,
        // even though the core profile constrains both to the same mode.
        // Passing a single-int destination would smash the stack.
        ::glGetIntegerv(GL_POLYGON_MODE, s.m_PolygonMode.data());

        ::glGetIntegerv(GL_VIEWPORT, s.m_Viewport.data());
        ::glGetIntegerv(GL_SCISSOR_BOX, s.m_Scissor.data());

        s.m_FboDraw = GlGetUInt(GL_DRAW_FRAMEBUFFER_BINDING);
        s.m_FboRead = GlGetUInt(GL_READ_FRAMEBUFFER_BINDING);
        s.m_ActiveProgram = GlGetUInt(GL_CURRENT_PROGRAM);
        s.m_Vao = GlGetUInt(GL_VERTEX_ARRAY_BINDING);
        // GL_ACTIVE_TEXTURE returns GL_TEXTUREi; the per-slot array above
        // only pins which texture is bound to each unit, not which unit is
        // active when the region exits. Without this a pass that swaps the
        // active unit and forgets to restore it will go undetected.
        s.m_ActiveTextureUnit = GlGetUInt(GL_ACTIVE_TEXTURE);

        // Clamp the per-slot loops against driver-reported limits so we
        // never query a binding point the driver does not expose. Software
        // renderers (llvmpipe, SwiftShader) sometimes report fewer than
        // the GL 4.6 guaranteed minimums.
        GLint driverTextureUnits = 0;
        GLint driverUboBindings = 0;
        // Use GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: m_Textures2D iterates over
        // texture *units* (which glActiveTexture / GL_TEXTURE_BINDING_2D
        // address), not per-stage sampler bindings. The combined limit is
        // the correct bound — GL_MAX_TEXTURE_IMAGE_UNITS is the
        // fragment-stage-only cap and underestimates units a pass can leak
        // state into (e.g. via compute or vertex-stage bindings).
        ::glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &driverTextureUnits);
        ::glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &driverUboBindings);
        const u32 textureSlotLimit = std::min<u32>(kTextureSlots, driverTextureUnits > 0 ? static_cast<u32>(driverTextureUnits) : kTextureSlots);
        const u32 uboSlotLimit = std::min<u32>(kUboSlots, driverUboBindings > 0 ? static_cast<u32>(driverUboBindings) : kUboSlots);
        s.m_CapturedTextureSlotLimit = textureSlotLimit;
        s.m_CapturedUboSlotLimit = uboSlotLimit;

        for (u32 i = 0; i < textureSlotLimit; ++i)
        {
            s.m_Textures2D[i] = GlGetTextureBindingAtUnit(i, GL_TEXTURE_BINDING_2D);
            s.m_Textures2DArray[i] = GlGetTextureBindingAtUnit(i, GL_TEXTURE_BINDING_2D_ARRAY);
            s.m_TexturesCubeMap[i] = GlGetTextureBindingAtUnit(i, GL_TEXTURE_BINDING_CUBE_MAP);
        }

        for (u32 i = 0; i < uboSlotLimit; ++i)
            s.m_UniformBuffers[i] = GlGetIndexedUInt(GL_UNIFORM_BUFFER_BINDING, i);

        return s;
    }

    std::vector<std::string> GLStateSnapshot::DiffAgainst(const GLStateSnapshot& other) const
    {
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

    // -------------------------------------------------------------------------
    // GLStateGuard
    // -------------------------------------------------------------------------

    GLStateGuard::GLStateGuard(std::string_view passName, Policy policy)
        : m_PassName(passName), m_EntryState(GLStateSnapshot::Capture()), m_Policy(policy)
    {
        // Note: we always capture on construction regardless of Policy.
        // Policy::Ignore still needs a valid m_EntryState because callers may
        // opt back into diff work via DetectLeaks() (see
        // GLStateGuardTest.EmptyRegionHasNoLeaks). The destructor alone is
        // what Policy::Ignore suppresses.
    }

    GLStateGuard::~GLStateGuard()
    {
        // Destructor must not throw: Capture() performs GL calls, DiffAgainst()
        // allocates std::string / std::ostringstream, and the logger macros
        // may throw from fmt formatting. Swallow any exception — a leaked GL
        // state diff is a debugging aid, not worth terminating the process.
        try
        {
            if (m_Finalized || m_Policy == Policy::Ignore)
                return;

            const GLStateSnapshot exit = GLStateSnapshot::Capture();
            const auto diffs = m_EntryState.DiffAgainst(exit);

            if (!diffs.empty())
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
        catch (...)
        {
            // Intentionally swallow — see comment above.
        }
    }

    std::vector<std::string> GLStateGuard::DetectLeaks()
    {
        // Flip m_Finalized up-front so if Capture()/DiffAgainst() throw,
        // the destructor won't try the same work again during unwinding.
        m_Finalized = true;
        const GLStateSnapshot exit = GLStateSnapshot::Capture();
        return m_EntryState.DiffAgainst(exit);
    }
} // namespace OloEngine
