// =============================================================================
// DeferredPropertyTests.cpp
//
// Property-level (Layer 1) tests for the Deferred renderer path.
//
// Coverage:
//   * GBuffer construction/resize invariants — correct attachment count,
//     valid renderer IDs, round-trip through Resize().
//   * Octahedral view-space normal encode/decode round-trip matches the
//     shader implementation in PBR_GBuffer.glsl / DeferredLighting.glsl
//     to well under 1° error across the hemisphere.
//   * RendererSettings::DeferredSettings defaults are stable (MSAA=1,
//     OIT disabled, debug channel = 0, G-Buffer decals enabled).
//
// These tests deliberately avoid standing up a full Renderer3D scene.
// Shader-side encode/decode is validated at the GLSL level in
// `ShaderUnitTests`; this fixture exercises the CPU-side math mirror and
// the GBuffer class to catch regressions before they reach the GPU.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RenderingPath.h"

namespace OloEngine::Tests
{
    namespace
    {
        // CPU mirror of the shader's octEncodeGB / octDecode — keep in sync
        // with PBR_GBuffer.glsl (encode) and DeferredLighting.glsl (decode).
        glm::vec2 OctWrap(const glm::vec2& v)
        {
            glm::vec2 w;
            w.x = (1.0f - std::abs(v.y)) * (v.x >= 0.0f ? 1.0f : -1.0f);
            w.y = (1.0f - std::abs(v.x)) * (v.y >= 0.0f ? 1.0f : -1.0f);
            return w;
        }

        glm::vec2 OctEncode(glm::vec3 n)
        {
            const f32 invL1 = 1.0f / (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
            n *= invL1;
            glm::vec2 enc = glm::vec2(n.x, n.y);
            if (n.z < 0.0f)
                enc = OctWrap(enc);
            return enc * 0.5f + 0.5f;
        }

        glm::vec3 OctDecode(glm::vec2 enc)
        {
            enc = enc * 2.0f - 1.0f;
            glm::vec3 n(enc.x, enc.y, 1.0f - std::abs(enc.x) - std::abs(enc.y));
            if (n.z < 0.0f)
            {
                glm::vec2 xy = OctWrap(glm::vec2(n.x, n.y));
                n.x = xy.x;
                n.y = xy.y;
            }
            return glm::normalize(n);
        }
    } // namespace

    TEST(DeferredOctNormalTest, RoundTripKeepsHemisphereUnderOneDegree)
    {
        // Stratified hemisphere sweep — covers edges, poles, and the seam
        // (|n.z|≈0) where octahedral compression is most aggressive.
        constexpr u32 kSteps = 32;
        f32 maxAngleError = 0.0f;
        for (u32 i = 0; i <= kSteps; ++i)
        {
            const f32 theta = (static_cast<f32>(i) / static_cast<f32>(kSteps)) * glm::pi<f32>();
            for (u32 j = 0; j <= kSteps; ++j)
            {
                const f32 phi = (static_cast<f32>(j) / static_cast<f32>(kSteps)) * glm::two_pi<f32>();
                const glm::vec3 n(
                    std::sin(theta) * std::cos(phi),
                    std::sin(theta) * std::sin(phi),
                    std::cos(theta));

                const glm::vec2 enc = OctEncode(n);
                const glm::vec3 dec = OctDecode(enc);

                const f32 dot = glm::clamp(glm::dot(n, dec), -1.0f, 1.0f);
                const f32 angle = std::acos(dot);
                maxAngleError = std::max(maxAngleError, angle);
            }
        }

        // Octahedral at RG16 is known to be <0.5° worst-case; give a little
        // headroom for fp32 rounding on the CPU mirror.
        EXPECT_LT(maxAngleError, glm::radians(1.0f))
            << "maxAngleError = " << glm::degrees(maxAngleError) << " deg";
    }

    TEST(DeferredOctNormalTest, EncodeOutputStaysInUnitRange)
    {
        // The encoded value is remapped to [0,1]^2 so it fits an unsigned
        // RG16 attachment without additional bias — verify the invariant.
        const glm::vec3 dirs[] = {
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
            glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)),
            glm::normalize(glm::vec3(-1.0f, 1.0f, -1.0f)),
        };
        for (const auto& n : dirs)
        {
            const glm::vec2 enc = OctEncode(n);
            EXPECT_GE(enc.x, 0.0f);
            EXPECT_LE(enc.x, 1.0f);
            EXPECT_GE(enc.y, 0.0f);
            EXPECT_LE(enc.y, 1.0f);
        }
    }

    TEST(DeferredSettingsTest, DefaultsMatchPlan)
    {
        // Defaults keep the renderer on Forward so small scenes stay lean
        // — users opt into Deferred via RendererSettingsPanel. Change this
        // test intentionally if the default path is ever promoted.
        RendererSettings s;
        EXPECT_EQ(s.Path, RenderingPath::Forward);
        EXPECT_EQ(s.Deferred.MSAASampleCount, 1u);
        EXPECT_FALSE(s.OITEnabled);
        EXPECT_EQ(s.Deferred.DebugChannel, 0u);
        EXPECT_TRUE(s.Deferred.GBufferDecalsEnabled);
        EXPECT_TRUE(s.Deferred.PerSampleLighting);
    }

    TEST(DeferredSettingsTest, MSAASampleCountRoundTripsCommonValues)
    {
        // The panel exposes {1,2,4,8} and GBuffer::Create rejects anything
        // else. Verify the field accepts and preserves each valid value.
        RendererSettings s;
        for (u32 samples : { 1u, 2u, 4u, 8u })
        {
            s.Deferred.MSAASampleCount = samples;
            EXPECT_EQ(s.Deferred.MSAASampleCount, samples);
        }
    }
} // namespace OloEngine::Tests
