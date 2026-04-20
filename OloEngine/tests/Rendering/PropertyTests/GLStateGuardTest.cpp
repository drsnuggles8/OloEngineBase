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
} // namespace OloEngine::Tests
