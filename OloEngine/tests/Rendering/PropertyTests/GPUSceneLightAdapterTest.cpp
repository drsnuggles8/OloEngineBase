// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneLightAdapter.h"
#include "OloEngine/Renderer/LightCommon.h"

#include <glm/glm.hpp>
#include <glm/trigonometric.hpp>

#include <array>
#include <cstring>
#include <utility>

// Pins GPUSceneLightAdapter against the hand-packing Scene::ProcessScene3DSharedLogic
// performed before the canonical light record existed (Scene.cpp at f5cbfb70b,
// lines 7096-7330). Every expected struct below is that packing copied
// expression for expression, comments included, so the adapter cannot drift
// from what the raster shaders were tuned against. Equality is bitwise: the
// adapter is a copy, and a copy has no tolerance.
namespace OloEngine::Tests
{
    namespace
    {
        // Authored values: nothing is 0 or 1 and no two lanes share a value, so
        // a lane landing in the wrong place cannot pass by coincidence. The spot
        // direction is deliberately not unit length and the cone angles are not
        // round numbers.
        const glm::vec3 kPosition{ 12.5f, -3.25f, 7.75f };
        const glm::vec3 kDirection{ 0.3f, -1.7f, 0.45f };
        const glm::vec3 kColor{ 0.9f, 0.35f, 0.15f };
        constexpr f32 kIntensity = 4.75f;
        constexpr f32 kRange = 27.5f;
        constexpr f32 kRadius = 0.85f;
        constexpr f32 kInnerCutoff = 13.7f;
        constexpr f32 kOuterCutoff = 29.3f;
        constexpr f32 kAttenuation = 0.065f;

        // Bitwise equality with a readable failure: every struct here is a row
        // of float lanes, so the first differing lanes are printed as floats.
        template<typename T>
        [[nodiscard]] ::testing::AssertionResult SameBytes(const T& actual, const T& expected)
        {
            static_assert(sizeof(T) % sizeof(f32) == 0);
            if (Math::BitwiseEqual(actual, expected))
            {
                return ::testing::AssertionSuccess();
            }
            std::array<f32, sizeof(T) / sizeof(f32)> actualLanes{};
            std::array<f32, sizeof(T) / sizeof(f32)> expectedLanes{};
            std::memcpy(actualLanes.data(), &actual, sizeof(T));
            std::memcpy(expectedLanes.data(), &expected, sizeof(T));
            ::testing::AssertionResult result = ::testing::AssertionFailure();
            for (sizet lane = 0, laneCount = actualLanes.size(); lane < laneCount; ++lane)
            {
                if (!Math::BitwiseEqual(actualLanes[lane], expectedLanes[lane]))
                {
                    result << "vec4 " << lane / 4 << "." << "xyzw"[lane % 4] << ": " << actualLanes[lane]
                           << " != " << expectedLanes[lane] << "; ";
                }
            }
            return result;
        }

        // Scene.cpp fills only the fields the component of each type owns; the
        // rest keep GPUSceneLightInput's defaults, m_SpotFalloff = 1.0 included,
        // which is the constant the pre-record packing wrote into SpotParams.z.
        [[nodiscard]] GPUSceneLightInput AuthoredLight(GPUSceneLightType type)
        {
            GPUSceneLightInput input;
            input.m_Type = std::to_underlying(type);
            input.m_Color = kColor;
            input.m_Intensity = kIntensity;
            input.m_CastShadows = true;
            switch (type)
            {
                case GPUSceneLightType::Directional:
                    input.m_Direction = kDirection;
                    break;
                case GPUSceneLightType::Point:
                    input.m_Position = kPosition;
                    input.m_Range = kRange;
                    input.m_Attenuation = kAttenuation;
                    break;
                case GPUSceneLightType::Spot:
                    input.m_Position = kPosition;
                    input.m_Direction = kDirection;
                    input.m_Range = kRange;
                    input.m_InnerCutoffDegrees = kInnerCutoff;
                    input.m_OuterCutoffDegrees = kOuterCutoff;
                    input.m_Attenuation = kAttenuation;
                    break;
                case GPUSceneLightType::SphereArea:
                    input.m_Position = kPosition;
                    input.m_Range = kRange;
                    input.m_Radius = kRadius;
                    break;
            }
            return input;
        }

