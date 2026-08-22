// =============================================================================
// OpenGLStateGuard.cpp
//
// The GL implementation of GLStateGuard's snapshot capture and apply, moved
// here from Renderer/Debug/GLStateGuard.cpp (#691, ADR 0011 §1.6).
// The neutral shell keeps the backend gates, the diff, and the guard's
// policy/finalisation logic; everything that touches glad lives in this TU.
// =============================================================================

#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLStateGuard.h"

#include <glad/gl.h>

#include <algorithm>

namespace OloEngine::Detail
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

        // A snapshotted container-object name is validated before the rebind:
        // the guarded scope may legitimately delete an object that was bound at
        // entry (a render-graph resize destroys framebuffers/VAOs), and
        // rebinding a deleted name is itself a GL_INVALID_OPERATION — the guard
        // would then manufacture the very GL-error pollution it exists to
        // contain (#505). Restoring 0 for a dead name matches what the driver
        // did to the binding when the object was deleted.
        // The parameter type is glad's shared `GLboolean (GLAD_API_PTR*)(GLuint)`
        // pointer signature (all glIs* typedefs alias it) so the calling
        // convention matches on every target, not just x64 where it's moot.
        u32 ValidOrZero(u32 name, PFNGLISFRAMEBUFFERPROC isAlive)
        {
            // Compare explicitly against GL_TRUE: GLboolean is an unsigned
            // char, and using it directly as a logical operand is both a
            // reliability finding (cpp:S867) and a real portability trap —
            // the GL spec only guarantees GL_TRUE/GL_FALSE, not 1/0.
            return (name != 0u && isAlive(name) == GL_TRUE) ? name : 0u;
        }
    } // namespace

    GLStateSnapshot CaptureGLState()
    {
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
        s.m_StencilBackFunc = GlGetInt(GL_STENCIL_BACK_FUNC);
        s.m_StencilBackRef = GlGetInt(GL_STENCIL_BACK_REF);
        s.m_StencilBackValueMask = static_cast<u32>(GlGetInt(GL_STENCIL_BACK_VALUE_MASK));
        s.m_StencilWriteMask = static_cast<u32>(GlGetInt(GL_STENCIL_WRITEMASK));
        s.m_StencilBackWriteMask = static_cast<u32>(GlGetInt(GL_STENCIL_BACK_WRITEMASK));
        s.m_StencilFail = GlGetInt(GL_STENCIL_FAIL);
        s.m_StencilPassDepthFail = GlGetInt(GL_STENCIL_PASS_DEPTH_FAIL);
        s.m_StencilPassDepthPass = GlGetInt(GL_STENCIL_PASS_DEPTH_PASS);
        s.m_StencilBackFail = GlGetInt(GL_STENCIL_BACK_FAIL);
        s.m_StencilBackPassDepthFail = GlGetInt(GL_STENCIL_BACK_PASS_DEPTH_FAIL);
        s.m_StencilBackPassDepthPass = GlGetInt(GL_STENCIL_BACK_PASS_DEPTH_PASS);

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
        const u32 textureSlotLimit = std::min<u32>(GLStateSnapshot::kTextureSlots, driverTextureUnits > 0 ? static_cast<u32>(driverTextureUnits) : GLStateSnapshot::kTextureSlots);
        const u32 uboSlotLimit = std::min<u32>(GLStateSnapshot::kUboSlots, driverUboBindings > 0 ? static_cast<u32>(driverUboBindings) : GLStateSnapshot::kUboSlots);
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

    void ApplyGLStateCore(const GLStateSnapshot& snapshot)
    {
        // FBO bindings FIRST — subsequent state-setting calls are global so
        // their order doesn't matter, but `glNamedFramebuffer*` DSA calls
        // (not used here) would still target the snapshot's FBO regardless.
        ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ValidOrZero(snapshot.m_FboDraw, ::glIsFramebuffer));
        ::glBindFramebuffer(GL_READ_FRAMEBUFFER, ValidOrZero(snapshot.m_FboRead, ::glIsFramebuffer));

        ::glUseProgram(ValidOrZero(snapshot.m_ActiveProgram, ::glIsProgram));
        ::glBindVertexArray(ValidOrZero(snapshot.m_Vao, ::glIsVertexArray));

        // Depth
        if (snapshot.m_DepthTest)
            ::glEnable(GL_DEPTH_TEST);
        else
            ::glDisable(GL_DEPTH_TEST);
        ::glDepthMask(snapshot.m_DepthMask ? GL_TRUE : GL_FALSE);
        ::glDepthFunc(static_cast<GLenum>(snapshot.m_DepthFunc));

        // Blend (global enable + separate RGB/A funcs + equations). Per-
        // attachment enables / funcs (`glEnablei(GL_BLEND, N)`,
        // `glBlendFunci`, `glBlendEquationi`) are NOT captured in the
        // snapshot and therefore not restored here — passes that mutate
        // them must still manage that subset explicitly.
        if (snapshot.m_Blend)
            ::glEnable(GL_BLEND);
        else
            ::glDisable(GL_BLEND);
        ::glBlendFuncSeparate(static_cast<GLenum>(snapshot.m_BlendSrcRgb), static_cast<GLenum>(snapshot.m_BlendDstRgb),
                              static_cast<GLenum>(snapshot.m_BlendSrcAlpha), static_cast<GLenum>(snapshot.m_BlendDstAlpha));
        ::glBlendEquationSeparate(static_cast<GLenum>(snapshot.m_BlendEqRgb), static_cast<GLenum>(snapshot.m_BlendEqAlpha));

        // Stencil
        if (snapshot.m_StencilTest)
            ::glEnable(GL_STENCIL_TEST);
        else
            ::glDisable(GL_STENCIL_TEST);
        ::glStencilFuncSeparate(GL_FRONT, static_cast<GLenum>(snapshot.m_StencilFunc), snapshot.m_StencilRef, snapshot.m_StencilMask);
        ::glStencilFuncSeparate(GL_BACK, static_cast<GLenum>(snapshot.m_StencilBackFunc), snapshot.m_StencilBackRef, snapshot.m_StencilBackValueMask);
        ::glStencilMaskSeparate(GL_FRONT, snapshot.m_StencilWriteMask);
        ::glStencilMaskSeparate(GL_BACK, snapshot.m_StencilBackWriteMask);
        ::glStencilOpSeparate(GL_FRONT, static_cast<GLenum>(snapshot.m_StencilFail),
                              static_cast<GLenum>(snapshot.m_StencilPassDepthFail),
                              static_cast<GLenum>(snapshot.m_StencilPassDepthPass));
        ::glStencilOpSeparate(GL_BACK, static_cast<GLenum>(snapshot.m_StencilBackFail),
                              static_cast<GLenum>(snapshot.m_StencilBackPassDepthFail),
                              static_cast<GLenum>(snapshot.m_StencilBackPassDepthPass));

        // Cull + front-face
        if (snapshot.m_CullFace)
            ::glEnable(GL_CULL_FACE);
        else
            ::glDisable(GL_CULL_FACE);
        ::glCullFace(static_cast<GLenum>(snapshot.m_CullFaceMode));
        ::glFrontFace(static_cast<GLenum>(snapshot.m_FrontFace));

        // Polygon mode — in 4.6 core front + back must match, so only
        // issue the front value (back is captured solely for the diff).
        ::glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(snapshot.m_PolygonMode[0]));

        // Viewport + scissor
        ::glViewport(snapshot.m_Viewport[0], snapshot.m_Viewport[1], snapshot.m_Viewport[2], snapshot.m_Viewport[3]);
        ::glScissor(snapshot.m_Scissor[0], snapshot.m_Scissor[1], snapshot.m_Scissor[2], snapshot.m_Scissor[3]);
        if (snapshot.m_ScissorTest)
            ::glEnable(GL_SCISSOR_TEST);
        else
            ::glDisable(GL_SCISSOR_TEST);

        // Active texture unit — restore the enum a caller selected, so
        // subsequent non-DSA texture mutation in another pass doesn't pick
        // up the wrong unit. Per-unit bindings themselves are not
        // restored (see the class comment for rationale). The valid range
        // matches the snapshot's `kTextureSlots` cap (engine reserves up
        // to TEX_SHADER_GRAPH_0 = 50) so units in GL_TEXTURE32..GL_TEXTURE50
        // are also restored — the earlier GL_TEXTURE31 cap dropped those.
        const u32 kMaxActive = GL_TEXTURE0 + GLStateSnapshot::kTextureSlots - 1;
        if (snapshot.m_ActiveTextureUnit >= GL_TEXTURE0 && snapshot.m_ActiveTextureUnit <= kMaxActive)
            ::glActiveTexture(snapshot.m_ActiveTextureUnit);
    }
} // namespace OloEngine::Detail
