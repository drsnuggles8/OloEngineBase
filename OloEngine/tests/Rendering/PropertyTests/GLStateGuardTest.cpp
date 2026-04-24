// =============================================================================
// GLStateGuardTest.cpp
//
// Tests for the Layer-2 GPU state validation primitive (GLStateGuard). These
// verify the guard itself: capture / diff / leak detection on a known
// sequence of state mutations. Actual deployment at render-pass boundaries is
// a follow-up change once the guard is proven reliable.
//
// Test contract:
//   1. No-op region: guard should detect zero leaks.
//   2. Each mutable field (blend, depth test/mask, viewport, FBO binding,
//      program, texture/UBO bindings) leaks individually ⇒ detected.
//   3. Multiple leaks produce multiple diff entries.
//   4. A region that *restores* state before the guard destructor runs
//      should detect zero leaks (guard must compare to the entry snapshot,
//      not to some assumed default).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Core/Log.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    // Helper: does the diff list contain an entry whose field name starts
    // with the given prefix? Used because field formatting includes old/new
    // values that depend on the GL default (e.g., "DepthFunc: 519 -> 514").
    bool DiffContainsPrefix(const std::vector<std::string>& diffs, std::string_view prefix)
    {
        return std::any_of(diffs.begin(), diffs.end(),
                           [&](const std::string& d)
                           { return d.rfind(prefix, 0) == 0; });
    }

    // =========================================================================
    // No-op: a guard over an empty region must report zero leaks.
    // =========================================================================
    TEST(GLStateGuardTest, EmptyRegionHasNoLeaks)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GLStateGuard guard("EmptyRegion", GLStateGuard::Policy::Ignore);
        const auto diffs = guard.DetectLeaks();
        EXPECT_TRUE(diffs.empty()) << "unexpected leaks: " << diffs.size();
    }

    // =========================================================================
    // Blend-enable is the classic OloEngine leak pattern (ResetState bugs,
    // InfiniteGrid leaking into terrain). A pass that flips blend must show
    // up in DetectLeaks().
    // =========================================================================
    TEST(GLStateGuardTest, LeakedBlendIsDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ::glDisable(GL_BLEND);

        GLStateGuard guard("BlendLeakRegion", GLStateGuard::Policy::Ignore);
        ::glEnable(GL_BLEND);
        const auto diffs = guard.DetectLeaks();

        EXPECT_TRUE(DiffContainsPrefix(diffs, "Blend:"))
            << "expected a 'Blend:' entry in diffs, got " << diffs.size() << " entries";

        ::glDisable(GL_BLEND); // cleanup for subsequent tests
    }

    // =========================================================================
    // Depth-mask disable is the bug that silently no-ops depth clears. A pass
    // that flips GL_DEPTH_WRITEMASK off without restoring must be caught.
    // =========================================================================
    TEST(GLStateGuardTest, LeakedDepthMaskIsDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ::glDepthMask(GL_TRUE);

        GLStateGuard guard("DepthMaskLeakRegion", GLStateGuard::Policy::Ignore);
        ::glDepthMask(GL_FALSE);
        const auto diffs = guard.DetectLeaks();

        EXPECT_TRUE(DiffContainsPrefix(diffs, "DepthMask"))
            << "expected 'DepthMask' entry; got " << diffs.size();

        ::glDepthMask(GL_TRUE);
    }

    // =========================================================================
    // FBO leak: a pass that binds its output FBO and doesn't unbind is the
    // canonical "next pass renders to the wrong target" bug.
    // =========================================================================
    TEST(GLStateGuardTest, LeakedDrawFboIsDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FramebufferSpecification spec{};
        spec.Width = 32;
        spec.Height = 32;
        spec.Attachments = { FramebufferTextureFormat::RGBA8 };
        auto fb = Framebuffer::Create(spec);

        ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        GLStateGuard guard("FboLeakRegion", GLStateGuard::Policy::Ignore);
        fb->Bind();
        const auto diffs = guard.DetectLeaks();

        EXPECT_TRUE(DiffContainsPrefix(diffs, "DrawFBO"))
            << "expected 'DrawFBO' entry; got " << diffs.size();

        ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }

    // =========================================================================
    // Texture-binding leak: a pass leaves a texture bound to slot 5 and the
    // next pass (which assumes slot 5 is its own texture) reads garbage.
    // =========================================================================
    TEST(GLStateGuardTest, LeakedTextureBindingIsDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const u32 tex = CreateUniformFloatTexture2D(8, 8, 0.5f, 0.5f, 0.5f, 1.0f);

        ::glBindTextureUnit(5, 0);

        GLStateGuard guard("TextureLeakRegion", GLStateGuard::Policy::Ignore);
        ::glBindTextureUnit(5, static_cast<GLuint>(tex));
        const auto diffs = guard.DetectLeaks();

        EXPECT_TRUE(DiffContainsPrefix(diffs, "Texture2D[5]"))
            << "expected 'Texture2D[5]' entry; got " << diffs.size();

        ::glBindTextureUnit(5, 0);
        ::glDeleteTextures(1, reinterpret_cast<const GLuint*>(&tex));
    }

    // =========================================================================
    // UBO-binding leak: the fog test earlier in this suite illustrates how a
    // single forgotten UBO binding at slot 17 can silently break later passes.
    // Verify the guard sees it.
    // =========================================================================
    TEST(GLStateGuardTest, LeakedUboBindingIsDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto ubo = UniformBuffer::Create(64, 11);
        ::glBindBufferBase(GL_UNIFORM_BUFFER, 11, 0);

        GLStateGuard guard("UboLeakRegion", GLStateGuard::Policy::Ignore);
        // Re-bind by constructing a scoped SetData+Bind effect: we just
        // bind the buffer directly to slot 11 to simulate a pass that
        // updates a UBO and forgets to rebind slot 11 to 0 afterward.
        const GLuint uboHandle = static_cast<GLuint>(ubo->GetRendererID());
        ::glBindBufferBase(GL_UNIFORM_BUFFER, 11, uboHandle);
        const auto diffs = guard.DetectLeaks();

        EXPECT_TRUE(DiffContainsPrefix(diffs, "UBO[11]"))
            << "expected 'UBO[11]' entry; got " << diffs.size();

        ::glBindBufferBase(GL_UNIFORM_BUFFER, 11, 0);
    }

    // =========================================================================
    // Multiple simultaneous leaks produce multiple diff entries (sanity check
    // that the guard doesn't short-circuit on the first mismatch).
    // =========================================================================
    TEST(GLStateGuardTest, MultipleLeaksAreAllReported)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ::glDisable(GL_BLEND);
        ::glDepthMask(GL_TRUE);
        ::glDisable(GL_STENCIL_TEST);

        GLStateGuard guard("MultiLeakRegion", GLStateGuard::Policy::Ignore);
        ::glEnable(GL_BLEND);
        ::glDepthMask(GL_FALSE);
        ::glEnable(GL_STENCIL_TEST);

        const auto diffs = guard.DetectLeaks();
        EXPECT_GE(diffs.size(), 3u) << "expected at least 3 diff entries";
        EXPECT_TRUE(DiffContainsPrefix(diffs, "Blend:"));
        EXPECT_TRUE(DiffContainsPrefix(diffs, "DepthMask"));
        EXPECT_TRUE(DiffContainsPrefix(diffs, "StencilTest"));

        // cleanup
        ::glDisable(GL_BLEND);
        ::glDepthMask(GL_TRUE);
        ::glDisable(GL_STENCIL_TEST);
    }

    // =========================================================================
    // A region that *restores* state should produce zero leaks. Without this
    // property the guard would produce false positives on well-behaved passes.
    // =========================================================================
    TEST(GLStateGuardTest, RestoredStateShowsNoLeaks)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ::glDisable(GL_BLEND);

        GLStateGuard guard("RestoreRegion", GLStateGuard::Policy::Ignore);
        ::glEnable(GL_BLEND);
        ::glDisable(GL_BLEND); // restore
        const auto diffs = guard.DetectLeaks();

        EXPECT_FALSE(DiffContainsPrefix(diffs, "Blend:"))
            << "guard falsely reported blend leak after restore";
    }
    // =========================================================================
    // Policy::Restore — core-state restoration on destructor.
    //
    // A pass that mutates a variety of *core* GL state (depth, blend, stencil,
    // cull, polygon-mode, scissor, viewport, FBO, active program, VAO) without
    // cleaning up should have that state rolled back by the guard destructor
    // when constructed with `Policy::Restore`. Verifies `ApplyCore()` actually
    // runs and covers every field it's documented to touch.
    // =========================================================================
    TEST(GLStateGuardTest, RestorePolicy_RestoresCoreStateOnDtor)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // ---- Arrange: set a known entry state for every ApplyCore() field.
        ::glDisable(GL_DEPTH_TEST);
        ::glDepthMask(GL_TRUE);
        ::glDepthFunc(GL_LESS);
        ::glDisable(GL_BLEND);
        ::glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        ::glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        ::glDisable(GL_STENCIL_TEST);
        ::glStencilFunc(GL_ALWAYS, 0, 0xFFu);
        ::glDisable(GL_CULL_FACE);
        ::glCullFace(GL_BACK);
        ::glFrontFace(GL_CCW);
        ::glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        ::glDisable(GL_SCISSOR_TEST);
        ::glScissor(0, 0, 16, 16);
        ::glViewport(0, 0, 32, 32);
        ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        ::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        ::glUseProgram(0);
        ::glBindVertexArray(0);

        // A dummy FBO to swap the draw binding to inside the scope.
        FramebufferSpecification spec{};
        spec.Width = 32;
        spec.Height = 32;
        spec.Attachments = { FramebufferTextureFormat::RGBA8 };
        auto fb = Framebuffer::Create(spec);

        {
            GLStateGuard guard("RestoreCoreRegion", GLStateGuard::Policy::Restore);

            // ---- Act: mutate the same set the snapshot covers.
            ::glEnable(GL_DEPTH_TEST);
            ::glDepthMask(GL_FALSE);
            ::glDepthFunc(GL_GREATER);
            ::glEnable(GL_BLEND);
            ::glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            ::glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_SUBTRACT);
            ::glEnable(GL_STENCIL_TEST);
            ::glStencilFunc(GL_NEVER, 1, 0x0Fu);
            ::glEnable(GL_CULL_FACE);
            ::glCullFace(GL_FRONT);
            ::glFrontFace(GL_CW);
            ::glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            ::glEnable(GL_SCISSOR_TEST);
            ::glScissor(1, 2, 3, 4);
            ::glViewport(7, 8, 9, 10);
            fb->Bind();
        } // guard dtor → ApplyCore()

        // ---- Assert: every restored core field matches the entry snapshot.
        GLboolean depthTest = GL_FALSE;
        ::glGetBooleanv(GL_DEPTH_TEST, &depthTest);
        EXPECT_EQ(depthTest, GL_FALSE) << "DepthTest not restored";

        GLboolean depthMask = GL_FALSE;
        ::glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        EXPECT_EQ(depthMask, GL_TRUE) << "DepthMask not restored";

        GLint depthFunc = 0;
        ::glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        EXPECT_EQ(depthFunc, GL_LESS) << "DepthFunc not restored";

        GLboolean blend = GL_TRUE;
        ::glGetBooleanv(GL_BLEND, &blend);
        EXPECT_EQ(blend, GL_FALSE) << "Blend not restored";

        GLboolean stencil = GL_TRUE;
        ::glGetBooleanv(GL_STENCIL_TEST, &stencil);
        EXPECT_EQ(stencil, GL_FALSE) << "StencilTest not restored";

        GLboolean cull = GL_TRUE;
        ::glGetBooleanv(GL_CULL_FACE, &cull);
        EXPECT_EQ(cull, GL_FALSE) << "CullFace not restored";

        GLint cullMode = 0;
        ::glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
        EXPECT_EQ(cullMode, GL_BACK) << "CullFaceMode not restored";

        GLint frontFace = 0;
        ::glGetIntegerv(GL_FRONT_FACE, &frontFace);
        EXPECT_EQ(frontFace, GL_CCW) << "FrontFace not restored";

        GLint polyMode[2] = { 0, 0 };
        ::glGetIntegerv(GL_POLYGON_MODE, polyMode);
        EXPECT_EQ(polyMode[0], GL_FILL) << "PolygonMode not restored";

        GLboolean scissor = GL_TRUE;
        ::glGetBooleanv(GL_SCISSOR_TEST, &scissor);
        EXPECT_EQ(scissor, GL_FALSE) << "ScissorTest not restored";

        GLint scissorBox[4] = { 0, 0, 0, 0 };
        ::glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        EXPECT_EQ(scissorBox[0], 0);
        EXPECT_EQ(scissorBox[1], 0);
        EXPECT_EQ(scissorBox[2], 16);
        EXPECT_EQ(scissorBox[3], 16);

        GLint viewport[4] = { 0, 0, 0, 0 };
        ::glGetIntegerv(GL_VIEWPORT, viewport);
        EXPECT_EQ(viewport[0], 0);
        EXPECT_EQ(viewport[1], 0);
        EXPECT_EQ(viewport[2], 32);
        EXPECT_EQ(viewport[3], 32);

        GLint drawFbo = 1;
        ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        EXPECT_EQ(drawFbo, 0) << "DrawFBO not restored";

        GLint readFbo = 1;
        ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        EXPECT_EQ(readFbo, 0) << "ReadFBO not restored";

        GLint program = 1;
        ::glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        EXPECT_EQ(program, 0) << "ActiveProgram not restored";

        GLint vao = 1;
        ::glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        EXPECT_EQ(vao, 0) << "VAO not restored";
    }

    // =========================================================================
    // Policy::Restore — per-slot textures and UBOs are intentionally NOT
    // restored (see GLStateGuard.h for rationale: restore cost would be 185
    // calls per pass). The leak is still *logged*, though, so a regression
    // in a pass is still surfaced via the warn-level diagnostic. This test
    // asserts both: (a) the bindings remain leaked after dtor, and (b) the
    // ringbuffer sink captured a warning naming the leaked slot.
    // =========================================================================
    TEST(GLStateGuardTest, RestorePolicy_DoesNotUnbindTexturesOrUbosButLogsLeak)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const u32 tex = CreateUniformFloatTexture2D(8, 8, 0.1f, 0.2f, 0.3f, 1.0f);
        auto ubo = UniformBuffer::Create(64, 13);
        const GLuint uboHandle = static_cast<GLuint>(ubo->GetRendererID());

        // Clean entry state: slot 6 texture unbound, UBO slot 13 unbound.
        ::glBindTextureUnit(6, 0);
        ::glBindBufferBase(GL_UNIFORM_BUFFER, 13, 0);

        {
            GLStateGuard guard("RestoreLeakRegion", GLStateGuard::Policy::Restore);
            ::glBindTextureUnit(6, static_cast<GLuint>(tex));
            ::glBindBufferBase(GL_UNIFORM_BUFFER, 13, uboHandle);
            // Intentionally leak: rely on guard dtor for core restore only.
        } // guard dtor — ApplyCore() runs, per-slot bindings untouched.

        // ---- Assert: per-slot bindings remain leaked (NOT restored).
        GLint boundTex = 0;
        ::glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 6, &boundTex);
        EXPECT_EQ(static_cast<GLuint>(boundTex), static_cast<GLuint>(tex))
            << "Policy::Restore must NOT unbind per-slot textures";

        GLint boundUbo = 0;
        ::glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 13, &boundUbo);
        EXPECT_EQ(static_cast<GLuint>(boundUbo), uboHandle)
            << "Policy::Restore must NOT unbind per-slot UBOs";

        // ---- Assert: a leak-diagnostic line was emitted. The ringbuffer
        //      holds the last N formatted messages from the core logger; the
        //      destructor logs a "GLStateGuard[RestoreLeakRegion]" warning
        //      plus one indented line per diff entry (Texture2D[6] and
        //      UBO[13]). Scan the tail of the ringbuffer for those markers.
        //      Can't use a baseline size here — the ringbuffer evicts older
        //      messages as new ones arrive, so size is clamped to capacity.
        const auto messages = Log::Get().GetRecentLogMessages();

        const auto containsSubstr = [&messages](std::string_view needle)
        {
            for (const auto& line : messages)
            {
                if (line.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        };

        EXPECT_TRUE(containsSubstr("GLStateGuard[RestoreLeakRegion]"))
            << "expected leak-header log line from Policy::Restore dtor";
        EXPECT_TRUE(containsSubstr("Texture2D[6]") || containsSubstr("UBO[13]"))
            << "expected per-slot leak diagnostic in log";

        // Cleanup.
        ::glBindTextureUnit(6, 0);
        ::glBindBufferBase(GL_UNIFORM_BUFFER, 13, 0);
        ::glDeleteTextures(1, reinterpret_cast<const GLuint*>(&tex));
    }
} // namespace OloEngine::Tests
