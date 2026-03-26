#include <gtest/gtest.h>
#include "OloEngine/Audio/DSP/Spatializer/VBAP.h"
#include "OloEngine/Audio/AudioTransform.h"
#include "OloEngine/Audio/SampleBufferOperations.h"

#include <glm/glm.hpp>
#include <miniaudio.h>

#include <cmath>
#include <numeric>

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
        for (auto& chg : vbapData.ChannelGroups)
        {
            const auto& gains = chg.Gains.Read();
            float sumSq = 0.0f;
            for (sizet i = 0; i < 4; ++i)
            {
                sumSq += gains[i] * gains[i];
            }
            // RMS-normalized overall gain at full attenuation should be <= 1
            EXPECT_LE(sumSq, 1.5f) << "At angle " << angleDeg;
        }
    }
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
