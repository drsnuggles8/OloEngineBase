// =============================================================================
// GLStateGuard.h
//
// RAII GPU state guard. Captures a snapshot of critical OpenGL state on
// construction and — on destruction — detects state that leaks out of a
// rendering pass. Intended to sit at render-pass boundaries so that a pass
// which flips a global (`glEnable(GL_BLEND)`, `glDepthMask(GL_FALSE)`,
// binding point overwrites, etc.) and fails to restore it is caught
// immediately with an actionable assertion.
//
// Usage (at a pass boundary):
//     {
//         GLStateGuard guard("ScenePass");
//         // ... draw calls ...
//     }  // dtor compares the current state against the snapshot and logs
//        // / asserts on any field that was left mutated.
//
// Default-enabled in Debug builds only; becomes a no-op in Dist to keep
// release performance identical. Detection policy is tunable via the
// Policy enum (Log / Assert / Ignore) so guards can be deployed
// incrementally: start with `Log`, flush out existing leaks, then promote
// to `Assert` for CI.
// =============================================================================

#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // Snapshot of the GL pipeline state we consider "critical" for
    // render-pass boundary validation. Captured via the DSA-free
    // `glGet*` path because DSA doesn't cover global toggles.
    struct GLStateSnapshot
    {
        // Fixed-function toggles
        bool m_DepthTest = false;
        bool m_DepthMask = true;
        i32 m_DepthFunc = 0;

        bool m_Blend = false;
        i32 m_BlendSrcRgb = 0;
        i32 m_BlendDstRgb = 0;
        i32 m_BlendSrcAlpha = 0;
        i32 m_BlendDstAlpha = 0;
        i32 m_BlendEqRgb = 0;
        i32 m_BlendEqAlpha = 0;

        bool m_StencilTest = false;
        i32 m_StencilFunc = 0;
        i32 m_StencilRef = 0;
        u32 m_StencilMask = 0;

        bool m_CullFace = false;
        i32 m_CullFaceMode = 0;
        i32 m_FrontFace = 0;

        bool m_ScissorTest = false;

        i32 m_PolygonMode = 0; // GL_FILL / GL_LINE / GL_POINT

        // Viewport / scissor
        std::array<i32, 4> m_Viewport = { 0, 0, 0, 0 };
        std::array<i32, 4> m_Scissor = { 0, 0, 0, 0 };

        // Bindings
        u32 m_FboDraw = 0;
        u32 m_FboRead = 0;
        u32 m_ActiveProgram = 0;
        u32 m_Vao = 0;
        // Enum in [GL_TEXTURE0, GL_TEXTURE31] indicating the unit a pass
        // left selected on exit. Per-unit bindings are tracked below.
        u32 m_ActiveTextureUnit = 0;

        // Per-slot bindings. OloEngine currently uses binding points up to
        // at least unit 24 (shadow / IBL / foliage / terrain splat) and UBO
        // binding 18 (post-process / fog). Size these generously at 32 —
        // the OpenGL 4.6 guaranteed minimum for GL_MAX_TEXTURE_IMAGE_UNITS
        // and GL_MAX_UNIFORM_BUFFER_BINDINGS — so no live binding escapes
        // detection. Clamp the capture loops against the driver's actual
        // limit in case it reports fewer (software renderers occasionally do).
        static constexpr u32 kTextureSlots = 32;
        static constexpr u32 kUboSlots = 32;
        std::array<u32, kTextureSlots> m_Textures2D{};
        std::array<u32, kUboSlots> m_UniformBuffers{};

        // Capture the current GL pipeline state. Caller must have a live
        // GL context. Returns a populated snapshot.
        static GLStateSnapshot Capture();

        // Compute a list of human-readable descriptions for every field in
        // which this snapshot differs from `other`. Empty vector means
        // identical state.
        std::vector<std::string> DiffAgainst(const GLStateSnapshot& other) const;
    };

    class GLStateGuard
    {
      public:
        enum class Policy
        {
            // On destruction, print each mutation to the logger. No abort.
            Log,
            // On destruction, print each mutation and abort (OLO_CORE_ASSERT).
            Assert,
            // Skip the compare entirely — used to temporarily disable a
            // known-leaking region without removing the guard.
            Ignore,
        };

        // Captures entry state immediately.
        explicit GLStateGuard(std::string_view passName, Policy policy = Policy::Log);

        // Performs the compare (unless already finalised or Policy == Ignore).
        ~GLStateGuard();

        GLStateGuard(const GLStateGuard&) = delete;
        GLStateGuard& operator=(const GLStateGuard&) = delete;
        GLStateGuard(GLStateGuard&&) = delete;
        GLStateGuard& operator=(GLStateGuard&&) = delete;

        // Force the compare to run now and return the mismatches, without
        // logging. Useful for tests. Subsequent destructor call is a no-op.
        std::vector<std::string> DetectLeaks();

        // Access the entry snapshot (for tests / diagnostics).
        const GLStateSnapshot& EntryState() const
        {
            return m_EntryState;
        }

      private:
        std::string m_PassName;
        GLStateSnapshot m_EntryState;
        Policy m_Policy;
        bool m_Finalized = false;
    };
} // namespace OloEngine