        // A zero render origin, exactly as Scene.cpp encodes for the adapter:
        // the raster upload sites shift the positions themselves.
        [[nodiscard]] GPUSceneLight Encode(GPUSceneLightType type)
        {
            return EncodeGPUSceneLight(AuthoredLight(type), glm::vec3(0.0f), 0, 0);
        }
    } // namespace

    TEST(GPUSceneLightAdapter, DirectionalMatchesThePreRecordMultiLightPacking)
    {
        const GPUSceneLight record = Encode(GPUSceneLightType::Directional);

        UBOStructures::MultiLightData expected{};
        expected.Position = glm::vec4(kDirection, 0.0f);   // w=0 for directional
        expected.Direction = glm::vec4(kDirection, -1.0f); // w=-1 = no shadow index
        expected.Color = glm::vec4(kColor, kIntensity);
        expected.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        expected.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // type = DIRECTIONAL_LIGHT = 0
        EXPECT_TRUE(SameBytes(GPUSceneLightAdapter::ToMultiLightData(record), expected));
    }

    TEST(GPUSceneLightAdapter, PointMatchesThePreRecordMultiLightAndForwardPlusPacking)
    {
        const GPUSceneLight record = Encode(GPUSceneLightType::Point);

        // ShadowAndAttenuation.x = -1 (no atlas entry) until the atlas allocation
        // patches the winners; .y carries the quadratic attenuation coefficient.
        const GPUPointLight expectedForwardPlus{ glm::vec4(kPosition, kRange),
                                                 glm::vec4(kColor, kIntensity),
                                                 glm::vec4(-1.0f, kAttenuation, 0.0f, 0.0f) };
        EXPECT_TRUE(SameBytes(GPUSceneLightAdapter::ToForwardPlusPoint(record), expectedForwardPlus));

        UBOStructures::MultiLightData expected{};
        expected.Position = glm::vec4(kPosition, 1.0f); // w=1 for point
        expected.Color = glm::vec4(kColor, kIntensity);
        expected.AttenuationParams = glm::vec4(1.0f, 0.0f, kAttenuation, kRange);
        expected.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);  // type = POINT_LIGHT = 1
        expected.Direction = glm::vec4(0.0f, -1.0f, 0.0f, -1.0f); // w = atlas base entry, patched later
        EXPECT_TRUE(SameBytes(GPUSceneLightAdapter::ToMultiLightData(record), expected));
    }

    TEST(GPUSceneLightAdapter, SpotMatchesThePreRecordMultiLightAndForwardPlusPacking)
    {
        const GPUSceneLight record = Encode(GPUSceneLightType::Spot);

        // The packing sanitised the authored direction first (LightCommon.h) and
        // handed the SAME unnormalised vector to the UBO and, normalised, to the
        // Forward+ SSBO. Scene.cpp still sanitises before it builds the input.
        const glm::vec3 spotDir = SanitizeSpotLightDirection(kDirection);
        EXPECT_TRUE(Math::BitwiseEqual(spotDir, kDirection)) << "a finite non-zero direction passes through unchanged";

        // SpotParams.z = -1 (no atlas entry) until the atlas allocation patches the winners.
        const GPUSpotLight expectedForwardPlus{
            glm::vec4(kPosition, kRange),
            glm::vec4(glm::normalize(spotDir), glm::cos(glm::radians(kOuterCutoff))),
            glm::vec4(kColor, kIntensity),
            glm::vec4(glm::cos(glm::radians(kInnerCutoff)), kAttenuation, -1.0f, 0.0f)
        };
        EXPECT_TRUE(SameBytes(GPUSceneLightAdapter::ToForwardPlusSpot(record), expectedForwardPlus));

        UBOStructures::MultiLightData expected{};
        expected.Position = glm::vec4(kPosition, 2.0f); // w=2 for spot
        expected.Color = glm::vec4(kColor, kIntensity);
        expected.AttenuationParams = glm::vec4(1.0f, 0.0f, kAttenuation, kRange);
        expected.SpotParams = glm::vec4(glm::cos(glm::radians(kInnerCutoff)),
                                        glm::cos(glm::radians(kOuterCutoff)),
                                        1.0f,
                                        2.0f // type = SPOT_LIGHT = 2
        );
        expected.Direction = glm::vec4(spotDir, -1.0f); // w = atlas base entry, patched later
        EXPECT_TRUE(SameBytes(GPUSceneLightAdapter::ToMultiLightData(record), expected));
    }

    TEST(GPUSceneLightAdapter, SphereAreaMatchesThePreRecordMultiLightAndForwardPlusPacking)
    {
        const GPUSceneLight record = Encode(GPUSceneLightType::SphereArea);

        // RangeAndPadding.y = -1 (no atlas entry) until patched.
        const GPUSphereAreaLight expectedForwardPlus{ glm::vec4(kPosition, kRadius),
                                                      glm::vec4(kColor, kIntensity),
                                                      glm::vec4(kRange, -1.0f, 0.0f, 0.0f) };
        EXPECT_TRUE(SameBytes(GPUSceneLightAdapter::ToForwardPlusSphereArea(record), expectedForwardPlus));

        // The emitter sphere radius rides SpotParams.z; PBRCommon.glsl decodes it there.
        UBOStructures::MultiLightData expected{};
        expected.Position = glm::vec4(kPosition, 3.0f); // w=3 for sphere area
        expected.Color = glm::vec4(kColor, kIntensity);
        expected.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, kRange);
        expected.SpotParams = glm::vec4(0.0f, 0.0f, kRadius, 3.0f); // type = SPHERE_AREA_LIGHT = 3
        expected.Direction = glm::vec4(0.0f, -1.0f, 0.0f, -1.0f);   // w = atlas base entry, patched later
        EXPECT_TRUE(SameBytes(GPUSceneLightAdapter::ToMultiLightData(record), expected));
    }

    TEST(GPUSceneLightAdapter, TypeTagsAndShadowSentinelsFollowGPUSceneLightType)
    {
        // PBRCommon.glsl: DIRECTIONAL_LIGHT 0, POINT_LIGHT 1, SPOT_LIGHT 2, SPHERE_AREA_LIGHT 3.
        static_assert(std::to_underlying(GPUSceneLightType::Directional) == 0u);
        static_assert(std::to_underlying(GPUSceneLightType::Point) == 1u);
        static_assert(std::to_underlying(GPUSceneLightType::Spot) == 2u);
        static_assert(std::to_underlying(GPUSceneLightType::SphereArea) == 3u);

        for (u32 type = 0; type <= 3; ++type)
        {
            SCOPED_TRACE(::testing::Message() << "light type " << type);
            const GPUSceneLight record = Encode(static_cast<GPUSceneLightType>(type));
            EXPECT_EQ(std::to_underlying(GPUSceneLightAdapter::TypeOf(record)), type);

            const UBOStructures::MultiLightData data = GPUSceneLightAdapter::ToMultiLightData(record);
            EXPECT_FLOAT_EQ(data.Position.w, static_cast<f32>(type)) << "PBRCommon.glsl reads the type from Position.w";
            EXPECT_FLOAT_EQ(data.SpotParams.w, static_cast<f32>(type)) << "... and again from SpotParams.w";
            EXPECT_FLOAT_EQ(data.Direction.w, GPUSceneLightAdapter::kNoShadowEntry)
                << "the shadow entry is a per-frame allocation Scene patches in later";
            EXPECT_EQ(record.Flags & GPUSceneLightFlagCastShadows, static_cast<u32>(GPUSceneLightFlagCastShadows))
                << "CastShadows survives in the record even though no raster struct carries it";
        }

        EXPECT_FLOAT_EQ(GPUSceneLightAdapter::kNoShadowEntry, -1.0f);
        const GPUPointLight point = GPUSceneLightAdapter::ToForwardPlusPoint(Encode(GPUSceneLightType::Point));
        const GPUSpotLight spot = GPUSceneLightAdapter::ToForwardPlusSpot(Encode(GPUSceneLightType::Spot));
        const GPUSphereAreaLight sphere =
            GPUSceneLightAdapter::ToForwardPlusSphereArea(Encode(GPUSceneLightType::SphereArea));
        EXPECT_FLOAT_EQ(point.ShadowAndAttenuation.x, GPUSceneLightAdapter::kNoShadowEntry);
        EXPECT_FLOAT_EQ(spot.SpotParams.z, GPUSceneLightAdapter::kNoShadowEntry);
        EXPECT_FLOAT_EQ(sphere.RangeAndPadding.y, GPUSceneLightAdapter::kNoShadowEntry);
    }
} // namespace OloEngine::Tests
