// =============================================================================
// DrawIndexedRawOffsetTest.cpp
//
// Regression test for `RenderCommand::DrawIndexedRaw(vao, count, baseIndex)`.
//
// Backstory — the bug this test guards against:
//   ShadowRenderPass calls DrawIndexedRaw per shadow caster. The original
//   signature took only (vao, count) and issued glDrawElements with a
//   nullptr index offset, so every caster started at index 0. For models
//   that share ONE combined index buffer across many submeshes (e.g.
//   Sponza's 22 opaque submeshes attached to a single cached VAO/IBO),
//   every caster ended up drawing indices [0, submeshIndexCount) of the
//   combined buffer — i.e. the FIRST submesh, clipped to varying lengths.
//   Visible symptom: columns/arches/walls cast no shadows while small
//   helper meshes did. Helmet (single-submesh model) worked because its
//   baseIndex was 0.
//
// What this test does:
//   - Builds one VAO with one combined IBO containing two distinct
//     triangles (left half and right half of NDC).
//   - Renders each by name via DrawIndexedRaw(vao, 3, baseIndex) — the
//     LEFT triangle at offset 0, the RIGHT triangle at offset 3.
//   - Reads back the framebuffer and asserts which side is filled.
//   - If the baseIndex parameter is silently ignored, the "right" draw
//     ends up drawing the LEFT triangle instead, and the test fails.
//
// This is a low-level test (renderer-API surface). It avoids the full
// shadow pass to keep the failure signal tight and easy to diagnose.
// =============================================================================
#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Renderer/RenderCommand.h"

#include <array>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Vertex shader that just emits a_Position in NDC clip space.
        constexpr const char* kVertSrc = R"(#version 460 core
layout(location = 0) in vec3 a_Position;
void main() { gl_Position = vec4(a_Position, 1.0); }
)";

        // Solid white fragment shader.
        constexpr const char* kFragSrc = R"(#version 460 core
