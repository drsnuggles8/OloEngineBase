#include "OloEnginePCH.h"
#include "OloEngine/Renderer/BackendSelection.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string_view>

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

    std::filesystem::path DefaultRendererConfigPath()
    {
        return std::filesystem::path("config") / "renderer.yaml";
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
            selection.Diagnostic = std::string("unknown backend '") + std::string(value) +
                                   "' in --rhi= (expected 'opengl' or 'vulkan'). Falling back to OpenGL.";
            return selection;
        }

        // 2) Config-file fallback: Renderer: { RHI: <name> }. Absent or malformed
        // config is the common case (no file ships by default) and is silent.
        if (!configFile.empty() && std::filesystem::exists(configFile))
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
