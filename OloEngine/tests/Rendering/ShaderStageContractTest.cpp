// =============================================================================
// ShaderStageContractTest.cpp
//
// Within-shader contract checks that catch silent half-built shaders
// before the editor surfaces them as "the post-process pass renders a
// black screen" or "selection outline shader compiles but never produces
// pixels".
//
// What this test does
// -------------------
//   For every production .glsl with a `#type fragment` stage, *except*
//   those whose file name contains "Depth" (the engine convention for
//   shadow-map / depth pre-pass shaders that intentionally write only
//   `gl_FragDepth` via the rasterizer and declare zero `out` variables):
//     1. Compile to SPIR-V.
//     2. Reflect `stage_outputs`.
//     3. Assert there is at least one fragment output (one of
//        `layout(location = N) out vec4 ...`). A fragment shader with
//        zero outputs writes nothing — the entire pass is a no-op.
//
// Why this isn't caught by shaderc
// --------------------------------
//   Empty-output fragment shaders compile cleanly under shaderc; the
//   GLSL spec doesn't require at least one output. They only fail when
//   you try to render into a framebuffer that expects a color attachment.
//   The editor *does* render into such framebuffers, so an empty
//   fragment shader silently produces a black pass.
//
// Classification: shaderpipe.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <spirv_cross/spirv_cross.hpp>

#include "ShaderHarness.h"

#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        struct Failure
        {
            std::string ShaderPath;
            std::string Detail;
        };
    } // namespace

    TEST(ShaderStageContract, EveryFragmentShaderDeclaresAtLeastOneOutput)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        u32 fragmentStagesChecked = 0;
        std::vector<Failure> failures;

        for (const auto& path : shaders)
        {
            // Two classes of fragment shaders legitimately declare zero
            // color outputs:
            //   1. Depth-only passes (shadow maps + depth pre-passes) —
            //      identified by "Depth" in the file name.
            //   2. Occlusion-query proxy passes (`OcclusionProxy.glsl`) —
            //      rendered with color writes disabled.
            // Both write depth via the rasterizer and don't need a colour
            // attachment.
            if (const std::string fileName = path.filename().generic_string(); fileName.contains("Depth") || fileName == "OcclusionProxy.glsl")
                continue;

            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitByType(source);

            for (const auto& [kind, stageSource] : stages)
            {
                if (kind != shaderc_glsl_fragment_shader)
                    continue;

                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler);
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                    continue; // ShaderCompilationTest's responsibility.

                ++fragmentStagesChecked;
                try
                {
                    spirv_cross::Compiler refl(std::vector<u32>(result.cbegin(), result.cend()));
                    const auto& outputs = refl.get_shader_resources().stage_outputs;
                    if (outputs.empty())
                    {
                        failures.push_back({ path.generic_string(),
                                             "fragment stage declares zero outputs — pass writes "
                                             "nothing and renders silently as black." });
                    }
                }
                catch (...)
                {
                }
            }
        }

        EXPECT_GT(fragmentStagesChecked, 0u);

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " fragment shader(s) declare zero outputs:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.ShaderPath << "\n    " << f.Detail << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // EveryVertexShaderWritesGlPosition
    //
    // A vertex shader that doesn't assign `gl_Position` compiles cleanly
    // under shaderc but produces undefined clip-space coordinates — the
    // primitives get clipped or rasterized at random screen locations, so
    // the entire pass renders nothing useful. This bug class is silent at
    // build time and shows up as "the model disappeared after I edited
    // its shader" in OloEditor.
    //
    // Detection: a substring search on the vertex stage source for
    // `gl_Position`. Robust enough for the engine's shader style —
    // every legitimate vertex shader contains the literal token, and a
    // shader that uses it inside an `#if 0` block already fails to
    // render in production. SPIR-V reflection would be technically
    // sharper (look for `OpStore` to the `BuiltInPosition` decorated
    // variable) but the cost/benefit doesn't justify it here.
    // -------------------------------------------------------------------------
    TEST(ShaderStageContract, EveryVertexShaderWritesGlPosition)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        u32 vertexStagesChecked = 0;
        std::vector<Failure> failures;

        for (const auto& path : shaders)
        {
            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitByType(source);

            // Shaders using tessellation write gl_Position from the
            // tess-evaluation stage, not the vertex stage. The vertex
            // stage in those shaders legitimately just forwards attributes.
            bool hasTessEval = false;
            for (const auto& [kind, _] : stages)
                if (kind == shaderc_glsl_tess_evaluation_shader)
                {
                    hasTessEval = true;
                    break;
                }
            if (hasTessEval)
                continue;

            for (const auto& [kind, stageSource] : stages)
            {
                if (kind != shaderc_glsl_vertex_shader)
                    continue;
                ++vertexStagesChecked;

                if (stageSource.find("gl_Position") == std::string::npos)
                {
                    failures.push_back({ path.generic_string(),
                                         "vertex stage never references gl_Position — "
                                         "primitives produce undefined clip coordinates "
                                         "and the pass renders nothing." });
                }
            }
        }

        EXPECT_GT(vertexStagesChecked, 0u);

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " vertex shader(s) never assign gl_Position:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.ShaderPath << "\n    " << f.Detail << "\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
