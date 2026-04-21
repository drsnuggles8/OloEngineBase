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
        i32 m_StencilBackFunc = 0;
        i32 m_StencilBackRef = 0;
        u32 m_StencilBackValueMask = 0;
        // Persistent write masks — distinct from the read mask
        // (GL_STENCIL_VALUE_MASK) captured in m_StencilMask. Front and back
        // can diverge under glStencilMaskSeparate.
        u32 m_StencilWriteMask = 0;
        u32 m_StencilBackWriteMask = 0;
        // Per-face ops. Stencil state is separable via glStencilOpSeparate /
        // glStencilFuncSeparate, and a pass that mutates the back-face ops
        // and forgets to restore them would go undetected without these.
        i32 m_StencilFail = 0;
        i32 m_StencilPassDepthFail = 0;
        i32 m_StencilPassDepthPass = 0;
        i32 m_StencilBackFail = 0;
        i32 m_StencilBackPassDepthFail = 0;
        i32 m_StencilBackPassDepthPass = 0;

        bool m_CullFace = false;
        i32 m_CullFaceMode = 0;
        i32 m_FrontFace = 0;

        bool m_ScissorTest = false;

        // GL_POLYGON_MODE always writes two values (front + back) even in the
        // 4.6 core profile where both entries are constrained to the same
        // mode. A single GLint destination corrupts the stack; keep both.
        std::array<i32, 2> m_PolygonMode = { 0, 0 }; // [0]=front, [1]=back

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

        // Per-slot bindings. OloEngine reserves texture units through
        // TEX_SHADER_GRAPH_0 = 43, so track at least 44 slots to catch leaks
        // in the engine-reserved range. Clamp the capture loop against the
        // driver's actual limit in case it reports fewer than our local cap.
        static constexpr u32 kTextureSlots = 44;
        static constexpr u32 kUboSlots = 32;
        // Per-texture-unit bindings for every target the engine actually
        // binds. 2D covers colour/normal/roughness/AO; 2D_ARRAY is used by
        // CSM shadow maps; CUBE_MAP by IBL / environment maps. A pass
        // that forgot to unbind e.g. a cubemap would go undetected if we
        // only snapshot GL_TEXTURE_BINDING_2D.
        std::array<u32, kTextureSlots> m_Textures2D{};
        std::array<u32, kTextureSlots> m_Textures2DArray{};
        std::array<u32, kTextureSlots> m_TexturesCubeMap{};
        std::array<u32, kUboSlots> m_UniformBuffers{};

        // Per-capture driver-reported limits. Clamped against kTextureSlots /
        // kUboSlots, so they never exceed the array bounds. DiffAgainst uses
        // max(this, other) of these so diffs over slots beyond the driver's
        // reported range are skipped cleanly (software rasterisers such as
        // llvmpipe sometimes report fewer than the GL 4.6 guaranteed minimums).
        u32 m_CapturedTextureSlotLimit = kTextureSlots;
        u32 m_CapturedUboSlotLimit = kUboSlots;

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
