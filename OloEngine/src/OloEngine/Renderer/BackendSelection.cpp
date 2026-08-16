#include "OloEnginePCH.h"
#include "OloEngine/Renderer/BackendSelection.h"

#include "OloEngine/Core/FileSystem.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] std::string ToLower(std::string_view s)
        {
            std::string out(s);
            std::ranges::transform(out, out.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // Maps a backend name to an API, or nullopt for an unrecognised name.
        [[nodiscard]] std::optional<RendererAPI::API> ParseBackendName(std::string_view name)
        {
            const std::string lowered = ToLower(name);
            if (lowered == "opengl")
            {
                return RendererAPI::API::OpenGL;
            }
            if (lowered == "vulkan")
            {
                return RendererAPI::API::Vulkan;
            }
            return std::nullopt;
        }

        // Applies the availability check to a *requested* API. The degrade here is
        // build-time availability only ("this binary doesn't contain the backend");
        // runtime capability failures refuse to init inside VulkanContext instead.
        [[nodiscard]] BackendSelection Resolve(RendererAPI::API requested, std::string source, std::string_view requestedName)
        {
            BackendSelection selection;
            selection.Source = std::move(source);
#if !OLO_WITH_VULKAN
            if (requested == RendererAPI::API::Vulkan)
            {
                selection.Api = RendererAPI::API::OpenGL;
                selection.Diagnostic = std::string("'") + std::string(requestedName) +
                                       "' was requested but this binary was built with OLO_WITH_VULKAN=OFF — "
                                       "the Vulkan backend is not compiled in. Falling back to OpenGL.";
                return selection;
            }
#else
            (void)requestedName;
#endif
            selection.Api = requested;
            return selection;
        }
    } // namespace

    std::filesystem::path ResolveRendererConfigPath(const std::filesystem::path& base,
                                                    const std::filesystem::path& exeDir)
    {
        const auto relative = std::filesystem::path("config") / "renderer.yaml";

        // base (cwd) first: the editor runs with cwd = OloEditor/ and keeps its
        // config there (gitignored), and a packaged game double-clicked from its
        // own folder resolves identically either way.
        std::error_code ec;
        if (auto atBase = base / relative; std::filesystem::exists(atBase, ec))
        {
            return atBase;
        }

        // Fall back to the file shipped next to the executable — this is what
        // rescues a packaged game launched with an unexpected working directory
        // (#691 Phase 9). Only when that file actually exists: the creation
        // default below stays base-anchored so a fresh editor write lands in
        // OloEditor/config/, not in the build output tree.
        if (!exeDir.empty())
        {
            if (auto anchored = exeDir / relative; std::filesystem::exists(anchored, ec))
            {
                return anchored;
            }
        }

        return base / relative;
    }

    std::filesystem::path DefaultRendererConfigPath()
    {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (ec)
        {
            cwd.clear();
        }
        return ResolveRendererConfigPath(cwd, FileSystem::GetExecutableDirectory());
    }

    bool WriteRendererConfig(const std::filesystem::path& configFile, RendererAPI::API api)
    {
        std::error_code ec;
        if (const auto dir = configFile.parent_path(); !dir.empty())
        {
            std::filesystem::create_directories(dir, ec);
        }
        std::ofstream config(configFile, std::ios::trunc);
        if (!config)
        {
            return false;
        }
        // Exactly the schema SelectRendererBackend parses; see the header note.
        config << "# Written by the engine's renderer-backend selection UI (ADR 0011 s2).\n"
               << "# `--rhi=opengl|vulkan` on the command line overrides this file.\n"
               << "Renderer:\n"
               << "  RHI: " << (api == RendererAPI::API::Vulkan ? "vulkan" : "opengl") << "\n";
        return static_cast<bool>(config);
    }

    BackendSelection SelectRendererBackend(int argc, char** argv, const std::filesystem::path& configFile)
    {
        // 1) `--rhi=<name>` on the command line (argv[0] is the program name).
        constexpr std::string_view kFlagPrefix = "--rhi=";
        for (int i = 1; i < argc; ++i)
        {
            if (argv[i] == nullptr)
            {
                continue;
            }
            const std::string_view arg(argv[i]);
            if (!arg.starts_with(kFlagPrefix))
            {
                continue;
            }
            const std::string_view value = arg.substr(kFlagPrefix.size());
            if (const auto api = ParseBackendName(value))
            {
                return Resolve(*api, "--rhi flag", value);
            }
            BackendSelection selection;
            // Source names where the (rejected) request came from, same as the
            // Resolve() degrade path — the Api is the default, the origin is not.
            selection.Source = "--rhi flag";
            selection.Diagnostic = std::string("unknown backend '") + std::string(value) +
                                   "' in --rhi= (expected 'opengl' or 'vulkan'). Falling back to OpenGL.";
            return selection;
        }

        // 2) Config-file fallback: Renderer: { RHI: <name> }. Absent, unreadable or
        // malformed config is the common case (no file ships by default) and is
        // silent — the error_code overload keeps filesystem errors non-throwing.
        std::error_code existsError;
        if (!configFile.empty() && std::filesystem::exists(configFile, existsError))
        {
            try
            {
                std::ifstream stream(configFile);
                const YAML::Node data = YAML::Load(stream);
                if (const YAML::Node renderer = data["Renderer"])
                {
                    if (const YAML::Node rhi = renderer["RHI"])
                    {
                        const auto value = rhi.as<std::string>();
                        if (const auto api = ParseBackendName(value))
                        {
                            return Resolve(*api, "config file", value);
                        }
                        BackendSelection selection;
                        selection.Source = "config file";
                        selection.Diagnostic = std::string("unknown backend '") + value + "' in " +
                                               configFile.string() + " (expected 'opengl' or 'vulkan'). Falling back to OpenGL.";
                        return selection;
                    }
                }
            }
            catch (const YAML::Exception&)
            {
                // Malformed config must not block startup; the default is safe.
            }
        }

        // 3) Default.
        return {};
    }
} // namespace OloEngine
