// =============================================================================
// StateGuardBackend.h
//
// Internal seam between the backend-neutral GLStateGuard shell
// (Renderer/Debug/GLStateGuard.cpp) and the OpenGL implementation of its
// snapshot capture/apply (Platform/OpenGL/OpenGLStateGuard.cpp) — issue #691
// ADR 0011 §1.6.
//
// Free functions rather than a virtual backend interface, on purpose: the
// guard is constructed BY VALUE on the stack at ~20 pass boundaries, so there
// is no object whose concrete type a factory could pick. No factory switch is
// needed either — the neutral shell only calls these behind its existing
// `RendererAPI::GetAPI() == API::OpenGL` gates, so on any other backend the
// calls are simply never reached (the same structural "no backend impl" path
// the early-outs already encoded). The definitions live in the OpenGL TU and
// resolve at link time, which keeps GLStateGuard.cpp free of both
// <glad/gl.h> and Platform/ includes — an include the ratchet cannot see but
// that would leak the GL API (rhi_boundary_baseline.json).
// =============================================================================

#pragma once

#include "OloEngine/Renderer/Debug/GLStateGuard.h"

namespace OloEngine::Detail
{
    // Capture the current GL pipeline state into a GLStateSnapshot. Defined in
    // Platform/OpenGL/OpenGLStateGuard.cpp. Caller must hold a live GL context
    // and must be on the OpenGL backend — the neutral shell's gates guarantee
    // both.
    [[nodiscard]] GLStateSnapshot CaptureGLState();

    // Re-issue the GL calls that bring the pipeline state back to `snapshot`'s
    // core subset (see GLStateSnapshot::ApplyCore for the contract). Defined in
    // Platform/OpenGL/OpenGLStateGuard.cpp; same preconditions as above.
    void ApplyGLStateCore(const GLStateSnapshot& snapshot);
} // namespace OloEngine::Detail
