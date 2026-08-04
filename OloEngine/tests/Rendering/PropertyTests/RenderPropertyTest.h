// =============================================================================
// RenderPropertyTest.h
//
// Shared fixture for property-based renderer tests. Provides:
//   - A hidden-window OpenGL 4.6 context (shared across the test process)
//   - Automatic SKIP when no GL context is available (CI without GPU)
//   - CWD management (shaders resolve relative to OloEditor/)
//   - Readback + procedural input helpers for common patterns
//
// Design notes
// ------------
// The context is created once (lazy) and kept for the lifetime of the test
// process. Per-test setup is cheap; only framebuffers, UBOs and shaders are
// created per test. We deliberately do NOT call Renderer::Init() — the PBR
// catalog requires a real window/Application and will need its own fixture.
// This header is sufficient for any test that only compiles a single shader
// and runs a fullscreen triangle (post-process catalog).
//
// Usage
// -----
//     TEST(MyPostProcessTest, SomeInvariant)
//     {
//         RenderPropertyFixture::EnsureGpuOrSkip();
//         ...
//     }
// =============================================================================
#pragma once

#include "OloEnginePCH.h"

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// Forward declares (avoid dragging GLFW into every test TU)
struct GLFWwindow;

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // RenderPropertyFixture
    //
    // Static utility type with a singleton GPU-context. Tests call
    // EnsureGpuOrSkip() as their first line and use the helper free-functions
    // below for readback / procedural input generation.
    // -------------------------------------------------------------------------
    class RenderPropertyFixture
    {
      public:
        // Returns true if a usable GL 4.5+ context was successfully created in
        // the current process. Initializes the context on first call.
        static bool IsGpuAvailable();

        // Convenience: calls GTEST_SKIP() with a helpful message if no GPU.
        // Must be called from within a TEST/TEST_F body.
#define OLO_ENSURE_GPU_OR_SKIP()                                                       \
    do                                                                                 \
    {                                                                                  \
        if (!::OloEngine::Tests::RenderPropertyFixture::IsGpuAvailable())              \
        {                                                                              \
            GTEST_SKIP() << "No GPU / GL 4.5+ context available in this environment."; \
        }                                                                              \
    } while (false)

      private:
        RenderPropertyFixture() = default;
    };

    // -------------------------------------------------------------------------
    // Procedural input helpers
    // -------------------------------------------------------------------------

    // Creates an RGBA32F texture from a host-side float buffer. Returns the
    // raw GL texture name; the caller owns it (call glDeleteTextures).
    // `pixels` must contain width * height * 4 floats.
    u32 CreateFloatTexture2D(u32 width, u32 height, const f32* pixels);

    // Creates an RGBA32F texture filled with a uniform RGBA color.
    u32 CreateUniformFloatTexture2D(u32 width, u32 height, f32 r, f32 g, f32 b, f32 a);

    // Creates a single-pixel RGBA32F texture with the given color.
    u32 CreateSinglePixelFloatTexture(f32 r, f32 g, f32 b, f32 a);

    // Creates an RGBA8 texture from a host-side byte buffer. Returns the
    // raw GL texture name; the caller owns it (call glDeleteTextures).
    // `pixels` must contain width * height * 4 bytes. Internally guards
    // GL_UNPACK_* state so prior tests can't corrupt the upload.
    u32 CreateRgba8Texture2D(u32 width, u32 height, const u8* pixels);

    // -------------------------------------------------------------------------
    // Readback helpers
    // -------------------------------------------------------------------------

    // Reads the full contents of an RGBA8 texture into the caller's buffer.
    // Sized to width * height * 4 bytes.
    void ReadbackRgba8(u32 textureId, u32 width, u32 height, std::vector<u8>& out);

    // Reads the full contents of an RGBA16F or RGBA32F texture as floats.
    // Sized to width * height * 4 floats (GL converts 16F → 32F internally).
    void ReadbackRgbaFloat(u32 textureId, u32 width, u32 height, std::vector<f32>& out);

    // Basic statistics for validation. Tuples are per-channel.
    struct FloatStats
    {
        f32 m_MinR = 0.0f, m_MinG = 0.0f, m_MinB = 0.0f, m_MinA = 0.0f;
        f32 m_MaxR = 0.0f, m_MaxG = 0.0f, m_MaxB = 0.0f, m_MaxA = 0.0f;
        f64 m_SumR = 0.0, m_SumG = 0.0, m_SumB = 0.0, m_SumA = 0.0;
        u32 m_NanCount = 0;
        u32 m_InfCount = 0;
        u32 m_PixelCount = 0;
    };
    FloatStats ComputeStats(const std::vector<f32>& pixels);

    // -------------------------------------------------------------------------
    // Fullscreen-triangle / quad helper for single-shader post-process tests.
    //
    // Binds `inputTexture` at GL texture unit 0, binds the internal
    // fullscreen quad VAO, and issues the draw. Callers are responsible for
    // binding the output framebuffer, setting viewport / fixed-function state,
    // and binding the shader + any required UBOs before calling this.
    // -------------------------------------------------------------------------
    class FullscreenPass
    {
      public:
        FullscreenPass();
        ~FullscreenPass();

        // Disable copying (owns raw GL resources).
        FullscreenPass(const FullscreenPass&) = delete;
        FullscreenPass& operator=(const FullscreenPass&) = delete;

        void Draw(u32 inputTexture) const;

      private:
        u32 m_Vao = 0;
        u32 m_Vbo = 0;
    };

    // -------------------------------------------------------------------------
    // Bind an AUXILIARY input (depth, velocity, ...) at `slot` for whichever
    // shader variant is in flight.
    //
    // `FullscreenPass::Draw` handles slot 0 only. A converted post-process shader
    // reads every OTHER input through the offset table too, so a bare
    // `glBindTextureUnit(TEX_POSTPROCESS_DEPTH, depth)` is invisible to it and the
    // shader samples the reserved null. That is how
    // `MotionBlurVelocityTest.VelocityDirectionDrivesBlur` failed — no velocity
    // reached the shader, so nothing smeared — and, worse, how
    // `MotionBlurStaticTest.ZeroVelocityIsIdentity` PASSED for the wrong reason:
    // a missing velocity texture reads as zero velocity, which is exactly what it
    // asserts. A test that cannot fail is not evidence.
    //
    // NOT scoped, despite living next to the scope guard below: this binds and
    // returns, exactly like the `glBindTextureUnit` it replaces. It is a named
    // function rather than a bare GL call so that the requirement — the shader in
    // flight must be slot-based — is stated at the call site instead of being an
    // unwritten assumption that breaks silently if `ScopedSlotBasedShaders` is
    // ever dropped from the harness.
    // -------------------------------------------------------------------------
    void BindSlotBasedInput(u32 slot, u32 texture);

    // -------------------------------------------------------------------------
    // Compile shaders created inside this scope as the SLOT-BASED variant.
    //
    // WHY THESE HARNESSES OPT OUT OF BINDLESS RATHER THAN PARTICIPATING IN IT.
    // They are property tests of shader MATH — FXAA edge preservation, motion
    // blur direction, DOF focus. They drive a shader file directly with their own
    // GL binds, outside the render graph, with no frame loop.
    //
    // Making them bind through the heap instead was tried and is a bad trade. The
    // heap's lifetimes assume a frame: `FrameTransient` slots are only reclaimed
    // at a frame boundary these tests never reach (so the ring fills for the whole
    // process, and retired slots are released against textures the test already
    // deleted, raising GL errors inside whichever LATER test triggers the reset),
    // while retiring eagerly puts descriptor teardown next to caller-owned
    // textures whose deletion order the harness does not control. Every variant of
    // that produced a different order-dependent failure.
    //
    // The bindless path does not lose coverage here. `BindlessHeapGpuTest` proves
    // the seam end-to-end with real texel readback, and the visual-evidence suites
    // exercise these very shaders through the real engine passes with the heap on.
    // What this scope buys is that the MATH tests measure math, under both
    // configurations, instead of measuring binding plumbing they do not use.
    // -------------------------------------------------------------------------
    class ScopedSlotBasedShaders
    {
      public:
        ScopedSlotBasedShaders();
        ~ScopedSlotBasedShaders();

        ScopedSlotBasedShaders(const ScopedSlotBasedShaders&) = delete;
        ScopedSlotBasedShaders& operator=(const ScopedSlotBasedShaders&) = delete;

      private:
        bool m_WasEnabled = false;
    };
} // namespace OloEngine::Tests
