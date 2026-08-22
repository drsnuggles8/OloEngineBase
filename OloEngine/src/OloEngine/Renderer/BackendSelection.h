#pragma once

#include "OloEngine/Renderer/RendererAPI.h"

#include <filesystem>
#include <string>

namespace OloEngine
{
    // Result of resolving which RHI backend this process should use (ADR 0011 §2).
    // Selection is a RUNTIME switch: `--rhi=opengl|vulkan` at process start, falling
    // back to a config setting, falling back to OpenGL. It is deliberately separate
    // from the build-time OLO_WITH_VULKAN availability switch.
    struct BackendSelection
    {
        RendererAPI::API Api = RendererAPI::API::OpenGL;

        // Where the choice came from, for the startup log: "--rhi flag",
        // "config file", or "default".
        std::string Source = "default";

        // Non-empty when the user's request could not be honoured (unknown backend
        // name, or a backend that is not compiled into this binary). The selection
        // then degrades to OpenGL — loudly, never silently: the caller must log this
        // at error level. A runtime CAPABILITY failure (device below ADR 0010's
        // contract) is NOT handled here — that refuses to initialise inside
        // VulkanContext instead of degrading, per ADR 0010's no-silent-fallback rule.
        std::string Diagnostic;
    };

    // Resolve the backend for this process. Pure and side-effect-free apart from
    // reading `configFile` if it exists — no logging, no static writes — so it is
    // unit-testable headlessly; the caller applies the result via RendererAPI::SetAPI
    // and logs Source/Diagnostic. MUST be called (and applied) before Window::Create;
    // see RendererAPI::SetAPI for the ordering contract.
    //
    // Chain: `--rhi=<name>` among argv[1..] → a `Renderer: { RHI: <name> }` mapping in
    // `configFile` (YAML; silently skipped when absent or malformed) → OpenGL.
    // Recognised names (case-insensitive): "opengl", "vulkan".
    [[nodiscard]] BackendSelection SelectRendererBackend(int argc, char** argv, const std::filesystem::path& configFile);

    // The config file the engine reads the fallback setting from. Resolution
    // (#691): `config/renderer.yaml` under the process working directory
    // if that file exists (the editor's shape — Application pins the cwd from its
    // specification before selection runs); otherwise the same relative path under
    // the executable's own directory if THAT file exists (the packaged-game shape,
    // robust against a shortcut with a stale "Start in"); otherwise the cwd path,
    // as the creation default. The editor Renderer Settings dropdown writes this
    // file; it applies on restart.
    [[nodiscard]] std::filesystem::path DefaultRendererConfigPath();

    // The pure resolution rule behind DefaultRendererConfigPath, parameterised on
    // the two anchors so it is unit-testable with temp directories. `exeDir` may
    // be empty (platform could not answer); the return is `base`-anchored then.
    [[nodiscard]] std::filesystem::path ResolveRendererConfigPath(const std::filesystem::path& base,
                                                                  const std::filesystem::path& exeDir);

    // Persist a backend choice to `configFile` in exactly the schema
    // SelectRendererBackend parses — one writer shape shared by every UI that
    // offers the choice (the editor dropdown today, a runtime settings screen
    // later), so the parser and the writer cannot drift. Creates the parent
    // directory; returns false when the file cannot be written. The choice
    // applies on the next process start, never live.
    bool WriteRendererConfig(const std::filesystem::path& configFile, RendererAPI::API api);
} // namespace OloEngine
