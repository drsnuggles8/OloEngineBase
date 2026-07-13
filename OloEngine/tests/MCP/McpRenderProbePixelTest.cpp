// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure decode + shaping core behind olo_render_probe_pixel
// (issue #607). The tool's whole value is that the numbers it prints are the
// numbers the GPU wrote — so the two transforms that could silently lie are the
// ones pinned here:
//
//   * OctDecodeGB must be the EXACT inverse of octEncodeGB() in
//     PBR_GBuffer.glsl. A subtly different decode would report a plausible-
//     looking but wrong normal, which is worse than reporting nothing: an agent
//     would trust it and go debug the wrong thing.
//   * LinearizeDepth must invert the standard GL perspective depth mapping, and
//     must not blow up on the degenerate inputs a probe genuinely hits (a
//     cleared depth of 1.0, a camera with no near/far).
//
// Plus the degradation contract: a channel whose target does not exist on this
// rendering path is reported as unavailable-with-a-reason, never as a failure
// and never as a zero pretending to be data.
//
// The core is header-only and GL-free precisely so this runs headlessly; the
// live tool is verified over the MCP attach loop.
#include "MCP/McpRenderProbePixel.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
    using namespace OloEngine::MCP::ProbePixel;

    // The shader's octEncodeGB(), transcribed. Encoding here and decoding with
    // the production helper is what makes this a round-trip test of the actual
    // contract rather than a restatement of the decode.
    glm::vec2 OctEncodeReference(glm::vec3 n)
    {
        n /= (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
        if (n.z < 0.0f)
        {
            const f32 x = (1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
            const f32 y = (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
            return { x, y };
        }
        return { n.x, n.y };
    }

    TexelSample MakeFloatSample(std::string target, std::string format, int channels,
                                f32 c0, f32 c1 = 0.0f, f32 c2 = 0.0f, f32 c3 = 0.0f)
    {
        TexelSample sample;
        sample.Available = true;
        sample.Target = std::move(target);
        sample.Format = std::move(format);
        sample.Kind = SampleKind::Float;
        sample.Channels = channels;
        sample.F = { c0, c1, c2, c3 };
        sample.SourceWidth = 1920;
        sample.SourceHeight = 1080;
        return sample;
    }

    TexelSample MakeMissingSample(std::string target, std::string reason)
    {
        TexelSample sample;
        sample.Target = std::move(target);
        sample.Unavailable = std::move(reason);
        return sample;
    }

    // ---- octahedral normal round-trip ------------------------------------

    TEST(McpRenderProbePixel, OctDecodeInvertsTheShaderEncodeOnEveryOctant)
    {
        const glm::vec3 directions[] = {
            { 0.0f, 1.0f, 0.0f },  // up (the classic ground normal)
            { 0.0f, -1.0f, 0.0f }, // down
            { 1.0f, 0.0f, 0.0f },  // +X
            { -1.0f, 0.0f, 0.0f }, // -X
            { 0.0f, 0.0f, 1.0f },  // toward the camera (z >= 0 branch)
            { 0.0f, 0.0f, -1.0f }, // away (the z < 0 fold — the branch a wrong decode gets wrong)
            { 0.577f, 0.577f, 0.577f },
            { -0.577f, 0.577f, -0.577f },
            { 0.267f, -0.535f, -0.802f },
        };

        for (const glm::vec3& raw : directions)
        {
            const glm::vec3 expected = glm::normalize(raw);
            const glm::vec2 encoded = OctEncodeReference(expected);
            const glm::vec3 decoded = OctDecodeGB(encoded.x, encoded.y);

            EXPECT_NEAR(decoded.x, expected.x, 1e-5f) << "x for (" << raw.x << "," << raw.y << "," << raw.z << ")";
            EXPECT_NEAR(decoded.y, expected.y, 1e-5f) << "y for (" << raw.x << "," << raw.y << "," << raw.z << ")";
            EXPECT_NEAR(decoded.z, expected.z, 1e-5f) << "z for (" << raw.x << "," << raw.y << "," << raw.z << ")";
        }
    }

    TEST(McpRenderProbePixel, OctDecodeAlwaysReturnsAUnitVector)
    {
        // Even for encoded pairs the shader would never emit, the decode must not
        // hand back a non-normalised vector an agent would then reason about.
        const glm::vec2 pairs[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.5f, 0.5f }, { -0.9f, 0.9f }, { 0.3f, -0.7f } };
        for (const glm::vec2& pair : pairs)
        {
            const glm::vec3 decoded = OctDecodeGB(pair.x, pair.y);
            EXPECT_NEAR(glm::length(decoded), 1.0f, 1e-5f);
        }
    }

    // ---- depth linearization ----------------------------------------------

    TEST(McpRenderProbePixel, LinearizeDepthMapsTheClipPlanes)
    {
        constexpr f32 kNear = 0.1f;
        constexpr f32 kFar = 1000.0f;

        // Device 0 is the near plane, device 1 the far plane (a cleared/background
        // pixel) — the two values a probe hits constantly.
        EXPECT_NEAR(LinearizeDepth(0.0f, kNear, kFar), kNear, 1e-3f);

        // The far plane only round-trips to ~0.03% in f32: at device depth 1 the
        // denominator collapses to 2*near (0.2 here), so the division amplifies the
        // last mantissa bits. That is a property of the depth encoding, not of this
        // decode — and it is exactly why the tool reports the RAW device depth
        // alongside the linearized value instead of only the pretty number.
        EXPECT_NEAR(LinearizeDepth(1.0f, kNear, kFar), kFar, kFar * 1e-3f);
    }

    TEST(McpRenderProbePixel, LinearizeDepthRoundTripsAKnownViewDistance)
    {
        constexpr f32 kNear = 0.1f;
        constexpr f32 kFar = 100.0f;
        constexpr f32 kViewDepth = 7.5f;

        // Forward transform: view depth -> NDC z -> window depth, exactly what the
        // fixed-function pipeline stores.
        const f32 ndc = ((kFar + kNear) / (kFar - kNear)) -
                        (2.0f * kFar * kNear) / (kViewDepth * (kFar - kNear));
        const f32 device = ndc * 0.5f + 0.5f;

        EXPECT_NEAR(LinearizeDepth(device, kNear, kFar), kViewDepth, 1e-2f);
    }

    TEST(McpRenderProbePixel, LinearizeDepthIsSafeOnDegenerateClipPlanes)
    {
        // A camera the tool could not read (near/far both zero) must not produce a
        // NaN/inf that then gets serialized into JSON as `null` or garbage.
        EXPECT_FLOAT_EQ(LinearizeDepth(0.5f, 0.0f, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(LinearizeDepth(0.5f, 10.0f, 1.0f), 0.0f); // far < near
        EXPECT_TRUE(std::isfinite(LinearizeDepth(1.0f, 0.1f, 1000.0f)));
    }

    // ---- G-Buffer shaping --------------------------------------------------

    TEST(McpRenderProbePixel, DeferredProbeDecodesEveryChannel)
    {
        const glm::vec3 normal = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec2 encoded = OctEncodeReference(normal);

        GBufferProbeInput in;
        in.X = 640;
        in.Y = 360;
        in.RenderingPath = "Deferred";
        in.CameraKnown = true;
        in.NearClip = 0.1f;
        in.FarClip = 1000.0f;
        in.Albedo = MakeFloatSample("GBufferAlbedo", "RGBA8", 4, 0.8f, 0.2f, 0.1f, 1.0f);
        in.Normal = MakeFloatSample("GBufferNormal", "RGBA16F", 4, encoded.x, encoded.y, 0.35f, 0.9f);
        in.Emissive = MakeFloatSample("GBufferEmissive", "RGBA16F", 4, 2.0f, 0.0f, 0.0f, 0.0f);
        in.Velocity = MakeFloatSample("Velocity", "RG16F", 2, 0.01f, -0.02f);
        in.Depth = MakeFloatSample("SceneDepth", "DEPTH32F", 1, 0.5f);
        in.FinalColor = MakeFloatSample("ToneMapColorTexture", "RGBA8", 4, 0.4f, 0.1f, 0.05f, 1.0f);

        TexelSample entityId;
        entityId.Available = true;
        entityId.Target = "SceneEntityID";
        entityId.Format = "R32I";
        entityId.Kind = SampleKind::Int;
        entityId.Channels = 1;
        entityId.I = { 42, 0, 0, 0 };
        in.EntityId = entityId;

        const Json j = BuildGBufferProbe(in);
        const Json& channels = j.at("channels");

        EXPECT_EQ(j.at("x").get<u32>(), 640u);
        EXPECT_EQ(j.at("y").get<u32>(), 360u);

        EXPECT_TRUE(channels.at("albedo").at("available").get<bool>());
        EXPECT_NEAR(channels.at("albedo").at("value")[0].get<f32>(), 0.8f, 1e-6f);
        EXPECT_NEAR(channels.at("metallic").at("value").get<f32>(), 1.0f, 1e-6f);

        // The normal is DECODED, not echoed: this is the assertion that the tool
        // reports a world normal rather than the raw octahedral pair.
        const Json& normalJson = channels.at("normal");
        EXPECT_NEAR(normalJson.at("value")[0].get<f32>(), 0.0f, 1e-5f);
        EXPECT_NEAR(normalJson.at("value")[1].get<f32>(), 1.0f, 1e-5f);
        EXPECT_NEAR(normalJson.at("value")[2].get<f32>(), 0.0f, 1e-5f);
        EXPECT_NEAR(normalJson.at("encoded")[0].get<f32>(), encoded.x, 1e-6f);

        EXPECT_NEAR(channels.at("roughness").at("value").get<f32>(), 0.35f, 1e-6f);
        EXPECT_NEAR(channels.at("ao").at("value").get<f32>(), 0.9f, 1e-6f);
        EXPECT_NEAR(channels.at("emissive").at("value")[0].get<f32>(), 2.0f, 1e-6f);
        EXPECT_NEAR(channels.at("velocity").at("value")[1].get<f32>(), -0.02f, 1e-6f);

        // Integer, not float — an entity id compared against a scene UUID must be exact.
        EXPECT_EQ(channels.at("entityID").at("value").get<i32>(), 42);

        const Json& depth = channels.at("depth");
        EXPECT_NEAR(depth.at("device").get<f32>(), 0.5f, 1e-6f);
        EXPECT_TRUE(depth.at("linearViewDepth").is_number());
        EXPECT_GT(depth.at("linearViewDepth").get<f32>(), 0.1f);

        EXPECT_TRUE(j.at("unavailableChannels").empty());
        EXPECT_FALSE(j.contains("note"));
    }

    TEST(McpRenderProbePixel, ForwardPathDegradesPerChannelInsteadOfFailing)
    {
        // The G-Buffer does not exist outside Deferred. That must be a USEFUL
        // answer (which channels are missing, why, and how to fix it), not an
        // error and not zeros masquerading as data.
        GBufferProbeInput in;
        in.X = 10;
        in.Y = 20;
        in.RenderingPath = "Forward+";
        in.Albedo = MakeMissingSample("GBufferAlbedo", "no GPU backing this frame");
        in.Normal = MakeMissingSample("GBufferNormal", "no GPU backing this frame");
        in.Emissive = MakeMissingSample("GBufferEmissive", "no GPU backing this frame");
        in.Velocity = MakeMissingSample("Velocity", "no GPU backing this frame");
        in.EntityId = MakeMissingSample("SceneEntityID", "no GPU backing this frame");
        in.Depth = MakeFloatSample("SceneDepth", "DEPTH32F", 1, 0.9f); // forward still has depth
        in.FinalColor = MakeFloatSample("SceneColorTexture", "RGBA16F", 4, 0.2f, 0.2f, 0.2f, 1.0f);

        const Json j = BuildGBufferProbe(in);
        const Json& channels = j.at("channels");

        EXPECT_FALSE(channels.at("albedo").at("available").get<bool>());
        EXPECT_EQ(channels.at("albedo").at("reason").get<std::string>(), "no GPU backing this frame");
        EXPECT_FALSE(channels.at("normal").at("available").get<bool>());
        EXPECT_FALSE(channels.at("entityID").at("available").get<bool>());

        // Depth and the final colour survive on every path.
        EXPECT_TRUE(channels.at("depth").at("available").get<bool>());
        EXPECT_TRUE(channels.at("finalColor").at("available").get<bool>());

        const auto missing = j.at("unavailableChannels").get<std::vector<std::string>>();
        EXPECT_NE(std::find(missing.begin(), missing.end(), "albedo"), missing.end());
        EXPECT_NE(std::find(missing.begin(), missing.end(), "roughness"), missing.end());
        EXPECT_EQ(std::find(missing.begin(), missing.end(), "depth"), missing.end());

        // The note must name the actual remedy, not just state the problem.
        ASSERT_TRUE(j.contains("note"));
        EXPECT_NE(j.at("note").get<std::string>().find("Deferred"), std::string::npos);
    }

    TEST(McpRenderProbePixel, DepthWithoutACameraReportsNullRatherThanAWrongNumber)
    {
        GBufferProbeInput in;
        in.RenderingPath = "Deferred";
        in.CameraKnown = false;
        in.Depth = MakeFloatSample("SceneDepth", "DEPTH32F", 1, 0.75f);

        const Json depth = BuildGBufferProbe(in).at("channels").at("depth");
        EXPECT_NEAR(depth.at("device").get<f32>(), 0.75f, 1e-6f);
        EXPECT_TRUE(depth.at("linearViewDepth").is_null());
        EXPECT_TRUE(depth.contains("note"));
    }

    TEST(McpRenderProbePixel, RawProbeCarriesIntegerChannelsAsIntegers)
    {
        TexelSample sample;
        sample.Available = true;
        sample.Target = "VirtualGeometryDebug";
        sample.Format = "R32UI";
        sample.Kind = SampleKind::Int;
        sample.Channels = 1;
        sample.I = { 1337, 0, 0, 0 };
        sample.SourceWidth = 800;
        sample.SourceHeight = 600;

        const Json j = BuildRawProbe(sample, 12, 34);
        EXPECT_TRUE(j.at("available").get<bool>());
        EXPECT_EQ(j.at("target").get<std::string>(), "VirtualGeometryDebug");
        EXPECT_EQ(j.at("value")[0].get<i32>(), 1337);
        EXPECT_EQ(j.at("x").get<u32>(), 12u);
        EXPECT_EQ(j.at("y").get<u32>(), 34u);
    }

    TEST(McpRenderProbePixel, RawProbeOfAMissingTargetCarriesTheReason)
    {
        const Json j = BuildRawProbe(MakeMissingSample("AOBuffer", "AO is disabled"), 1, 2);
        EXPECT_FALSE(j.at("available").get<bool>());
        EXPECT_EQ(j.at("reason").get<std::string>(), "AO is disabled");
    }
} // namespace
