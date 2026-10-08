// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "Rendering/ShaderHarness.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderSourceScan.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/OpenGL/OpenGLDescriptorHeap.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <gtest/gtest.h>

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// =============================================================================
// BindlessRawRouteTest -- what the raw-GLSL bindless route (OLO_RHI_BINDLESS=1,
// OpenGLShader::CreateProgramFromRawGLSL) needs from the driver's own GLSL
// front end, which no SPIR-V check reaches (#1565, #1567).
//
// The slotted route hands the driver SPIR-V or SPIRV-Cross text, so shaderc and
// SPIRV-Cross stand between the source and the driver. The raw route hands the
// driver the include-resolved source itself. Two things the slotted route hid:
//
//   * NVIDIA's front end rejects some identifiers glslang accepts. Measured on
//     an RTX 4090 (driver 617.14) with a one-function probe, `#version 460 core`:
//     `packed`, `row_major`, `register` and `char` as a parameter or local name
//     fail to compile (`C1012`, then `C1503` for every use); `std140`, `std430`,
//     `column_major`, `binding`, `offset` and every other layout-qualifier name
//     compile. Every word that BOTH compilers reject is already caught by the
//     slotted route. `oloGroomUnpackTint(float packed)` took GroomStrand and
//     VSM_GroomDepth off the route (#1567). The guard covers every shader file,
//     because which programs reach the driver as text is a per-file opt-in and
//     an include travels: ReservoirCore's `packed` parameters reached GL only
//     as SPIR-V, until something includes them from a raw-route program.
//   * The driver counts every ACTIVE uniform block against
//     GL_MAX_<stage>_UNIFORM_BLOCKS. The slotted route prunes inactive ones in
//     SPIRV-Cross; the raw route adds OloHeapOffsetBlock. NVIDIA reports 14 (the
//     GL 4.6 minimum), Mesa radeonsi 15; Terrain_PBR's fragment stage declared
//     15 and failed to link with `C5058: no buffers available` (#1565).
//
// GlslDriverPortability.* and the uniform-block budget are CPU-only and run in
// CI. EveryOptedInProgramIsBuiltOnTheRawRouteByThisDriver builds every opted-in
// program on the real driver and skips without GL_ARB_bindless_texture.
// =============================================================================

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        // Identifiers the NVIDIA GLSL front end reserves and glslang does not;
        // see the header comment for how the list was measured.
        constexpr std::array<std::string_view, 4> kNvidiaReservedIdentifiers{ "packed", "row_major", "register",
                                                                              "char" };

        // GL 4.6's minimum for GL_MAX_{VERTEX,TESS_*,GEOMETRY,FRAGMENT}_UNIFORM_BLOCKS,
        // and what NVIDIA reports, so the portable ceiling for one stage.
        constexpr u32 kGlMinimumUniformBlocksPerStage = 14u;

        // Replaces every comment character with a space, keeping newlines, so
        // line numbers survive and prose naming a word is not a use of it.
        [[nodiscard]] std::string BlankComments(std::string text)
        {
            for (sizet i = 0; i + 1 < text.size(); ++i)
            {
                if (text[i] == '/' && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                    {
                        text[i++] = ' ';
                    }
                }
                else if (text[i] == '/' && text[i + 1] == '*')
                {
                    while (i < text.size() && !(text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/'))
                    {
                        if (text[i] != '\n')
                        {
                            text[i] = ' ';
                        }
                        ++i;
                    }
                    if (i + 1 < text.size())
                    {
                        text[i] = ' ';
                        text[i + 1] = ' ';
                    }
                }
            }
            return text;
        }

        // Blanks the parenthesised argument of every `layout(...)`: inside it,
        // `packed` and `row_major` are qualifiers, not identifiers.
        [[nodiscard]] std::string BlankLayoutQualifiers(std::string text)
        {
            static constexpr std::string_view kLayout = "layout";
            for (sizet pos = text.find(kLayout); pos != std::string::npos; pos = text.find(kLayout, pos + 1))
            {
                if (pos > 0 && ShaderSourceScan::IsIdentifierChar(text[pos - 1]))
                {
                    continue;
                }
                sizet open = pos + kLayout.size();
                while (open < text.size() && ShaderSourceScan::IsAsciiSpace(text[open]))
                {
                    ++open;
                }
                if (open >= text.size() || text[open] != '(')
                {
                    continue;
                }
                for (sizet i = open + 1; i < text.size() && text[i] != ')'; ++i)
                {
                    if (text[i] != '\n')
                    {
                        text[i] = ' ';
                    }
                }
            }
            return text;
        }

        struct ReservedUse
        {
            u32 Line = 0;
            std::string_view Word;
        };

        // Every whole-identifier use of a word in kNvidiaReservedIdentifiers,
        // outside comments and layout qualifiers.
        [[nodiscard]] std::vector<ReservedUse> FindReservedIdentifierUses(const std::string& source)
        {
            const std::string code = BlankLayoutQualifiers(BlankComments(source));
            std::vector<ReservedUse> uses;
            for (const std::string_view word : kNvidiaReservedIdentifiers)
            {
                for (sizet pos = code.find(word); pos != std::string::npos; pos = code.find(word, pos + 1))
                {
                    const sizet after = pos + word.size();
                    const bool leftOk = pos == 0 || !ShaderSourceScan::IsIdentifierChar(code[pos - 1]);
                    const bool rightOk = after >= code.size() || !ShaderSourceScan::IsIdentifierChar(code[after]);
                    if (leftOk && rightOk)
                    {
                        const auto line = static_cast<u32>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) + 1u;
                        uses.push_back({ line, word });
                    }
                }
            }
            return uses;
        }

        // Mirrors OpenGLShader::WantsBindlessVariant's opt-in (minus the heap
        // toggle), applied to the include closure as the engine applies it to
        // the include-resolved source.
        [[nodiscard]] bool OptsInToTheRawRoute(const fs::path& root, const fs::path& shader)
        {
            std::vector<fs::path> files = SH::IncludeClosure(root, shader);
            files.push_back(shader);
            return std::ranges::any_of(files,
                                       [](const fs::path& file)
                                       {
                                           const std::string text = SH::ReadWholeFile(file);
                                           return ShaderSourceScan::MentionsOutsideComments(text, "OLO_BINDLESS") ||
                                                  ShaderSourceScan::MentionsOutsideComments(text, "OLO_BINDLESS_ROUTE_PARITY");
                                       });
        }

        // The top-level graphics shaders (the files Shader::Create loads).
        [[nodiscard]] std::vector<fs::path> GraphicsShaders(const fs::path& root)
        {
            std::vector<fs::path> out;
            for (const fs::directory_entry& entry : fs::directory_iterator(root))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".glsl" &&
                    SH::ReadWholeFile(entry.path()).find("#type") != std::string::npos)
                {
                    out.push_back(entry.path());
                }
            }
            std::ranges::sort(out);
            return out;
        }

        [[nodiscard]] const char* StageName(shaderc_shader_kind kind)
        {
            switch (kind)
            {
                case shaderc_glsl_vertex_shader:
                    return "vertex";
                case shaderc_glsl_tess_control_shader:
                    return "tess_control";
                case shaderc_glsl_tess_evaluation_shader:
                    return "tess_evaluation";
                case shaderc_glsl_geometry_shader:
                    return "geometry";
                case shaderc_glsl_fragment_shader:
                    return "fragment";
                default:
                    return "other";
            }
        }

        struct RawRouteStage
        {
            shaderc_shader_kind Kind = shaderc_glsl_vertex_shader;
            std::string Text; // preprocessed, comments blanked
        };

        // Every stage of a program as the raw route hands it to the driver:
        // includes resolved and OLO_BINDLESS defined, as the route's prologue
        // does, OLO_VULKAN not. Empty with `error` set if a stage does not
        // preprocess.
        [[nodiscard]] std::vector<RawRouteStage> PreprocessForRawRoute(const fs::path& root, const fs::path& shader,
                                                                       shaderc::Compiler& compiler, std::string& error)
        {
            std::vector<RawRouteStage> stages;
            for (const auto& [kind, stageSource] : SH::SplitStages(SH::ReadWholeFile(shader)))
            {
                shaderc::CompileOptions options;
                options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
                options.SetIncluder(std::make_unique<SH::Includer>(root));
                options.AddMacroDefinition("OLO_BINDLESS", "1");
                const std::string name = shader.generic_string();
                const shaderc::PreprocessedSourceCompilationResult result =
                    compiler.PreprocessGlsl(stageSource, kind, name.c_str(), options);
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    error = std::string(StageName(kind)) + ": " + result.GetErrorMessage();
                    return {};
                }
                stages.push_back({ kind, BlankComments(std::string(result.cbegin(), result.cend())) });
            }
            return stages;
        }

        // A program that still requires a Vulkan-only extension once OLO_VULKAN
        // is undefined (the ray-traced and ReSTIR programs) never reaches GL, on
        // either route.
        [[nodiscard]] bool IsVulkanOnly(const std::vector<RawRouteStage>& stages)
        {
            static const std::regex kVulkanOnlyExtension(
                R"(#\s*extension\s+GL_EXT_(ray_query|descriptor_heap|buffer_reference\w*|mesh_shader)\b)");
            return std::ranges::any_of(stages, [](const RawRouteStage& stage)
                                       { return std::regex_search(stage.Text, kVulkanOnlyExtension); });
        }

        // The uniform blocks one preprocessed stage declares.
        [[nodiscard]] std::vector<std::string> UniformBlocks(const RawRouteStage& stage)
        {
            static const std::regex kUniformBlock(R"(\buniform\s+([A-Za-z_]\w*)\s*\{)");
            std::vector<std::string> blocks;
            for (std::sregex_iterator it(stage.Text.begin(), stage.Text.end(), kUniformBlock), end; it != end; ++it)
            {
                blocks.push_back((*it)[1].str());
            }
            return blocks;
        }

        // The top-level graphics programs that opt in to the raw route and can
        // reach GL at all.
        [[nodiscard]] std::vector<fs::path> RawRoutePrograms(const fs::path& root, std::string& preprocessFailures,
                                                             u32& vulkanOnly)
        {
            shaderc::Compiler compiler;
            std::vector<fs::path> programs;
            for (const fs::path& shader : GraphicsShaders(root))
            {
                if (!OptsInToTheRawRoute(root, shader))
                {
                    continue;
                }
                std::string error;
                const std::vector<RawRouteStage> stages = PreprocessForRawRoute(root, shader, compiler, error);
                if (!error.empty())
                {
                    preprocessFailures += "\n    " + shader.filename().string() + " " + error;
                    continue;
                }
                // A mesh-shading program's task/mesh stages are Vulkan-only, and
                // SplitByType does not return them, so ask the file itself.
                static const std::regex kMeshShadingStage(R"(#type\s+(task|mesh)\b)");
                if (IsVulkanOnly(stages) || std::regex_search(SH::ReadWholeFile(shader), kMeshShadingStage))
                {
                    ++vulkanOnly;
                    continue;
                }
                programs.push_back(shader);
            }
            return programs;
        }

        // Puts the process's descriptor heap back however the test leaves.
        struct HeapRestore
        {
            RHI::IDescriptorHeapBackend* Backend = RHI::DescriptorHeap::Get().GetBackend();
            RHI::HeapDesc Desc = RHI::DescriptorHeap::Get().GetDesc();
            bool Enabled = RHI::DescriptorHeap::Get().IsEnabled();

            ~HeapRestore()
            {
                if (Backend != nullptr)
                {
                    RHI::DescriptorHeap::Get().Initialize(Desc, Backend);
                }
                RHI::DescriptorHeap::Get().SetEnabled(Enabled);
            }
        };
    } // namespace

    // The scanner itself, on the defect's shapes: a parameter, a local and a
    // use are each counted, the layout-qualifier spelling and prose are not.
    TEST(GlslDriverPortability, TheReservedIdentifierScanCountsUsesAndSkipsQualifiersAndProse)
    {
        const std::string source = "layout(std140, row_major) uniform A { mat4 m; };\n"
                                   "layout(packed) uniform B { vec4 v; };\n"
                                   "// a packed lane, see row_major above\n"
                                   "vec3 f(float packed) { return vec3(packed); }\n"
                                   "void g() { int register = 0; char c; }\n"
                                   "float packedTint; float unpacked; float row_majorish;\n";
        const std::vector<ReservedUse> uses = FindReservedIdentifierUses(source);
        ASSERT_EQ(uses.size(), 4u) << "expected packed x2 (line 4), register and char (line 5)";
        EXPECT_EQ(std::ranges::count_if(uses, [](const ReservedUse& u)
                                        { return u.Word == "packed" && u.Line == 4u; }),
                  2);
        EXPECT_EQ(std::ranges::count_if(uses, [](const ReservedUse& u)
                                        { return u.Line == 5u; }),
                  2);
    }

    // #1567. Every shader file, not only the ones on the raw route today: the
    // route is a per-file opt-in, and an include reaches many programs.
    TEST(GlslDriverPortability, NoShaderUsesAnIdentifierTheNvidiaFrontEndReserves)
    {
        const fs::path root = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(root)) << root.string();

        u32 scanned = 0;
        std::string report;
        for (const fs::path& file : SH::EnumerateShaderSources(root))
        {
            ++scanned;
            for (const ReservedUse& use : FindReservedIdentifierUses(SH::ReadWholeFile(file)))
            {
                report += "\n    " + fs::relative(file, root).generic_string() + ":" + std::to_string(use.Line) +
                          "  '" + std::string(use.Word) + "'";
            }
        }
        EXPECT_GT(scanned, 300u) << "the scan found too few shader files; the walk is broken, not the tree";
        EXPECT_TRUE(report.empty())
            << "These identifiers compile through glslang (the slotted route) but NVIDIA's GLSL front end "
               "rejects them on the raw bindless route (C1012 / C1503), so the program silently leaves the "
               "route. Rename them:"
            << report;
    }

    // #1565. Every stage of every program on the raw route declares no more
    // uniform blocks than GL 4.6 guarantees. Declared is an upper bound on what
    // the driver counts (active), so this holds on any conformant driver.
    TEST(BindlessRawRoute, EveryStageFitsTheGlMinimumUniformBlockBudget)
    {
        const fs::path root = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(root)) << root.string();
        std::multimap<sizet, std::string, std::greater<>> table;
        std::map<std::string, sizet> counts;
        std::string overBudget;
        std::string preprocessFailures;
        u32 vulkanOnly = 0;
        const std::vector<fs::path> programs = RawRoutePrograms(root, preprocessFailures, vulkanOnly);
        shaderc::Compiler compiler;
        for (const fs::path& shader : programs)
        {
            std::string error;
            for (const RawRouteStage& stage : PreprocessForRawRoute(root, shader, compiler, error))
            {
                const std::vector<std::string> blocks = UniformBlocks(stage);
                const std::string key = shader.stem().string() + ":" + StageName(stage.Kind);
                counts[key] = blocks.size();
                table.emplace(blocks.size(), key);
                if (blocks.size() > kGlMinimumUniformBlocksPerStage)
                {
                    std::string names;
                    for (const std::string& b : blocks)
                    {
                        names += " " + b;
                    }
                    overBudget += "\n    " + key + ": " + std::to_string(blocks.size()) + " —" + names;
                }
            }
        }

        std::string top;
        u32 shown = 0;
        for (const auto& [count, key] : table)
        {
            if (shown++ == 8u)
            {
                break;
            }
            top += "\n    " + std::to_string(count) + "  " + key;
        }
        GTEST_LOG_(INFO) << programs.size() << " GL programs on the raw route (" << vulkanOnly
                         << " Vulkan-only ones skipped); most uniform blocks per stage:" << top;

        EXPECT_GT(programs.size(), 40u) << "too few raw-route programs found; the opt-in scan is broken, not the tree";
        EXPECT_TRUE(preprocessFailures.empty()) << "could not preprocess:" << preprocessFailures;
        EXPECT_TRUE(overBudget.empty())
            << "These stages declare more uniform blocks than GL 4.6 guarantees per stage ("
            << kGlMinimumUniformBlocksPerStage
            << ", NVIDIA's limit). The raw route then fails to link with C5058 and the program leaves the "
               "route:"
            << overBudget;

        // The terrain's colour stages keep a block of headroom on NVIDIA: the
        // brush-preview and snow-accumulation constants reach them as flat
        // varyings from the tessellation-evaluation stage, not as two blocks.
        ASSERT_TRUE(counts.contains("Terrain_PBR:fragment"));
        EXPECT_LE(counts["Terrain_PBR:fragment"], kGlMinimumUniformBlocksPerStage - 1u);
        ASSERT_TRUE(counts.contains("Terrain_GBuffer:fragment"));
        EXPECT_LE(counts["Terrain_GBuffer:fragment"], kGlMinimumUniformBlocksPerStage - 1u);
    }

    // #1565 + #1567 on the real driver: every opted-in program, graphics and
    // compute, is BUILT on the raw route rather than falling back to the
    // slotted one. The CPU guards above cover the two failure classes known
    // today; this one covers whatever the driver rejects next.
    TEST(BindlessRawRoute, EveryOptedInProgramIsBuiltOnTheRawRouteByThisDriver)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HeapRestore restore;
        OpenGLDescriptorHeapBackend backend;
        backend.Initialize(kDescriptorHeapSlots);
        if (!backend.IsBindlessSupported())
        {
            GTEST_SKIP() << "GL_ARB_bindless_texture unavailable — the raw route is never taken on this device.";
        }
        RHI::HeapDesc desc;
        desc.ResourceSlotCapacity = kDescriptorHeapPersistentSlots;
        desc.SamplerSlotCapacity = kDescriptorHeapSamplerSlots;
        desc.FrameTransientRingSlots = kDescriptorHeapTransientSlots;
        RHI::DescriptorHeap::Get().Initialize(desc, &backend);
        RHI::DescriptorHeap::Get().SetEnabled(true);

        GLint maxFragmentBlocks = 0;
        glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &maxFragmentBlocks);

        const fs::path root = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        std::string offRoute;
        std::string measured;
        std::string preprocessFailures;
        u32 vulkanOnly = 0;
        const std::vector<fs::path> programs = RawRoutePrograms(root, preprocessFailures, vulkanOnly);
        EXPECT_TRUE(preprocessFailures.empty()) << "could not preprocess:" << preprocessFailures;
        for (const fs::path& path : programs)
        {
            const std::string relative = "assets/shaders/" + path.filename().string();
            Ref<Shader> shader = Shader::Create(relative);
            const auto* gl = shader ? dynamic_cast<const OpenGLShader*>(shader.Raw()) : nullptr;
            if (gl == nullptr)
            {
                offRoute += "\n    " + relative + ": not an OpenGL shader";
                continue;
            }
            shader->EnsureLinked();
            EXPECT_TRUE(gl->RequestedBindlessVariant())
                << relative << " opts in by this test's scan but the engine did not ask for the raw route; "
                               "the two opt-in predicates have drifted";
            if (!shader->IsReady() || !gl->IsBindlessVariant())
            {
                offRoute += "\n    " + relative + (shader->IsReady() ? ": fell back to the slotted route" : ": failed");
                continue;
            }

            const auto program = static_cast<GLuint>(shader->GetRendererID());
            GLint blocks = 0;
            glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
            GLint fragmentBlocks = 0;
            for (GLint b = 0; b < blocks; ++b)
            {
                GLint referenced = GL_FALSE;
                glGetActiveUniformBlockiv(program, static_cast<GLuint>(b), GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER,
                                          &referenced);
                fragmentBlocks += referenced != GL_FALSE ? 1 : 0;
            }
            if (path.stem() == "Terrain_PBR" || path.stem() == "Terrain_GBuffer" || path.stem() == "PBR_MultiLight")
            {
                measured += "\n    " + path.stem().string() + ": " + std::to_string(fragmentBlocks) + " / " +
                            std::to_string(maxFragmentBlocks) + " active fragment uniform blocks";
            }
            if (path.stem() == "Terrain_PBR")
            {
                EXPECT_LT(fragmentBlocks, maxFragmentBlocks)
                    << "Terrain_PBR links, but with no fragment uniform block to spare on this driver";
            }
        }

        u32 compute = 0;
        shaderc::Compiler compiler;
        for (const fs::path& path : SH::EnumerateShaderSources(root / "compute"))
        {
            if (!OptsInToTheRawRoute(root, path))
            {
                continue;
            }
            std::string error;
            const std::vector<RawRouteStage> stages = PreprocessForRawRoute(root, path, compiler, error);
            const bool isCompute = std::ranges::any_of(stages, [](const RawRouteStage& stage)
                                                       { return stage.Kind == shaderc_glsl_compute_shader; });
            if (!error.empty() || !isCompute || IsVulkanOnly(stages))
            {
                continue;
            }
            ++compute;
            const std::string relative = "assets/shaders/compute/" + fs::relative(path, root / "compute").generic_string();
            const Ref<ComputeShader> shader = ComputeShader::Create(relative);
            const auto* gl = shader ? dynamic_cast<const OpenGLComputeShader*>(shader.Raw()) : nullptr;
            if (gl == nullptr || !gl->IsValid() || !gl->IsBindlessVariant())
            {
                offRoute += "\n    " + relative + ((gl != nullptr && gl->IsValid()) ? ": fell back to the slot-based build" : ": failed");
            }
        }

        GTEST_LOG_(INFO) << programs.size() << " graphics and " << compute
                         << " compute programs on the raw route; GL_MAX_FRAGMENT_UNIFORM_BLOCKS = " << maxFragmentBlocks
                         << measured;
        EXPECT_GT(programs.size(), 40u) << "too few raw-route programs found; the opt-in scan is broken, not the tree";
        EXPECT_TRUE(offRoute.empty()) << "These programs opt in to the bindless route and were NOT built on it "
                                         "by this driver — OloEngine.log has the driver's reason:"
                                      << offRoute;
    }
} // namespace OloEngine::Tests
