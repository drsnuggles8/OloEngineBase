#pragma once

#include <filesystem>

namespace OloEngine::ShaderCachePaths
{
    // Root of the on-disk shader cache tree, shared by every backend (OpenGL
    // GL-tier SPIR-V/GLSL, the Vulkan SPIR-V tier, and the machine-local
    // GL program-binary / VkPipelineCache blobs — each backend appends its
    // own subdirectory).
    //
    // Overridable via the OLO_SHADER_CACHE_DIR environment variable; defaults
    // to %LOCALAPPDATA%\OloEngine\ShaderCache. Deliberately OUTSIDE any
    // worktree's source tree (issue #906): every worktree on this machine
    // then shares one warm cache instead of each paying its own cold
    // ~139-shader compile on first launch. This is only safe because the
    // cache entries are content-addressed — see OpenGLShader.cpp's per-tier
    // hashing — a mtime-keyed cache relocated here would invalidate the
    // shared store for every OTHER worktree on every checkout.
    [[nodiscard]] const std::filesystem::path& Root();
} // namespace OloEngine::ShaderCachePaths
