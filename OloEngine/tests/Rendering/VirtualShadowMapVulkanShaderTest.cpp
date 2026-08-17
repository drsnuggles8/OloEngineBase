// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// VirtualShadowMapVulkanShaderTest.cpp
//
// Compiles every Virtual Shadow Map shader with `OLO_VULKAN` defined, for the
// Vulkan target environment, exactly the way VulkanShader/VulkanComputeShader
// do it.
//
// WHY THIS EXISTS. `OLO_VULKAN` is defined in precisely two places in the whole
// engine — `VulkanShader.cpp` and `VulkanComputeShader.cpp` — so a shader's
// `#ifdef OLO_VULKAN` branch is fed to a compiler ONLY when a live Vulkan
// backend loads that shader. VSM's shaders are loaded by
// `VirtualShadowMap::LoadShaders`, which nothing in the suite drives on Vulkan.
// So until this test existed, the Vulkan half of #702 — the vertex-pull branches
// in VSM_Depth/VSM_DepthSkinned and the one-line gl_FragCoord flip in
// VirtualShadowRasterStage — had never been parsed by anything. A missing
// semicolon in there would have shipped, and the GL suite would have stayed
// green through it, because the GL route never sees those lines at all.
//
// WHAT THIS DOES AND DOES NOT PROVE. It proves the Vulkan branches are valid
// GLSL that produces SPIR-V against the real target environment: syntax,
// declared-but-unused bindings, undeclared identifiers, the vertex-pull stride
// arithmetic. It does NOT prove VSM renders correctly on Vulkan. In particular
// the composition in VirtualShadowRasterStage.glsl —
//
//     fragCoord.y_vulkan = VSM_VIRTUAL_RESOLUTION - fragCoord.y_gl
//
// is a claim about two conventions cancelling, and only a rendered frame can
// settle it. Compiling is a floor, not a ceiling; say "the Vulkan shaders
// compile", never "VSM works on Vulkan", until a Vulkan frame has been read
// back.
//
// THE GENERAL GAP, deliberately not fixed here: every shader in the engine with
// an `OLO_VULKAN` branch has this same exposure, and a sweep over all of them
// would be strictly more valuable than this file. That is engine-wide debt from
// #691 rather than #702's to pay, so it is named and left.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] fs::path ShaderRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        }

        [[nodiscard]] std::string ReadWholeFile(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            return buf.str();
        }

        // Inline `#include` the way the engine's shader loaders do before handing
        // source to shaderc. Resolved relative to the INCLUDING file first and
        // then to the shader root, because files under compute/ spell their
        // includes "../include/Foo.glsl" — joining that to the root alone yields a
        // path that does not exist, and the silent empty string that produces is
        // exactly how a test like this ends up compiling nothing and passing.
        // `src` is passed in rather than read from `path` so a caller can resolve
        // ONE STAGE of a multi-stage file; `path` stays the resolution anchor,
        // because includes are spelled relative to the including FILE.
        [[nodiscard]] std::string ResolveIncludes(const fs::path& path, const std::string& src,
                                                  std::set<std::string>& seen,
                                                  std::vector<std::string>& unresolved)
        {
            static const std::regex kInclude(R"(#include\s+\"([^\"]+)\")");

            std::string out;
            auto it = std::sregex_iterator(src.begin(), src.end(), kInclude);
            const auto end = std::sregex_iterator();
            std::size_t last = 0;
            for (; it != end; ++it)
            {
                const std::smatch& m = *it;
                out.append(src, last, static_cast<std::size_t>(m.position()) - last);

                const std::string spelling = m[1].str();
                const fs::path relative = (path.parent_path() / spelling).lexically_normal();
                const fs::path fromRoot = (ShaderRoot() / spelling).lexically_normal();
                // The header guards make a repeat inclusion harmless, but skipping
                // it keeps the compiled text close to what the engine feeds in.
                const auto inlineOnce = [&](const fs::path& target) -> std::string
                {
                    const std::string key = target.lexically_normal().string();
                    if (!seen.insert(key).second)
                    {
                        return {};
                    }
                    return ResolveIncludes(target, ReadWholeFile(target), seen, unresolved);
                };

                if (fs::exists(relative))
                {
                    out.append(inlineOnce(relative));
                }
                else if (fs::exists(fromRoot))
                {
                    out.append(inlineOnce(fromRoot));
                }
                else
                {
                    unresolved.push_back(spelling + " (from " + path.filename().string() + ")");
                }
                last = static_cast<std::size_t>(m.position()) + static_cast<std::size_t>(m.length());
            }
            out.append(src, last, std::string::npos);
            return out;
        }

        // The engine's graphics shaders are one file with `#type vertex` /
        // `#type fragment` sections; shaderc wants one stage at a time.
        struct Stage
        {
            shaderc_shader_kind Kind;
            std::string Source;
            std::string Name;
        };

        [[nodiscard]] std::vector<Stage> SplitStages(const std::string& src, const std::string& name)
        {
            static const std::regex kType(R"(#type\s+(\w+))");
            std::vector<Stage> stages;

            auto it = std::sregex_iterator(src.begin(), src.end(), kType);
            const auto end = std::sregex_iterator();
            if (it == end)
            {
                stages.push_back({ shaderc_glsl_compute_shader, src, name });
                return stages;
            }

            for (; it != end; ++it)
            {
                const std::smatch& m = *it;
                const std::string kind = m[1].str();
                const auto bodyStart = static_cast<std::size_t>(m.position()) + static_cast<std::size_t>(m.length());

                auto next = it;
                ++next;
                const std::size_t bodyEnd =
                    (next == end) ? src.size() : static_cast<std::size_t>(next->position());

                stages.push_back({ kind == "vertex" ? shaderc_glsl_vertex_shader : shaderc_glsl_fragment_shader,
                                   src.substr(bodyStart, bodyEnd - bodyStart), name + " [" + kind + "]" });
            }
            return stages;
        }

        struct Result
        {
            bool Succeeded = false;
            std::string Message;
        };

        // Mirrors VulkanShader::CompileOrGetVulkanBinaries option for option —
        // including the 1.4 target env spelled numerically, because a toolchain
        // predating the 1.4 SDK has no enumerator for it. A test that compiled
        // with different options would answer a different question.
        [[nodiscard]] Result CompileForVulkan(const Stage& stage)
        {
            shaderc::Compiler compiler;
            shaderc::CompileOptions options;
            constexpr auto kShadercEnvVulkan14 = static_cast<shaderc_env_version>((1u << 22) | (4u << 12));
            options.SetTargetEnvironment(shaderc_target_env_vulkan, kShadercEnvVulkan14);
            options.SetPreserveBindings(true);
            options.SetAutoBindUniforms(false);
            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            options.SetSuppressWarnings();
            options.AddMacroDefinition("OLO_VULKAN", "1");

            const auto module =
                compiler.CompileGlslToSpv(stage.Source, stage.Kind, stage.Name.c_str(), options);
            return Result{ .Succeeded = module.GetCompilationStatus() == shaderc_compilation_status_success,
                           .Message = module.GetErrorMessage() };
        }

        constexpr std::array<std::string_view, 11> kVsmShaders{ {
            "VSM_Depth.glsl",
            "VSM_DepthSkinned.glsl",
            "compute/VSM_AllocatePages.comp",
            "compute/VSM_BuildHPB.comp",
            "compute/VSM_ClearDirtyPages.comp",
            "compute/VSM_CullCasters.comp",
            "compute/VSM_EndFrame.comp",
            "compute/VSM_FindFreePages.comp",
            "compute/VSM_FreeWrappedPages.comp",
            "compute/VSM_InvalidatePages.comp",
            "compute/VSM_MarkRequiredPages.comp",
        } };
    } // namespace

    // THE HARNESS CONTROL, and it is the difference between this file proving
    // something and proving nothing. If `OLO_VULKAN` failed to reach the
    // preprocessor, every shader below would compile its OpenGL branch and the
    // suite would go green while the Vulkan code stayed exactly as unparsed as it
    // was before this file existed — a test that reports success for the state it
    // was written to detect.
    TEST(VirtualShadowMapVulkanShaders, TheHarnessActuallyDefinesOloVulkan)
    {
        const Stage probe{ shaderc_glsl_compute_shader,
                           R"(#version 460 core
layout(local_size_x = 1) in;
#ifndef OLO_VULKAN
#error OLO_VULKAN did not reach the preprocessor
#endif
void main() {}
)",
                           "olo_vulkan_probe.comp" };

        const Result defined = CompileForVulkan(probe);
        EXPECT_TRUE(defined.Succeeded)
            << "OLO_VULKAN is not reaching shaderc, so every compile in this file is exercising the "
               "OpenGL branch: "
            << defined.Message;

        // …and the probe must be capable of failing, or the #error proves nothing
        // either. Same options, macro withheld.
        shaderc::Compiler compiler;
        shaderc::CompileOptions bare;
        constexpr auto kShadercEnvVulkan14 = static_cast<shaderc_env_version>((1u << 22) | (4u << 12));
        bare.SetTargetEnvironment(shaderc_target_env_vulkan, kShadercEnvVulkan14);
        bare.SetSuppressWarnings();
        const auto without = compiler.CompileGlslToSpv(probe.Source, probe.Kind, probe.Name.c_str(), bare);
        EXPECT_NE(without.GetCompilationStatus(), shaderc_compilation_status_success)
            << "the probe compiled WITHOUT OLO_VULKAN, so its #error is inert and it cannot detect a "
               "missing macro";
    }

    TEST(VirtualShadowMapVulkanShaders, EveryVsmShaderCompilesForTheVulkanTarget)
    {
        u32 stagesCompiled = 0;

        for (const std::string_view relative : kVsmShaders)
        {
            const fs::path path = ShaderRoot() / relative;
            ASSERT_TRUE(fs::exists(path)) << "VSM shader missing: " << path.string();

            // SPLIT FIRST, RESOLVE PER STAGE — the order the engine's loaders use,
            // and it is not interchangeable. Each stage is compiled as its own
            // translation unit, so each needs its OWN copy of every header it
            // includes. Resolving over the whole file with one `seen` set gives
            // the header to whichever stage mentions it first and hands the other
            // an empty string: the fragment stage of VSM_DepthSkinned then lost
            // every VSM declaration and reported 14 errors that were entirely the
            // harness's doing. A test that blames the code under test for its own
            // bug is worse than no test.
            const std::string raw = ReadWholeFile(path);
            ASSERT_FALSE(raw.empty()) << relative << " is empty";

            for (Stage& stage : SplitStages(raw, std::string(relative)))
            {
                std::set<std::string> seen;
                std::vector<std::string> unresolved;
                stage.Source = ResolveIncludes(path, stage.Source, seen, unresolved);

                ASSERT_TRUE(unresolved.empty())
                    << stage.Name << " has unresolvable #include(s) — the compile below would silently "
                    << "miss whatever they declare: " << unresolved.front();

                const Result result = CompileForVulkan(stage);
                EXPECT_TRUE(result.Succeeded)
                    << stage.Name << " failed to compile with OLO_VULKAN defined:\n"
                    << result.Message
                    << "\n  This branch is invisible to the OpenGL route, so nothing else in the suite "
                       "would have caught it.";
                ++stagesCompiled;
            }
        }

        // The harness must have done work. Without this, a path typo or a broken
        // splitter makes the loop compile nothing and the test pass vacuously —
        // which is the failure mode this whole file exists to prevent elsewhere.
        EXPECT_GE(stagesCompiled, kVsmShaders.size())
            << "compiled only " << stagesCompiled << " stages from " << kVsmShaders.size()
            << " shaders — the harness is not reaching the sources";
    }

    // The flip is the ONE backend fork in the whole system, so its absence is
    // worth a test of its own: a refactor that dropped the #ifdef would leave
    // every page mirrored vertically on Vulkan while both backends still
    // compiled, and the GL suite would stay green through it.
    TEST(VirtualShadowMapVulkanShaders, TheRasterStageStillCarriesItsSingleBackendFork)
    {
        const std::string src = ReadWholeFile(ShaderRoot() / "include" / "VirtualShadowRasterStage.glsl");
        ASSERT_FALSE(src.empty());

        EXPECT_NE(src.find("#ifdef OLO_VULKAN"), std::string::npos)
            << "VirtualShadowRasterStage.glsl no longer has an OLO_VULKAN branch. gl_FragCoord's origin "
               "differs between the backends and the clip projection carries a y flip on Vulkan; without "
               "the compensation here the physical pool holds vertically mirrored pages on Vulkan only, "
               "and every consumer — the page lookup, the sampler, any golden — reads them silently wrong.";
        EXPECT_NE(src.find("(VSM_VIRTUAL_RESOLUTION - 1) - virtualTexel.y"), std::string::npos)
            << "the y-flip compensation changed shape — if that is deliberate, update this test and say "
               "in the commit what the new composition is";
    }
} // namespace OloEngine::Tests
