// OLO_TEST_LAYER: L2
// =============================================================================
// GroomAmbientShaderTest.cpp — include/GroomAmbient.glsl against the body's
// probe sampler (#1618).
//
// The groom samples the baked probe grid in one pass for both of its
// directions, without the coefficient arrays sampleLightProbeGrid blends (the
// arrays cost the strand shader a third of its occupancy). This holds the
// one-pass pair to sampleLightProbeGrid along n and along -n, over positions in
// and around a probe volume with an invalid probe, and the rung selection to
// the ladder's: the probe's E/pi where the volume answers, the flat fill where
// it does not (IBL off).
// =============================================================================
#include "OloEnginePCH.h"
#include "RenderPropertyTest.h"

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
    namespace
    {
        // A buffer bound at an indexed binding for the test's span, and the
        // binding it displaced put back after.
        struct ScopedIndexedBuffer
        {
            GLenum Target = 0;
            GLuint Index = 0;
            GLint Previous = 0;
            GLuint Buffer = 0;

            ScopedIndexedBuffer(GLenum target, GLenum query, GLuint index, const void* data, GLsizeiptr bytes)
                : Target(target), Index(index)
            {
                glGetIntegeri_v(query, index, &Previous);
                glCreateBuffers(1, &Buffer);
                glNamedBufferData(Buffer, bytes, data, GL_STATIC_DRAW);
                glBindBufferBase(target, index, Buffer);
            }
            ~ScopedIndexedBuffer()
            {
                glBindBufferBase(Target, Index, static_cast<GLuint>(Previous));
                glDeleteBuffers(1, &Buffer);
            }
        };
    } // namespace

    TEST(GroomAmbientShader, TheOnePassProbePairIsTheBodysProbeSamplerInBothDirections)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard guard("GroomAmbientProbe", GLStateGuard::Policy::Restore);

        // LightProbeVolume (UBO 22): bounds (0,0,0)-(4,2,1), 3 x 2 x 2 probes.
        struct ProbeVolume
        {
            f32 Min[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
            f32 Max[4]{ 4.0f, 2.0f, 1.0f, 0.0f };
            i32 Dims[4]{ 3, 2, 2, 12 };
            f32 Spacing[4]{ 2.0f, 2.0f, 1.0f, 0.0f };
            i32 Enabled = 1;
            f32 Intensity = 1.3f;
            i32 Pad0 = 0;
            i32 Pad1 = 0;
        } volume;
        static_assert(sizeof(ProbeVolume) == 80);
        // Nine coefficients a probe, varied per probe, band and channel; probe
        // 5 invalid, so the renormalised blend is exercised.
        std::vector<glm::vec4> coefficients(12u * 9u);
        for (u32 p = 0; p < 12u; ++p)
        {
            for (u32 c = 0; c < 9u; ++c)
            {
                const f32 s = static_cast<f32>(p * 9u + c);
                glm::vec3 value(0.5f + 0.4f * std::sin(s * 1.7f), 0.4f + 0.3f * std::cos(s * 0.9f), 0.3f + 0.2f * std::sin(s * 2.3f));
                if (c > 0u)
                    value *= 0.35f; // a plausible L1/L2 falloff, with signs from the trig
                coefficients[p * 9u + c] = glm::vec4(value, c == 0u ? (p == 5u ? 0.0f : 1.0f) : 0.0f);
            }
        }
        const std::array<u8, 512> ddgiDisabled{}; // DDGIVolume (UBO 51): u_DDGIEnabled = 0
        ScopedIndexedBuffer probeUbo(GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER_BINDING, 22u, &volume, sizeof(volume));
        ScopedIndexedBuffer ddgiUbo(GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER_BINDING, 51u, ddgiDisabled.data(), 512);
        ScopedIndexedBuffer probeSsbo(GL_SHADER_STORAGE_BUFFER, GL_SHADER_STORAGE_BUFFER_BINDING, 8u, coefficients.data(),
                                      static_cast<GLsizeiptr>(coefficients.size() * sizeof(glm::vec4)));

        FramebufferSpecification spec;
        spec.Width = spec.Height = 64;
        spec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA32F,
                             FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA32F,
                             FramebufferTextureFormat::RGBA32F };
        auto target = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/tests/GroomAmbientProbe.glsl");
        ASSERT_TRUE(target && shader);
        target->Bind();
        constexpr std::array<GLenum, 5> kBuffers{ GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                                                  GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4 };
        glDrawBuffers(5, kBuffers.data());
        glViewport(0, 0, 64, 64);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        shader->Bind();
        FullscreenPass pass;
        pass.Draw(0);
        glFinish();
        target->Unbind();
        std::array<std::vector<f32>, 5> out;
        for (u32 i = 0; i < 5u; ++i)
        {
            ReadbackRgbaFloat(target->GetColorAttachmentRendererID(i), 64, 64, out[i]);
            ASSERT_EQ(out[i].size(), 64u * 64u * 4u);
        }

        u32 inside = 0;
        u32 asymmetric = 0;
        u32 outside = 0;
        constexpr f32 kInvPi = 0.318309886f;
        for (sizet i = 0; i < 64u * 64u * 4u; i += 4u)
        {
            const glm::vec3 leanFront(out[0][i], out[0][i + 1], out[0][i + 2]);
            const glm::vec3 leanBack(out[1][i], out[1][i + 1], out[1][i + 2]);
            const glm::vec3 refFront(out[2][i], out[2][i + 1], out[2][i + 2]);
            const glm::vec3 refBack(out[3][i], out[3][i + 1], out[3][i + 2]);
            const glm::vec3 pair(out[4][i], out[4][i + 1], out[4][i + 2]);
            const f32 scale = std::max(1.0f, glm::length(refFront) + glm::length(refBack));
            EXPECT_LE(glm::length(leanFront - refFront), 1.0e-5f * scale) << "texel " << i / 4u;
            EXPECT_LE(glm::length(leanBack - refBack), 1.0e-5f * scale) << "texel " << i / 4u;
            const bool probed = glm::dot(refFront, refFront) > 0.0f;
            const glm::vec3 expected = probed ? refFront * kInvPi : glm::vec3(0.03f);
            EXPECT_LE(glm::length(pair - expected), 1.0e-5f * scale) << "the rung, texel " << i / 4u;
            inside += probed ? 1u : 0u;
            outside += probed ? 0u : 1u;
            asymmetric += glm::length(refFront - refBack) > 0.05f ? 1u : 0u;
        }
        // NEGATIVE CONTROLS: the grid crosses the volume, and n and -n see
        // different light -- so the agreement covers both rungs and both signs.
        EXPECT_GT(inside, 1500u);
        EXPECT_GT(outside, 300u);
        EXPECT_GT(asymmetric, 1000u);
    }
} // namespace OloEngine::Tests
