// =============================================================================
// ShaderCompileFailureRecoveryTest.cpp
//
// Regression test for issue #568 — a user-authored GLSL compile/link failure
// used to be treated as an engineering invariant violation: `OpenGLShader`
// logged the error and then called `OLO_CORE_VERIFY(false, ...)`, which
// `OLO_DEBUGBREAK()`s in Debug builds and crashes the editor whenever a
// developer edits a shader into a broken state (the normal hot-reload
// iteration loop). This drives the real compile path — `Shader::Create`
// through `OpenGLShader`'s source-string constructor — with deliberately
// broken GLSL and asserts the process survives and the shader lands in
// `ShaderCompilationStatus::Failed` instead.
//
// Classification: shaderpipe (within-shader-compile-pipeline contract, not
// GLSL math correctness (L2) or GPU state (L4)).
// =============================================================================
// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderDebugUtils.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    namespace
    {
        // These tests construct throwaway shaders with pseudo file paths (the
        // source-string constructor uses the shader name as its "path"), so a
        // real on-disk shader cache entry for them would have no corresponding
        // production .glsl and permanently trip
        // AssetContentValidity.ShaderCacheEntriesAllHaveLiveGlslSources. Disable
        // the process-wide cache for the lifetime of the test and restore
        // whatever state it had on entry.
        struct ScopedDisableShaderCache
        {
            bool m_WasDisabled = ShaderDebugUtils::IsShaderCacheDisabled();
            ScopedDisableShaderCache()
            {
                ShaderDebugUtils::SetDisableShaderCache(true);
            }
            ~ScopedDisableShaderCache()
            {
                ShaderDebugUtils::SetDisableShaderCache(m_WasDisabled);
            }
        };

        constexpr const char* kValidVertexSrc = R"(
            #version 450 core
            layout(location = 0) in vec3 a_Position;
            void main()
            {
                gl_Position = vec4(a_Position, 1.0);
            }
        )";

        constexpr const char* kValidFragmentSrc = R"(
            #version 450 core
            layout(location = 0) out vec4 o_Color;
            void main()
            {
                o_Color = vec4(1.0);
            }
        )";

        // Deliberate GLSL syntax error: an undeclared type and a missing
        // semicolon. shaderc must reject this at the SPIR-V compile stage —
        // the same stage that crashed in the issue's repro.
        constexpr const char* kBrokenVertexSrc = R"(
            #version 450 core
            layout(location = 0) in vec3 a_Position;
            void main()
            {
                gl_Position = totally_not_a_type(a_Position, 1.0)
            }
        )";
    } // namespace

    TEST(ShaderCompileFailureRecovery, BrokenVertexSourceDoesNotCrashAndReachesFailedStatus)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedDisableShaderCache noCache;

        // Previously this call would OLO_DEBUGBREAK() and terminate the test
        // process before ASSERT_TRUE below ever ran — the crash itself was
        // the bug. Surviving to this point is already the primary assertion.
        Ref<Shader> shader = Shader::Create("BrokenVertexTestShader", kBrokenVertexSrc, kValidFragmentSrc);

        ASSERT_TRUE(shader != nullptr);
        EXPECT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Failed);
        EXPECT_FALSE(shader->IsReady());
        // A failed compile must not hand out a program id that looks usable.
        EXPECT_EQ(shader->GetRendererID(), 0u);
    }

    TEST(ShaderCompileFailureRecovery, BrokenFragmentSourceDoesNotCrashAndReachesFailedStatus)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedDisableShaderCache noCache;

        // Mirrors the vertex-stage case but breaks the fragment stage instead,
        // exercising the other half of the parallel-compile aggregation.
        constexpr const char* kBrokenFragmentSrc = R"(
            #version 450 core
            layout(location = 0) out vec4 o_Color;
            void main()
            {
                o_Color = still_not_a_type(1.0)
            }
        )";

        Ref<Shader> shader = Shader::Create("BrokenFragmentTestShader", kValidVertexSrc, kBrokenFragmentSrc);

        ASSERT_TRUE(shader != nullptr);
        EXPECT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Failed);
        EXPECT_EQ(shader->GetRendererID(), 0u);
    }

    TEST(ShaderCompileFailureRecovery, ValidSourceStillCompilesSuccessfully)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedDisableShaderCache noCache;

        // Sanity check alongside the failure cases above: the fix must not
        // regress the happy path.
        Ref<Shader> shader = Shader::Create("ValidTestShader", kValidVertexSrc, kValidFragmentSrc);

        ASSERT_TRUE(shader != nullptr);
        EXPECT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
        EXPECT_TRUE(shader->IsReady());
        EXPECT_NE(shader->GetRendererID(), 0u);
    }
} // namespace OloEngine::Tests
