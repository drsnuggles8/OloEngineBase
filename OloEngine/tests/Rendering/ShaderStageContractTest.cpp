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

#include <algorithm>
#include <set>
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

        // The working directory at PROCESS START — some other test in this
        // binary chdir()s, so a relative path resolved inside a test body can
        // miss the file (see RendererShutdownTest for the same note).
        const fs::path s_StartCwd = fs::current_path();

        /// Walk up from the start cwd until `relative` resolves, so the test
        /// works from the repo root or from inside a build directory.
        [[nodiscard]] fs::path ResolveRepoFile(const char* relative)
        {
            std::error_code ec;
            for (fs::path dir = s_StartCwd; !dir.empty(); dir = dir.parent_path())
            {
                if (fs::path candidate = dir / relative; fs::exists(candidate, ec))
                    return candidate;
                if (!dir.has_relative_path())
                    break;
            }
            return s_StartCwd / relative; // report the path we looked for
        }

        [[nodiscard]] std::string StripWhitespace(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                    out += c;
            }
            return out;
        }
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

    // -------------------------------------------------------------------------
    // DecalGBufferOutputLocationsMatchTheirDrawAttachmentMaps (issue #770)
    //
    // DecalRenderPass::ExecuteOnGBuffer switches a per-mode draw-attachment map
    // before dispatching each decal packet, and that map routes fragment output
    // LOCATION n to the G-Buffer attachment named at slot n. So the rule across
    // all four G-Buffer decal shaders is: **a fragment output's location must
    // equal the G-Buffer RT index the mode writes**.
    //
    // Decal_GBuffer_Normal.glsl broke it — it declared `layout(location = 0)`
    // against the map {NoAttachment, 1, NoAttachment, NoAttachment,
    // NoAttachment}, so location 0 landed on GL_NONE / VK_ATTACHMENT_UNUSED and
    // attachment 1 was fed from a location the shader never wrote. The decal
    // drew, passed depth, and wrote nothing — on BOTH backends, with no error
    // (GL discards the write silently; Vulkan's validation layer only warns).
    //
    // Both mirrors are checked here so a one-sided edit can't pass: the shader
    // locations come from SPIR-V reflection, and the C++ maps are matched as
    // whitespace-stripped source text (clang-format-proof; a rename fails
    // loudly, which is the intent).
    // -------------------------------------------------------------------------
    TEST(ShaderStageContract, DecalGBufferOutputLocationsMatchTheirDrawAttachmentMaps)
    {
        struct DecalMode
        {
            const char* Shader;
            std::set<u32> ExpectedLocations;
            // The map literal in DecalRenderPass.cpp, whitespace-stripped.
            const char* MapSource;
            const char* Why;
        };

        // kNone is RHI::NoAttachment — "this draw slot writes nowhere".
        const DecalMode kModes[] = {
            { "Decal_GBuffer.glsl", { 0u }, "drawAlbedoOnly={0,kNone,kNone,kNone,kNone}", "Albedo writes RT0 only" },
            { "Decal_GBuffer_Normal.glsl", { 1u }, "drawNormalOnly={kNone,1,kNone,kNone,kNone}", "Normal writes RT1 only" },
            { "Decal_GBuffer_RMA.glsl", { 0u, 1u }, "drawAlbedoAndNormal={0,1,kNone,kNone,kNone}", "RMA writes RT0.a and RT1.zw" },
            { "Decal_GBuffer_Emissive.glsl", { 2u }, "drawEmissiveOnly={kNone,kNone,2,kNone,kNone}", "Emissive writes RT2 only" },
        };

        const fs::path passSource = ResolveRepoFile("OloEngine/src/OloEngine/Renderer/Passes/DecalRenderPass.cpp");
        ASSERT_TRUE(fs::exists(passSource)) << "could not locate DecalRenderPass.cpp from cwd " << s_StartCwd.string();
        const std::string passText = StripWhitespace(SH::ReadWholeFile(passSource));
        ASSERT_FALSE(passText.empty());

        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        for (const auto& mode : kModes)
        {
            SCOPED_TRACE(mode.Shader);

            EXPECT_NE(passText.find(mode.MapSource), std::string::npos)
                << "DecalRenderPass.cpp no longer contains the draw-attachment map '" << mode.MapSource
                << "' this contract is written against (" << mode.Why
                << "). If the map genuinely changed, update BOTH it and the shader's output location.";

            const fs::path path = root / mode.Shader;
            ASSERT_TRUE(fs::exists(path)) << path.generic_string();

            std::set<u32> locations;
            bool sawFragmentStage = false;
            for (const auto& [kind, stageSource] : SH::SplitByType(SH::ReadWholeFile(path)))
            {
                if (kind != shaderc_glsl_fragment_shader)
                    continue;
                sawFragmentStage = true;

                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler);
                ASSERT_EQ(result.GetCompilationStatus(), shaderc_compilation_status_success)
                    << result.GetErrorMessage();

                spirv_cross::Compiler refl(std::vector<u32>(result.cbegin(), result.cend()));
                for (const auto& out : refl.get_shader_resources().stage_outputs)
                    locations.insert(refl.get_decoration(out.id, spv::DecorationLocation));
            }
            EXPECT_TRUE(sawFragmentStage);

            EXPECT_EQ(locations, mode.ExpectedLocations)
                << mode.Why << " — every fragment output location must equal the G-Buffer RT index the "
                << "mode's draw-attachment map assigns it, or the write lands on no attachment (#770).";
        }
    }
} // namespace OloEngine::Tests
