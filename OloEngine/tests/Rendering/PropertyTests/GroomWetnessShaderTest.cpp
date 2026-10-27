// OLO_TEST_LAYER: L2
// =============================================================================
// GroomWetnessShaderTest.cpp — include/GroomWetness.glsl against its C++ twins
// (#1615): the wetness at a height, the fibre's wet scales and a wet strand's
// laid-down point, over a 64 x 64 grid of inputs the probe and this test spell
// identically from the pixel's coordinates.
// =============================================================================
#include "OloEnginePCH.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Groom/GroomWetness.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <glad/gl.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    TEST(GroomWetnessShader, TheShaderWetsAsTheCppTwinsDo)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard guard("GroomWetnessProbe", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = spec.Height = 64;
        spec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA32F };
        auto target = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/tests/GroomWetnessProbe.glsl");
        ASSERT_TRUE(target && shader);
        target->Bind();
        constexpr std::array<GLenum, 2> kBuffers{ GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, kBuffers.data());
        glViewport(0, 0, 64, 64);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        shader->Bind();
        FullscreenPass pass;
        pass.Draw(0);
        glFinish();
        target->Unbind();
        std::vector<f32> scalars;
        std::vector<f32> points;
        ReadbackRgbaFloat(target->GetColorAttachmentHandle(0), 64, 64, scalars);
        ReadbackRgbaFloat(target->GetColorAttachmentHandle(1), 64, 64, points);
        ASSERT_EQ(scalars.size(), 64u * 64u * 4u);
        ASSERT_EQ(points.size(), 64u * 64u * 4u);

        GroomWetResponse optical;
        optical.Absorption = 2.2f;
        optical.Roughness = 0.5f;
        optical.Azimuthal = 0.4f;
        GroomWetResponse geometric;
        geometric.Flatten = 0.6f;
        geometric.Shorten = 0.15f;
        geometric.Clump = 0.5f;
        u32 wetPoints = 0;
        for (u32 py = 0; py < 64u; ++py)
        {
            for (u32 px = 0; px < 64u; ++px)
            {
                SCOPED_TRACE("pixel " + std::to_string(px) + ", " + std::to_string(py));
                const sizet i = (static_cast<sizet>(py) * 64u + px) * 4u;
                const f32 x = static_cast<f32>(px);
                const f32 y = static_cast<f32>(py);
                GroomWetnessFrame frame;
                frame.Rain = py < 32u ? 0.0f : 0.3f;
                frame.Line = 0.05f;
                frame.Band = 0.04f;
                const f32 wetness = std::fmod(x * 7.0f + y * 3.0f, 64.0f) / 63.0f;
                const GroomWetFibreScales scales = GroomWetFibre(wetness, optical);
                EXPECT_NEAR(scalars[i + 0], GroomWetnessAt((x - 32.0f) * 0.004f, frame), 1.0e-5f);
                EXPECT_NEAR(scalars[i + 1], scales.SigmaA, 1.0e-5f);
                EXPECT_NEAR(scalars[i + 2], scales.V0, 1.0e-5f);
                EXPECT_NEAR(scalars[i + 3], scales.S, 1.0e-5f);

                const f32 t = y / 63.0f;
                const glm::vec3 local((x - 32.0f) * 0.0005f, (y - 32.0f) * 0.0003f, 0.005f + 0.0004f * y);
                const glm::vec3 gravity = glm::normalize(glm::vec3(std::fmod(x, 7.0f) - 3.0f + 0.25f, std::fmod(y, 5.0f) - 2.0f,
                                                                   std::fmod(x + y, 9.0f) - 4.0f + 0.5f));
                const glm::vec3 clump = std::fmod(x, 3.0f) < 0.5f ? glm::vec3(0.0f) : glm::vec3(0.004f, -0.002f, 0.03f);
                const glm::vec3 expected = GroomWetLocal(local, t, wetness, gravity, clump, geometric);
                const glm::vec3 shaded(points[i + 0], points[i + 1], points[i + 2]);
                // Relative to the strand's reach: a point a few centimetres out,
                // to a few parts in a million.
                const f32 tolerance = 2.0e-5f * std::max(glm::length(local), 1.0e-3f);
                EXPECT_LE(glm::length(shaded - expected), tolerance)
                    << "shader (" << shaded.x << ", " << shaded.y << ", " << shaded.z << ") vs C++ (" << expected.x
                    << ", " << expected.y << ", " << expected.z << ")";
                wetPoints += glm::length(expected - local) > 1.0e-4f ? 1u : 0u;
            }
        }
        // NEGATIVE CONTROL: the grid really wets its points, so agreement is
        // about laid-down strands and not two copies of the input.
        EXPECT_GT(wetPoints, 64u * 64u / 2u);
    }
} // namespace OloEngine::Tests
