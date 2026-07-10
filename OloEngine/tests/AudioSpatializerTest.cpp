#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Audio/DSP/Spatializer/VBAP.h"
#include "OloEngine/Audio/AudioTransform.h"
#include "OloEngine/Audio/SampleBufferOperations.h"

#include <glm/glm.hpp>
#include <miniaudio.h>

#include <cmath>
#include <numeric>
#include <vector>

using namespace OloEngine::Audio::DSP;
using namespace OloEngine::Audio;

//==============================================================================
/// RealtimeGains Tests
//==============================================================================

TEST(VBAP_RealtimeGains, DefaultIsZero)
{
    RealtimeGains gains;
    const auto& g = gains.Read();
    for (sizet i = 0; i < g.size(); ++i)
    {
        EXPECT_FLOAT_EQ(g[i], 0.0f);
    }
}

TEST(VBAP_RealtimeGains, WriteAndRead)
{
    RealtimeGains gains;
    ChannelGains testGains{};
    testGains[0] = 0.5f;
    testGains[1] = 0.75f;
    testGains[2] = 1.0f;

    gains.Write(testGains);
    const auto& result = gains.Read();

    EXPECT_FLOAT_EQ(result[0], 0.5f);
    EXPECT_FLOAT_EQ(result[1], 0.75f);
    EXPECT_FLOAT_EQ(result[2], 1.0f);
}

TEST(VBAP_RealtimeGains, DoubleWriteReturnsLatest)
{
    RealtimeGains gains;

    ChannelGains first{};
    first[0] = 1.0f;
    gains.Write(first);

    ChannelGains second{};
    second[0] = 2.0f;
    gains.Write(second);

    const auto& result = gains.Read();
    EXPECT_FLOAT_EQ(result[0], 2.0f);
}

//==============================================================================
/// VectorAngle Tests
//==============================================================================

TEST(VBAP_VectorAngle, ForwardIsZero)
{
    // Forward direction (-Z) should be angle 0
    float angle = VectorAngle(glm::vec3(0.0f, 0.0f, -1.0f));
    EXPECT_NEAR(angle, 0.0f, 1e-5f);
}

