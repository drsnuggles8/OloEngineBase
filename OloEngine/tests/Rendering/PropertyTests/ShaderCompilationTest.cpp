// =============================================================================
// ShaderCompilationTest.cpp
//
// Layer-2 *catch-all* test: every production `.glsl` file under
// `OloEditor/assets/shaders/` (excluding `include/` headers and the
// `tests/` compute-shader harnesses) is preprocessed and handed to
// shaderc with the **Vulkan SPIR-V target environment** — exactly the
// path the engine takes at runtime via `OpenGLShader::CompileOrGetVulkanBinaries`.
//
// Why this test exists
// --------------------
// Property tests (Layer 1) are CPU mirrors of shader math — they never
// touch the `.glsl` files. The existing shader-unit tests (Layer 2) drive
// curated compute-shader harnesses under `assets/shaders/tests/`, which
// only cover a small subset of production shaders. Engine regressions in
// `DeferredLighting.glsl`, `PBR_GBuffer*.glsl`, or any fullscreen pass
// would therefore slip past `OloEngine-Tests.exe` and only surface when a
// developer launches the editor (the `gl_VertexID` → `gl_VertexIndex`
// Vulkan-env mismatch that motivated this test being a concrete example).
//
// The test replicates the engine's preprocessing:
//   1. Read the file.
//   2. Split on `#type {vertex|fragment|geometry|tess_*|compute}`.
//   3. For each stage, run shaderc with
//        target_env = vulkan (1.2),
//        preserve_bindings = true,
//        suppress_warnings = true,
//      with an includer that resolves `#include "…"` relative to
//      `OloEditor/assets/shaders/`.
//   4. Assert `compilation_status == success`.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr const char* kShaderRoot = "OloEditor/assets/shaders";

        // Locate the shader root relative to the current working directory.
        // Tests may run from the repo root (`ctest`) or from inside
        // `OloEditor/` (editor-launched suites). We probe a few candidates.
        fs::path ResolveShaderRoot()
        {
            const fs::path candidates[] = {
                fs::path(kShaderRoot),
                fs::current_path() / kShaderRoot,
                fs::current_path().parent_path() / kShaderRoot,
                fs::current_path() / "assets" / "shaders",
            };
            for (const auto& c : candidates)
            {
                std::error_code ec;
                if (fs::exists(c, ec) && fs::is_directory(c, ec))
                    return fs::canonical(c, ec);
            }
            return {};
        }

        std::string ReadWholeFile(const fs::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream oss;
            oss << f.rdbuf();
            return oss.str();
        }

        shaderc_shader_kind StageFromToken(const std::string& tok)
        {
            if (tok == "vertex")
                return shaderc_glsl_vertex_shader;
            if (tok == "fragment")
                return shaderc_glsl_fragment_shader;
            if (tok == "geometry")
                return shaderc_glsl_geometry_shader;
            if (tok == "tess_control" || tok == "tesscontrol" || tok == "tessellation_control")
                return shaderc_glsl_tess_control_shader;
            if (tok == "tess_eval" || tok == "tess_evaluation" || tok == "tesseval" ||
                tok == "tessevaluation" || tok == "tessellation_evaluation")
                return shaderc_glsl_tess_evaluation_shader;
            if (tok == "compute")
                return shaderc_glsl_compute_shader;
            return static_cast<shaderc_shader_kind>(-1);
        }

        // Split the raw file on `#type <name>` headers. Matches OpenGLShader::PreProcess.
        std::vector<std::pair<shaderc_shader_kind, std::string>> SplitByType(const std::string& src)
        {
            std::vector<std::pair<shaderc_shader_kind, std::string>> out;

            static const std::string kToken = "#type";
            std::size_t pos = src.find(kToken);
            while (pos != std::string::npos)
            {
                const std::size_t eol = src.find_first_of("\r\n", pos);
                if (eol == std::string::npos)
                    break;

                std::size_t s = pos + kToken.size();
                while (s < eol && (src[s] == ' ' || src[s] == '\t'))
                    ++s;
                std::size_t e = s;
                while (e < eol && src[e] != ' ' && src[e] != '\t' && src[e] != '\r' && src[e] != '\n')
                    ++e;
                const std::string tok = src.substr(s, e - s);

                const std::size_t next = src.find_first_not_of("\r\n", eol);
                if (next == std::string::npos)
                    break;
                const std::size_t nextTypePos = src.find(kToken, next);
                const std::size_t end = (nextTypePos == std::string::npos) ? src.size() : nextTypePos;

                const shaderc_shader_kind kind = StageFromToken(tok);
                if (kind != static_cast<shaderc_shader_kind>(-1))
                    out.emplace_back(kind, src.substr(next, end - next));
                pos = nextTypePos;
            }
            return out;
        }

        // shaderc Includer that resolves quoted/angle includes.
        //
        // The engine's convention:
        //   * top-level shaders reference `#include "include/Foo.glsl"`
        //     relative to the shader root,
        //   * nested includes inside `include/` reference sibling headers
        //     relative to the requesting file (e.g. SnowCommon.glsl does
        //     `#include "WindSampling.glsl"`).
        //
        // We therefore resolve each `requested_source` against the parent
        // directory of `requesting_source`, falling back to the shader
        // root for the top-level request (where requesting_source is the
        // name we passed to CompileGlslToSpv — an absolute path into
        // assets/shaders/).
        class Includer : public shaderc::CompileOptions::IncluderInterface
        {
          public:
            explicit Includer(fs::path root) : m_Root(std::move(root)) {}

            shaderc_include_result* GetInclude(const char* requested_source,
                                               shaderc_include_type,
                                               const char* requesting_source,
                                               size_t /*include_depth*/) override
            {
                auto* payload = new Payload();

                fs::path resolved;
                std::error_code ec;
                const fs::path reqDir = fs::path(requesting_source).parent_path();
                if (!reqDir.empty())
                {
                    const fs::path candidate = reqDir / requested_source;
                    if (fs::exists(candidate, ec))
                        resolved = fs::weakly_canonical(candidate, ec);
                }
                if (resolved.empty())
                {
                    const fs::path candidate = m_Root / requested_source;
                    if (fs::exists(candidate, ec))
                        resolved = fs::weakly_canonical(candidate, ec);
                    else
                        resolved = candidate; // report missing path in error
                }

                payload->Name = resolved.generic_string();
                if (fs::exists(resolved, ec))
                    payload->Content = ReadWholeFile(resolved);

                payload->Result.source_name = payload->Name.c_str();
                payload->Result.source_name_length = payload->Name.size();
                payload->Result.content = payload->Content.c_str();
                payload->Result.content_length = payload->Content.size();
                payload->Result.user_data = payload;
                return &payload->Result;
            }

            void ReleaseInclude(shaderc_include_result* data) override
            {
                delete static_cast<Payload*>(data->user_data);
            }

          private:
            struct Payload
            {
                shaderc_include_result Result{};
                std::string Name;
                std::string Content;
            };

            fs::path m_Root;
        };

        std::vector<fs::path> EnumerateProductionShaders(const fs::path& root)
        {
            std::vector<fs::path> out;
            for (auto& entry : fs::recursive_directory_iterator(root))
            {
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().extension() != ".glsl")
                    continue;

                // Skip include/ (headers, no #type stages) and tests/
                // (compute-shader test harnesses covered elsewhere).
                const std::string rel = fs::relative(entry.path(), root).generic_string();
                if (rel.starts_with("include/") || rel.starts_with("tests/"))
                    continue;

                out.push_back(entry.path());
            }
            std::sort(out.begin(), out.end());
            return out;
        }
    } // namespace

    // Single test that iterates every production shader. We report every
    // failure (rather than early-exiting) so a single edit to a shared
    // include produces a complete list of affected files instead of one
    // at a time.
    TEST(ShaderCompilation, AllProductionShadersCompileUnderVulkanTarget)
    {
        const fs::path root = ResolveShaderRoot();
        ASSERT_FALSE(root.empty())
            << "Could not resolve OloEditor/assets/shaders root (cwd = "
            << fs::current_path().generic_string() << ")";

        const auto shaders = EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty()) << "No .glsl files found under " << root;

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        u32 stagesCompiled = 0;
        std::vector<std::string> failures;

        for (const auto& path : shaders)
        {
            const std::string source = ReadWholeFile(path);
            auto stages = SplitByType(source);

            // Files with no `#type` header (e.g. compute/*.glsl) are
            // standalone compute shaders — treat the whole file as the
            // compute stage, matching OpenGLComputeShader's behaviour.
            if (stages.empty() && source.find("local_size_x") != std::string::npos)
                stages.emplace_back(shaderc_glsl_compute_shader, source);

            if (stages.empty())
            {
                failures.push_back(path.generic_string() + ": no #type stages found and no compute layout detected");
                continue;
            }

            for (const auto& [kind, stageSource] : stages)
            {
                shaderc::CompileOptions options;
                options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                options.SetPreserveBindings(true);
                options.SetAutoBindUniforms(false);
                options.SetSuppressWarnings();
                options.SetIncluder(std::make_unique<Includer>(root));

                const std::string name = path.generic_string();
                auto result = compiler.CompileGlslToSpv(
                    stageSource, kind, name.c_str(), options);

                ++stagesCompiled;
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    std::ostringstream oss;
                    oss << name << " stage " << static_cast<int>(kind) << ":\n"
                        << result.GetErrorMessage();
                    failures.push_back(oss.str());
                }
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " shader(s) failed to compile under Vulkan target env:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f << "\n";
            FAIL() << oss.str();
        }

        EXPECT_GT(stagesCompiled, 0u);
    }
} // namespace OloEngine::Tests
