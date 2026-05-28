#pragma once

// =============================================================================
// ShaderHarness — file-walk + shaderc-compile helpers shared by Rendering tests
// that need to operate on every production .glsl file on disk.
//
// Mirrors what `OpenGLShader::PreProcess` + `CompileOrGetVulkanBinaries` do at
// runtime (split on `#type`, compile each stage with target_env = vulkan 1.2,
// preserve_bindings = true) but as free functions that produce SPIR-V byte
// vectors instead of GL program objects.
//
// Currently used by:
//   - ShaderReflectionBindingTest.cpp (binding-vs-C++ contract validation)
//
// A near-duplicate of the same logic lives inline inside
// `PropertyTests/ShaderCompilationTest.cpp`. That file predates this header
// and was intentionally left untouched to avoid risk; if a third test ever
// needs the harness, that file should be migrated to include this header
// instead of growing a third copy.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests::ShaderHarness
{
    namespace fs = std::filesystem;

    inline constexpr const char* kShaderRoot = "OloEditor/assets/shaders";

    /// Find OloEditor/assets/shaders/ relative to the current working directory.
    /// Tests may run from the repo root (ctest) or from inside OloEditor/
    /// (editor-launched suites or `gtest_discover_tests` WORKING_DIRECTORY).
    inline fs::path ResolveShaderRoot()
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

    inline std::string ReadWholeFile(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream oss;
        oss << f.rdbuf();
        return oss.str();
    }

    inline shaderc_shader_kind StageFromToken(const std::string& tok)
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

    /// Split a raw .glsl file on `#type <name>` headers. Matches
    /// `OpenGLShader::PreProcess`.
    inline std::vector<std::pair<shaderc_shader_kind, std::string>> SplitByType(const std::string& src)
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

            if (const shaderc_shader_kind kind = StageFromToken(tok); kind != static_cast<shaderc_shader_kind>(-1))
                out.emplace_back(kind, src.substr(next, end - next));
            pos = nextTypePos;
        }
        return out;
    }

    /// shaderc Includer that resolves `#include "..."` first relative to the
    /// requesting file's directory, then to the shader root. Mirrors the
    /// engine's convention where top-level shaders reference
    /// `include/Foo.glsl` and nested includes use sibling paths.
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
            if (const fs::path reqDir = fs::path(requesting_source).parent_path(); !reqDir.empty())
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
                    resolved = candidate;
            }

            if (fs::exists(resolved, ec))
            {
                payload->Name = resolved.generic_string();
                payload->Content = ReadWholeFile(resolved);
                payload->Result.source_name = payload->Name.c_str();
                payload->Result.source_name_length = payload->Name.size();
                payload->Result.content = payload->Content.c_str();
                payload->Result.content_length = payload->Content.size();
            }
            else
            {
                payload->Name.clear();
                payload->Content = "failed to resolve include '" + std::string(requested_source) +
                                   "' from '" + std::string(requesting_source) + "'";
                payload->Result.source_name = payload->Name.c_str();
                payload->Result.source_name_length = 0;
                payload->Result.content = payload->Content.c_str();
                payload->Result.content_length = payload->Content.size();
            }
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

    /// Walk `root` recursively for `.glsl` files, skipping `include/`
    /// (headers, no `#type` stages) and `tests/` (compute-shader harnesses).
    inline std::vector<fs::path> EnumerateProductionShaders(const fs::path& root)
    {
        std::vector<fs::path> out;
        for (auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".glsl")
                continue;

            if (const std::string rel = fs::relative(entry.path(), root).generic_string(); rel.starts_with("include/") || rel.starts_with("tests/"))
                continue;

            out.push_back(entry.path());
        }
        std::ranges::sort(out);
        return out;
    }

    /// Compile a single `#type`-stage source to SPIR-V using the same options
    /// the engine uses at runtime (target_env = Vulkan 1.2, preserve_bindings,
    /// no auto-bind). Returns the shaderc result so callers can inspect the
    /// compilation status and SPIR-V output.
    inline shaderc::SpvCompilationResult CompileStageToSpv(
        const fs::path& shaderPath,
        const std::string& stageSource,
        shaderc_shader_kind kind,
        const fs::path& root,
        shaderc::Compiler& compiler)
    {
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        options.SetPreserveBindings(true);
        options.SetAutoBindUniforms(false);
        options.SetSuppressWarnings();
        options.SetIncluder(std::make_unique<Includer>(root));

        const std::string name = shaderPath.generic_string();
        return compiler.CompileGlslToSpv(stageSource, kind, name.c_str(), options);
    }
} // namespace OloEngine::Tests::ShaderHarness