layout(location = 0) out vec4 o_Color;
void main() { o_Color = vec4(1.0); }
)";

        GLuint CompileShader(GLenum type, const char* src)
        {
            GLuint shader = ::glCreateShader(type);
            ::glShaderSource(shader, 1, &src, nullptr);
            ::glCompileShader(shader);
            GLint ok = GL_FALSE;
            ::glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[1024]{};
                ::glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                ADD_FAILURE() << "Shader compile failed: " << log;
            }
            return shader;
        }

        GLuint LinkProgram()
        {
            GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertSrc);
            GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragSrc);
            GLuint program = ::glCreateProgram();
            ::glAttachShader(program, vs);
            ::glAttachShader(program, fs);
            ::glLinkProgram(program);
            ::glDeleteShader(vs);
            ::glDeleteShader(fs);
            GLint ok = GL_FALSE;
            ::glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[1024]{};
                ::glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                ADD_FAILURE() << "Program link failed: " << log;
            }
            return program;
        }

        // Average brightness inside an axis-aligned region of an RGBA8 buffer.
        f32 AverageBrightness(const std::vector<u8>& pixels, u32 width, u32 height,
                              u32 x0, u32 y0, u32 x1, u32 y1)
        {
            u64 sum = 0;
            u64 count = 0;
            for (u32 y = y0; y < y1 && y < height; ++y)
            {
                for (u32 x = x0; x < x1 && x < width; ++x)
                {
                    const auto idx = (y * width + x) * 4u;
                    sum += pixels[idx]; // R channel — equals G/B since shader writes (1,1,1,1)
                    ++count;
                }
            }
            return count == 0 ? 0.0f : static_cast<f32>(sum) / (static_cast<f32>(count) * 255.0f);
        }
    } // namespace

    TEST(DrawIndexedRawOffset, BaseIndexDrawsTheCorrectSubmesh)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        // ── Setup: one shared VBO + IBO holding two non-overlapping triangles ──
        // Both triangles are tall thin rectangles (well, isoceles triangles)
        // placed entirely within one half of NDC, so a central probe at
        // ±0.5x reliably hits exactly one of them and not the other.
        // Both reference the same vertex array, so neither draw can succeed
        // unless its baseIndex offset is respected.
        const std::array<f32, 6 * 3> vertices = {
            // Triangle A (entirely in LEFT half, centered ~x=-0.5) — vertices 0,1,2
            -0.9f, -0.9f, 0.0f,
            -0.1f, -0.9f, 0.0f,
            -0.5f,  0.9f, 0.0f,
            // Triangle B (entirely in RIGHT half, centered ~x=+0.5) — vertices 3,4,5
             0.1f, -0.9f, 0.0f,
             0.9f, -0.9f, 0.0f,
             0.5f,  0.9f, 0.0f,
        };
        const std::array<u32, 6> indices = { 0, 1, 2,  3, 4, 5 };

        // Pixel probe regions: small (8x8) windows centered on each triangle's
        // expected fill area in framebuffer space. For 64x64 viewport:
        //   left  probe — pixel (~16, ~16) — interior of triangle A
        //   right probe — pixel (~48, ~16) — interior of triangle B
        constexpr u32 kProbeHalfSize = 4;
        const auto leftProbeCenter  = std::pair<u32, u32>{ 16, 16 };
        const auto rightProbeCenter = std::pair<u32, u32>{ 48, 16 };

        GLuint vao = 0, vbo = 0, ibo = 0;
        ::glCreateVertexArrays(1, &vao);
        ::glCreateBuffers(1, &vbo);
        ::glCreateBuffers(1, &ibo);
        ::glNamedBufferData(vbo, static_cast<GLsizeiptr>(vertices.size() * sizeof(f32)), vertices.data(), GL_STATIC_DRAW);
        ::glNamedBufferData(ibo, static_cast<GLsizeiptr>(indices.size() * sizeof(u32)), indices.data(), GL_STATIC_DRAW);
        ::glVertexArrayVertexBuffer(vao, 0, vbo, 0, 3 * sizeof(f32));
        ::glVertexArrayElementBuffer(vao, ibo);
        ::glEnableVertexArrayAttrib(vao, 0);
        ::glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        ::glVertexArrayAttribBinding(vao, 0, 0);

        // Offscreen target.
        GLuint fbo = 0, colorTex = 0;
        ::glCreateFramebuffers(1, &fbo);
        ::glCreateTextures(GL_TEXTURE_2D, 1, &colorTex);
        ::glTextureStorage2D(colorTex, 1, GL_RGBA8, kWidth, kHeight);
        ::glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, colorTex, 0);
        ASSERT_EQ(::glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        GLuint program = LinkProgram();
        ::glUseProgram(program);

        // ── Test 1: DrawIndexedRaw without baseIndex draws triangle A (left) ──
        {
            ::glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            ::glViewport(0, 0, kWidth, kHeight);
            ::glDisable(GL_DEPTH_TEST);
            ::glDisable(GL_CULL_FACE);
            ::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            ::glClear(GL_COLOR_BUFFER_BIT);

            RenderCommand::DrawIndexedRaw(vao, 3);
            ::glFinish();

            std::vector<u8> pixels(kWidth * kHeight * 4u, 0);
            ::glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            const f32 leftProbe  = AverageBrightness(pixels, kWidth, kHeight,
                leftProbeCenter.first  - kProbeHalfSize, leftProbeCenter.second  - kProbeHalfSize,
                leftProbeCenter.first  + kProbeHalfSize, leftProbeCenter.second  + kProbeHalfSize);
            const f32 rightProbe = AverageBrightness(pixels, kWidth, kHeight,
                rightProbeCenter.first - kProbeHalfSize, rightProbeCenter.second - kProbeHalfSize,
                rightProbeCenter.first + kProbeHalfSize, rightProbeCenter.second + kProbeHalfSize);
            EXPECT_GT(leftProbe,  0.9f) << "Triangle A (indices 0..2) should fill the LEFT probe at pixel (16,16).";
            EXPECT_LT(rightProbe, 0.1f) << "RIGHT probe at pixel (48,16) must be empty when only indices 0..2 are drawn.";
        }

        // ── Test 2: DrawIndexedRaw with baseIndex=3 must draw triangle B (right). ──
        // This is the exact codepath the shadow pass exercises for Sponza's
        // multi-submesh combined IBO. If baseIndex is silently ignored, this
        // re-draws triangle A at the left and the assertion below catches it.
        {
            ::glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            ::glViewport(0, 0, kWidth, kHeight);
            ::glDisable(GL_DEPTH_TEST);
            ::glDisable(GL_CULL_FACE);
            ::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            ::glClear(GL_COLOR_BUFFER_BIT);

            RenderCommand::DrawIndexedRaw(vao, 3, /*baseIndex*/ 3u);
            ::glFinish();

            std::vector<u8> pixels(kWidth * kHeight * 4u, 0);
            ::glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            const f32 leftProbe  = AverageBrightness(pixels, kWidth, kHeight,
                leftProbeCenter.first  - kProbeHalfSize, leftProbeCenter.second  - kProbeHalfSize,
                leftProbeCenter.first  + kProbeHalfSize, leftProbeCenter.second  + kProbeHalfSize);
            const f32 rightProbe = AverageBrightness(pixels, kWidth, kHeight,
                rightProbeCenter.first - kProbeHalfSize, rightProbeCenter.second - kProbeHalfSize,
                rightProbeCenter.first + kProbeHalfSize, rightProbeCenter.second + kProbeHalfSize);
            // This is the exact regression: if baseIndex is silently dropped (as it was in the
            // shadow pass before the fix), the LEFT probe lights up (triangle A redrawn)
            // and the RIGHT probe stays dark.
            EXPECT_LT(leftProbe,  0.1f) << "baseIndex=3 must NOT draw triangle A. LEFT probe must remain empty.";
            EXPECT_GT(rightProbe, 0.9f) << "baseIndex=3 must draw triangle B (indices 3..5). RIGHT probe must be filled.";
        }

        // ── Cleanup ──
        ::glDeleteProgram(program);
        ::glDeleteFramebuffers(1, &fbo);
        ::glDeleteTextures(1, &colorTex);
        ::glDeleteBuffers(1, &vbo);
        ::glDeleteBuffers(1, &ibo);
        ::glDeleteVertexArrays(1, &vao);
    }
} // namespace OloEngine::Tests
