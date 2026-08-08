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

    // The config file the engine reads the fallback setting from, relative to the
    // process working directory (which Application sets from its specification
    // before selection runs). A future editor settings dropdown writes this file;
    // it applies on restart.
    [[nodiscard]] std::filesystem::path DefaultRendererConfigPath();
} // namespace OloEngine
