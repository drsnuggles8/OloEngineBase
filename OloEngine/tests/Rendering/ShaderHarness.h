#pragma once

// =============================================================================
// ShaderHarness — file-walk + shaderc-compile helpers shared by Rendering tests
// that need to operate on every production .glsl / .comp file on disk.
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
#include <regex>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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

    /// Every shader-like source under `root`, `include/` and `tests/` included:
    /// the set a binding-collision scan must cover. Sorted for stable output.
    inline std::vector<fs::path> EnumerateShaderSources(const fs::path& root)
    {
        std::vector<fs::path> out;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".comp" && ext != ".glsl" && ext != ".vert" && ext != ".frag" && ext != ".geom")
                continue;
            out.push_back(entry.path());
        }
        std::ranges::sort(out);
        return out;
    }

    /// The paths, as written, of every `#include "..."` directive in `source`.
    /// One level, and a commented-out directive counts: the scans that use this
    /// treat a mention as a use, which errs towards a spurious failure rather
    /// than a missed collision.
    inline std::vector<std::string> IncludedPaths(const std::string& source)
    {
        static const std::regex kInclude{ R"INC(#\s*include\s*"([^"]+)")INC" };
        std::vector<std::string> paths;
        for (std::sregex_iterator it{ source.begin(), source.end(), kInclude }, end; it != end; ++it)
            paths.push_back((*it)[1].str());
        return paths;
    }

    /// The file names named by every `#include "..."` directive in `source`.
    inline std::vector<std::string> IncludedFileNames(const std::string& source)
    {
        std::vector<std::string> names;
        for (const std::string& path : IncludedPaths(source))
            names.push_back(fs::path{ path }.filename().string());
        return names;
    }

    /// Resolve an include the way the engine's includer does: relative to
    /// `root` first, then under `root/include/` by file name.
    inline fs::path ResolveInclude(const fs::path& root, const std::string& includePath)
    {
        if (const fs::path direct = root / includePath; fs::exists(direct))
            return direct.lexically_normal();
        return (root / "include" / fs::path{ includePath }.filename()).lexically_normal();
    }

    /// The transitive include closure of `path` (existing files only, `path`
    /// itself excluded), following `#include "..."` through ResolveInclude.
    inline std::vector<fs::path> IncludeClosure(const fs::path& root, const fs::path& path)
    {
        const fs::path self = path.lexically_normal();
        std::vector<fs::path> closure;
        std::vector<fs::path> pending{ self };
        while (!pending.empty())
        {
            const fs::path current = pending.back();
            pending.pop_back();
            for (const std::string& include : IncludedPaths(ReadWholeFile(current)))
            {
                const fs::path resolved = ResolveInclude(root, include);
                if (!fs::exists(resolved) || resolved == self)
                    continue;
                if (std::ranges::find(closure, resolved) != closure.end())
                    continue;
                closure.push_back(resolved);
                pending.push_back(resolved);
            }
        }
        return closure;
    }

    /// The binding number of every `layout(... binding = N ...) buffer` block
    /// declared in `source`. Storage blocks only: samplers, images and uniform
    /// blocks are ignored.
    inline std::vector<u32> DeclaredStorageBufferBindings(const std::string& source)
    {
        static const std::regex kStorageBlock{
            R"BLK(layout\s*\(([^)]*)\)\s*(?:(?:readonly|writeonly|coherent|restrict|volatile)\s+)*buffer\b)BLK"
        };
        static const std::regex kBinding{ R"BND(binding\s*=\s*(\d+))BND" };
        std::vector<u32> bindings;
        for (std::sregex_iterator it{ source.begin(), source.end(), kStorageBlock }, end; it != end; ++it)
        {
            const std::string qualifiers = (*it)[1].str();
            std::smatch binding;
            if (std::regex_search(qualifiers, binding, kBinding))
                bindings.push_back(static_cast<u32>(std::stoul(binding[1].str())));
        }
        return bindings;
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

    /// Walk `root` recursively for `.glsl` and `.comp` files, skipping
    /// `include/` (headers, no `#type` stages) and `tests/` (compute-shader
    /// harnesses). `.comp` files are plain single-stage GLSL compute
    /// shaders with no `#type` header — see `SplitStages` below for how
    /// they're parsed.
    inline std::vector<fs::path> EnumerateProductionShaders(const fs::path& root)
    {
        std::vector<fs::path> out;
        for (auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            const fs::path ext = entry.path().extension();
            if (ext != ".glsl" && ext != ".comp")
                continue;

            if (const std::string rel = fs::relative(entry.path(), root).generic_string(); rel.starts_with("include/") || rel.starts_with("tests/"))
                continue;

            out.push_back(entry.path());
        }
        std::ranges::sort(out);
        return out;
    }

    /// Split a shader source into (kind, source) stage pairs. Handles both
    /// `#type`-delimited multi-stage `.glsl` files (via SplitByType) and
    /// standalone `.comp` / compute-only `.glsl` files that have no `#type`
    /// header and just declare `layout(local_size_x = ...) in;` directly —
    /// matches `OpenGLComputeShader`'s behaviour of treating the whole file
    /// as a single compute stage.
    inline std::vector<std::pair<shaderc_shader_kind, std::string>> SplitStages(const std::string& src)
    {
        auto stages = SplitByType(src);
        if (stages.empty() && src.find("local_size_x") != std::string::npos)
            stages.emplace_back(shaderc_glsl_compute_shader, src);
        return stages;
    }

    /// Compile a single `#type`-stage source to SPIR-V using the same options
    /// the engine uses at runtime (target_env = Vulkan 1.2, preserve_bindings,
    /// no auto-bind) for vertex/fragment/geometry/tessellation stages, which
    /// go through `OpenGLShader::CompileOrGetVulkanBinaries`. Compute stages
    /// go through the separate `OpenGLComputeShader`, which compiles GLSL
    /// natively (no shaderc/SPIR-V step at runtime) and therefore permits
    /// plain `uniform` declarations outside a block with no explicit
    /// `layout(location=L)` — legal core GLSL (locations are resolved via
    /// `glGetUniformLocation` at link time) that the Vulkan SPIR-V target
    /// env rejects outright ("non-opaque uniforms outside a block") and the
    /// OpenGL SPIR-V target env (ARB_gl_spirv semantics) requires an
    /// explicit location for. Compiling compute stages under the OpenGL
    /// target env with auto-mapped locations accepts that syntax while
    /// still producing SPIR-V for spirv-cross reflection (UBO sizes/
    /// offsets, bindings), which is the closest headless approximation of
    /// the real compile path. Returns the shaderc result so callers can
    /// inspect the compilation status and SPIR-V output.
    inline shaderc::SpvCompilationResult CompileStageToSpv(
        const fs::path& shaderPath,
        const std::string& stageSource,
        shaderc_shader_kind kind,
        const fs::path& root,
        shaderc::Compiler& compiler,
        bool generateDebugInfo = false)
    {
        shaderc::CompileOptions options;
        if (kind == shaderc_glsl_compute_shader)
        {
            options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
            options.SetAutoMapLocations(true);
        }
        else
        {
            options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        }
        options.SetPreserveBindings(true);
        options.SetAutoBindUniforms(false);
        options.SetSuppressWarnings();
        options.SetIncluder(std::make_unique<Includer>(root));
        // Debug info (OpName/OpMemberName) is opt-in: it's off by default to
        // match the engine's runtime compile path, and only the callers that
        // need member *names* out of reflection (rather than just offsets/
        // sizes) should pay for it.
        if (generateDebugInfo)
            options.SetGenerateDebugInfo();

        const std::string name = shaderPath.generic_string();
        return compiler.CompileGlslToSpv(stageSource, kind, name.c_str(), options);
    }

    /// Compile one raster stage through the production VulkanShader tier:
    /// Vulkan 1.4, SPIR-V 1.6, OLO_VULKAN=1, debug names and performance
    /// optimisation. Contract tests use this route when the authoring-side
    /// backend fork itself is what must be reflected (not the OpenGL-via-SPIR-V
    /// tier used by CompileStageToSpv above).
    inline shaderc::SpvCompilationResult CompileVulkanBackendStageToSpv(
        const fs::path& shaderPath,
        const std::string& stageSource,
        shaderc_shader_kind kind,
        const fs::path& root,
        shaderc::Compiler& compiler)
    {
        constexpr auto kShadercEnvVulkan14 = static_cast<shaderc_env_version>((1u << 22) | (4u << 12));
        constexpr auto kShadercSpirv16 = static_cast<shaderc_spirv_version>((1u << 16) | (6u << 8));
        constexpr auto kShadercSpirv15 = static_cast<shaderc_spirv_version>((1u << 16) | (5u << 8));
        static_assert(static_cast<u32>(kShadercEnvVulkan14) == 0x00404000u);
        static_assert(static_cast<u32>(kShadercSpirv16) == 0x00010600u);
        static_assert(static_cast<u32>(kShadercSpirv15) == 0x00010500u);

        const std::string name = shaderPath.generic_string();
        const auto compileForEnvironment = [&](shaderc_env_version environment, shaderc_spirv_version spirvVersion)
        {
            shaderc::CompileOptions options;
            options.SetTargetEnvironment(shaderc_target_env_vulkan, environment);
            options.SetTargetSpirv(spirvVersion);
            options.SetPreserveBindings(true);
            options.SetAutoBindUniforms(false);
            options.SetGenerateDebugInfo();
            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            options.SetSuppressWarnings();
            options.AddMacroDefinition("OLO_VULKAN", "1");
            options.SetIncluder(std::make_unique<Includer>(root));
            return compiler.CompileGlslToSpv(stageSource, kind, name.c_str(), options);
        };

        auto result = compileForEnvironment(kShadercEnvVulkan14, kShadercSpirv16);
        // Ubuntu's shaderc 2023.8 accepts the hand-encoded Vulkan 1.4 value but
        // silently treats it as Vulkan 1.0. Its optimiser then rejects the
        // explicitly requested SPIR-V 1.6 module. Production cannot run Vulkan
        // on that pre-1.4 toolchain, but sanitizer CI still needs to reflect the
        // same OLO_VULKAN interface. Retry only that toolchain diagnostic with
        // the Vulkan 1.2/SPIR-V 1.5 pair already used by CompileStageToSpv; the
        // reflected stage inputs, outputs and descriptor bindings are unchanged.
        constexpr std::string_view kUnsupportedVulkan14Diagnostic =
            "Invalid SPIR-V binary version 1.6 for target environment SPIR-V 1.0";
        if (std::string_view(result.GetErrorMessage()).contains(kUnsupportedVulkan14Diagnostic))
        {
            return compileForEnvironment(shaderc_env_version_vulkan_1_2, kShadercSpirv15);
        }
        return result;
    }
} // namespace OloEngine::Tests::ShaderHarness