TEST(VBAP_VectorAngle, RightIs90)
{
    // Right (+X) should be ~90 degrees
    float angle = VectorAngle(glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_NEAR(glm::degrees(angle), 90.0f, 1e-3f);
}

TEST(VBAP_VectorAngle, LeftIsMinus90)
{
    // Left (-X) should be ~-90 degrees
    float angle = VectorAngle(glm::vec3(-1.0f, 0.0f, 0.0f));
    EXPECT_NEAR(glm::degrees(angle), -90.0f, 1e-3f);
}

TEST(VBAP_VectorAngle, Vec2Overload)
{
    float angle2 = VectorAngle(glm::vec2(1.0f, 0.0f));
    float angle3 = VectorAngle(glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_NEAR(angle2, angle3, 1e-5f);
}

//==============================================================================
/// VBAP Init / Algorithm Tests
//==============================================================================

class VBAPTest : public ::testing::Test
{
  protected:
    VBAPData vbapData;
    ma_channel sourceMap[MA_MAX_CHANNELS]{};
    ma_channel outputMap[MA_MAX_CHANNELS]{};

    void SetUp() override
    {
        // Stereo source → quad output
        sourceMap[0] = MA_CHANNEL_FRONT_LEFT;
        sourceMap[1] = MA_CHANNEL_FRONT_RIGHT;

        outputMap[0] = MA_CHANNEL_FRONT_LEFT;
        outputMap[1] = MA_CHANNEL_FRONT_RIGHT;
        outputMap[2] = MA_CHANNEL_BACK_LEFT;
        outputMap[3] = MA_CHANNEL_BACK_RIGHT;
    }

    void TearDown() override
    {
        VBAP::ClearVBAP(&vbapData);
    }
};

TEST_F(VBAPTest, InitSucceeds)
{
    bool result = VBAP::InitVBAP(&vbapData, 2, 4, sourceMap, outputMap);
    EXPECT_TRUE(result);
    EXPECT_FALSE(vbapData.VirtualSources.empty());
    EXPECT_FALSE(vbapData.ChannelGroups.empty());
    EXPECT_EQ(vbapData.ChannelGroups.size(), 2u); // 2 input channels
}

TEST_F(VBAPTest, VirtualSourceCountMatchesExpected)
{
    VBAP::InitVBAP(&vbapData, 2, 4, sourceMap, outputMap);
    // numVS = numOutput * 2 = 8
    EXPECT_EQ(vbapData.VirtualSources.size(), 8u);
}

TEST_F(VBAPTest, InverseMatricesCountMatchesSpeakers)
{
    VBAP::InitVBAP(&vbapData, 2, 4, sourceMap, outputMap);
    // One inverse matrix per sorted speaker pair
    EXPECT_EQ(vbapData.InverseMats.size(), vbapData.spPosSorted.size());
}

TEST_F(VBAPTest, ClearResetsData)
{
    VBAP::InitVBAP(&vbapData, 2, 4, sourceMap, outputMap);
    VBAP::ClearVBAP(&vbapData);
    EXPECT_TRUE(vbapData.VirtualSources.empty());
    EXPECT_TRUE(vbapData.ChannelGroups.empty());
    EXPECT_TRUE(vbapData.InverseMats.empty());
}

TEST_F(VBAPTest, UpdateProducesNonZeroGains)
{
    VBAP::InitVBAP(&vbapData, 2, 4, sourceMap, outputMap);

    // Create converter: quad → quad (passthrough)
    ma_channel_converter_config convConfig = ma_channel_converter_config_init(
        ma_format_f32, 4, outputMap, 4, outputMap, ma_channel_mix_mode_rectangular);
    ma_channel_converter converter;
    ma_result convResult = ma_channel_converter_init(&convConfig, nullptr, &converter);
    ASSERT_EQ(convResult, MA_SUCCESS);

    VBAP::PositionUpdateData updateData{ 0.0f, 1.0f, 1.0f, 1.0f };
    VBAP::UpdateVBAP(&vbapData, updateData, converter, true);

    // After update, at least one channel group should have non-zero gains
    bool anyNonZero = false;
    for (auto& chg : vbapData.ChannelGroups)
    {
        const auto& gains = chg.Gains.Read();
        for (sizet i = 0; i < 4; ++i)
        {
            if (gains[i] > 0.0f)
            {
                anyNonZero = true;
                break;
            }
        }
    }
    EXPECT_TRUE(anyNonZero);
    ma_channel_converter_uninit(&converter, nullptr);
}

TEST_F(VBAPTest, GainSumApproximatelyOne)
{
    VBAP::InitVBAP(&vbapData, 2, 4, sourceMap, outputMap);

    ma_channel_converter_config convConfig = ma_channel_converter_config_init(
        ma_format_f32, 4, outputMap, 4, outputMap, ma_channel_mix_mode_rectangular);
    ma_channel_converter converter;
    ma_result convResult = ma_channel_converter_init(&convConfig, nullptr, &converter);
    ASSERT_EQ(convResult, MA_SUCCESS);

    // Test at multiple angles
    for (float angleDeg = -180.0f; angleDeg <= 180.0f; angleDeg += 45.0f)
    {
        float angleRad = glm::radians(angleDeg);
        VBAP::PositionUpdateData updateData{ angleRad, 1.0f, 1.0f, 1.0f };
        VBAP::UpdateVBAP(&vbapData, updateData, converter, true);

        // For each channel group, sum of squared gains should be <= 1 (normalized)
        for (const auto& chg : vbapData.ChannelGroups)
        {
            const auto& gains = chg.Gains.Read();
            float sumSq = 0.0f;
            for (sizet i = 0; i < 4; ++i)
            {
                sumSq += gains[i] * gains[i];
            }
            // RMS-normalized overall gain at full attenuation should be <= 1
            constexpr float kGainSumEpsilon = 1e-2f;
            EXPECT_LE(sumSq, 1.0f + kGainSumEpsilon) << "At angle " << angleDeg;
        }
    }
    ma_channel_converter_uninit(&converter, nullptr);
}

//==============================================================================
/// AudioTransform Tests
//==============================================================================

TEST(AudioTransform, DefaultValues)
{
    Transform t;
    EXPECT_EQ(t.Position, glm::vec3(0.0f));
    EXPECT_EQ(t.Orientation, glm::vec3(0.0f, 0.0f, -1.0f));
    EXPECT_EQ(t.Up, glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST(AudioTransform, Equality)
{
    Transform a;
    Transform b;
    EXPECT_EQ(a, b);

    b.Position.x = 1.0f;
    EXPECT_NE(a, b);
}

//==============================================================================
/// SampleBufferOperations Tests
//==============================================================================

TEST(SampleBufferOps, ContentMatchesIdentical)
{
    float a[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float b[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    EXPECT_TRUE(SampleBufferOperations::ContentMatches(a, b, 2, 2));
}

TEST(SampleBufferOps, ContentMatchesDiffers)
{
    float a[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float b[] = { 1.0f, 2.0f, 3.0f, 5.0f };
    EXPECT_FALSE(SampleBufferOperations::ContentMatches(a, b, 2, 2));
}

TEST(SampleBufferOps, AddAndApplyGainRampConstant)
{
    float source[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float dest[4] = {};

    // Mono → mono, constant gain 0.5
    SampleBufferOperations::AddAndApplyGainRamp(dest, source, 0, 0, 1, 1, 4, 0.5f, 0.5f);

    EXPECT_FLOAT_EQ(dest[0], 0.5f);
    EXPECT_FLOAT_EQ(dest[1], 1.0f);
    EXPECT_FLOAT_EQ(dest[2], 1.5f);
    EXPECT_FLOAT_EQ(dest[3], 2.0f);
}

TEST(SampleBufferOps, AddAndApplyGainRampInterpolates)
{
    float source[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float dest[4] = {};

    // Gain ramp from 0 to 1 over 4 samples
    // Implementation uses delta = (gainEnd - gainStart) / (numSamples - 1)
    // so gain[i] = i / 3 for i in [0..3]
    SampleBufferOperations::AddAndApplyGainRamp(dest, source, 0, 0, 1, 1, 4, 0.0f, 1.0f);

    EXPECT_NEAR(dest[0], 0.0f, 1e-5f);
    EXPECT_NEAR(dest[1], 1.0f / 3.0f, 1e-5f);
    EXPECT_NEAR(dest[2], 2.0f / 3.0f, 1e-5f);
    EXPECT_NEAR(dest[3], 1.0f, 1e-5f);
}

//==============================================================================
/// ApplyGainRamp SIMD-vs-scalar parity (Finding 4)
///
/// The AVX/SSE fast path built the per-lane frame index as (i/numChannels) +
/// (lane/numChannels), but the correct index is (i + lane)/numChannels. Since i is always a
/// multiple of the SIMD width, those two agree only when numChannels evenly divides the width
/// (1/2/4/8) and DIVERGE from the scalar fallback for 3/5/6/7 channels. These tests pin the
/// fixed behaviour: every channel count now matches the exact scalar reference.
//==============================================================================

namespace
{
    // Exact scalar reference: gain per frame is gainStart + delta*frameIndex with
    // delta = (gainEnd - gainStart)/(numSamples - 1), applied to every channel of that frame.
    // Mirrors SampleBufferOperations::ApplyGainRamp's scalar fallback.
    void ApplyGainRampScalarReference(std::vector<f32>& data, u32 numSamples, u32 numChannels, f32 gainStart, f32 gainEnd)
    {
        if (numSamples == 1)
        {
            for (u32 ch = 0; ch < numChannels; ++ch)
                data[ch] *= gainEnd;
            return;
        }
        const f32 delta = (gainEnd - gainStart) / static_cast<f32>(numSamples - 1);
        for (u32 i = 0; i < numSamples; ++i)
        {
            for (u32 ch = 0; ch < numChannels; ++ch)
                data[i * numChannels + ch] *= gainStart + delta * static_cast<f32>(i);
        }
    }

    void ExpectApplyGainRampMatchesScalar(u32 numSamples, u32 numChannels, f32 gainStart, f32 gainEnd)
    {
        std::vector<f32> actual(static_cast<sizet>(numSamples) * numChannels);
        for (sizet i = 0; i < actual.size(); ++i)
            actual[i] = 1.0f + 0.01f * static_cast<f32>(i); // distinct, non-trivial samples

        std::vector<f32> expected = actual;

        SampleBufferOperations::ApplyGainRamp(actual.data(), numSamples, numChannels, gainStart, gainEnd);
        ApplyGainRampScalarReference(expected, numSamples, numChannels, gainStart, gainEnd);

        ASSERT_EQ(actual.size(), expected.size());
        for (sizet i = 0; i < actual.size(); ++i)
        {
            EXPECT_NEAR(actual[i], expected[i], 1e-4f)
                << "mismatch at element " << i << " (numChannels=" << numChannels << ", numSamples=" << numSamples << ")";
        }
    }
} // namespace

// The channel counts that don't divide the SIMD width — where the old per-lane index bug bit.
TEST(SampleBufferOps, ApplyGainRampThreeChannelMatchesScalar)
{
    ExpectApplyGainRampMatchesScalar(/*numSamples*/ 16, /*numChannels*/ 3, /*gainStart*/ 0.25f, /*gainEnd*/ 1.5f);
}

TEST(SampleBufferOps, ApplyGainRampSixChannelMatchesScalar)
{
    ExpectApplyGainRampMatchesScalar(16, 6, 0.0f, 2.0f);
}

TEST(SampleBufferOps, ApplyGainRampFiveAndSevenChannelMatchScalar)
{
    ExpectApplyGainRampMatchesScalar(9, 5, 0.5f, 1.0f);
    ExpectApplyGainRampMatchesScalar(11, 7, 0.1f, 0.9f);
}

// Regression guard: the common mono/stereo (and 4/8-channel) paths still run through SIMD and
// must stay correct after gating the fast path on (width % numChannels == 0).
TEST(SampleBufferOps, ApplyGainRampWidthDividingChannelsMatchScalar)
{
    ExpectApplyGainRampMatchesScalar(64, 1, 0.0f, 1.0f);
    ExpectApplyGainRampMatchesScalar(64, 2, 0.2f, 0.8f);
    ExpectApplyGainRampMatchesScalar(16, 4, 0.0f, 1.0f);
    ExpectApplyGainRampMatchesScalar(16, 8, 0.3f, 0.7f);
}
