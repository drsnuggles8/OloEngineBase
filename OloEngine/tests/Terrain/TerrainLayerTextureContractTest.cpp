// OLO_TEST_LAYER: unit
// =============================================================================
// TerrainLayerTextureContractTest.cpp — the CPU half of textured terrain layers
// (issue #1721).
//
// Three contracts, none of which needs a GPU:
//
//   * THE RESAMPLING FILTER. A layer file of another size than the material's
//     layer resolution used to be POINT-sampled: a 2048 px scan built at 512
//     kept one texel in sixteen. TerrainMaterial::ResampleRGBA8 averages by
//     area when an axis shrinks and interpolates between texel centres (with
//     wrap-around, a layer tiles) when it grows.
//   * THE sRGB ENCODE. The albedo array is sRGB, and a layer with no albedo
//     file is a solid colour from TerrainLayer::BaseColor, which is LINEAR. It
//     is encoded on the way in so the sampler decodes it back to what was
//     authored.
//   * THE MACRO MAP AND THE TWO-SCALE BLEND as TerrainSurfaceShading.h states
//     them: the values the shaders are fed from, and the CPU twins of the two
//     functions in include/TerrainLayerBlend.glsl.
//
// The rendered half is Rendering/PropertyTests/TerrainLayerTextureEvidenceTest.cpp.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainSurfaceShading.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] std::span<const u8> Bytes(const TArray<u8>& pixels)
        {
            return { pixels.GetData(), static_cast<sizet>(pixels.Num()) };
        }

        // An image whose every channel of texel (x, y) is values[y * width + x],
        // with alpha 255.
        [[nodiscard]] TArray<u8> Grey(std::span<const u8> values)
        {
            TArray<u8> pixels;
            for (const u8 value : values)
            {
                pixels.Add(value);
                pixels.Add(value);
                pixels.Add(value);
                pixels.Add(255);
            }
            return pixels;
        }

        // The sRGB encode by its definition, for the table-driven one to be
        // compared with.
        [[nodiscard]] u8 ReferenceEncode(const f64 linear)
        {
            const f64 clamped = std::clamp(linear, 0.0, 1.0);
            const f64 encoded = clamped <= 0.0031308 ? clamped * 12.92 : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
            return static_cast<u8>(std::lround(encoded * 255.0));
        }
    } // namespace

    TEST(TerrainLayerTextureContract, AMinifiedCheckerIsItsMeanNotOneOfItsCorners)
    {
        // 2x2 black/white checker -> 1x1. A point sample returns 0 or 255.
        const std::array<u8, 4> checker{ 0, 255, 255, 0 };
        const TArray<u8> source = Grey(checker);

        TArray<u8> linearData;
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 1, 1, false, linearData);
        ASSERT_EQ(linearData.Num(), 4);
        // Data (a normal map, an ARM map) averages its bytes: 127.5, rounded.
        EXPECT_EQ(linearData[0], 128);
        EXPECT_EQ(linearData[1], 128);
        EXPECT_EQ(linearData[2], 128);
        EXPECT_EQ(linearData[3], 255);

        TArray<u8> colour;
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 1, 1, true, colour);
        ASSERT_EQ(colour.Num(), 4);
        // Colour averages LIGHT: half of white is linear 0.5, which sRGB stores
        // as 188. Averaging the bytes instead (128) is linear 0.216 — a minified
        // high-contrast albedo that came out less than half as bright.
        EXPECT_EQ(colour[0], 188);
        EXPECT_EQ(colour[0], ReferenceEncode(0.5));
        EXPECT_EQ(colour[3], 255);
    }

    TEST(TerrainLayerTextureContract, EveryDestinationTexelAveragesTheSourceAreaItCovers)
    {
        // 4x4 -> 2x2: each destination texel is the mean of its own 2x2 block.
        const std::array<u8, 16> values{ 10, 20, 100, 120, //
                                         30, 40, 140, 160, //
                                         200, 210, 0, 0,   //
                                         220, 230, 0, 4 };
        TArray<u8> out;
        TerrainMaterial::ResampleRGBA8(Bytes(Grey(values)), 4, 4, 2, 2, false, out);
        ASSERT_EQ(out.Num(), 16);
        EXPECT_EQ(out[0], 25);  // (10 + 20 + 30 + 40) / 4
        EXPECT_EQ(out[4], 130); // (100 + 120 + 140 + 160) / 4
        EXPECT_EQ(out[8], 215); // (200 + 210 + 220 + 230) / 4
        EXPECT_EQ(out[12], 1);  // (0 + 0 + 0 + 4) / 4

        // 3 -> 2 along X: a destination texel covers 1.5 source texels, so the
        // middle one is shared half and half.
        const std::array<u8, 3> row{ 0, 90, 240 };
        TerrainMaterial::ResampleRGBA8(Bytes(Grey(row)), 3, 1, 2, 1, false, out);
        ASSERT_EQ(out.Num(), 8);
        EXPECT_EQ(out[0], 30);  // (0 * 1 + 90 * 0.5) / 1.5
        EXPECT_EQ(out[4], 190); // (90 * 0.5 + 240 * 1) / 1.5
    }

    TEST(TerrainLayerTextureContract, AMagnifiedImageIsInterpolatedAndWrapsAtItsEdges)
    {
        // 2 -> 4 along X. Destination texel centres fall at source positions
        // -0.25, 0.25, 0.75, 1.25, and a layer tiles, so -0.25 lies between the
        // LAST source texel and the first.
        const std::array<u8, 2> row{ 0, 200 };
        TArray<u8> out;
        TerrainMaterial::ResampleRGBA8(Bytes(Grey(row)), 2, 1, 4, 1, false, out);
        ASSERT_EQ(out.Num(), 16);
        EXPECT_EQ(out[0], 50);   // 0.75 * 0 + 0.25 * 200 (wrapped)
        EXPECT_EQ(out[4], 50);   // 0.75 * 0 + 0.25 * 200
        EXPECT_EQ(out[8], 150);  // 0.25 * 0 + 0.75 * 200
        EXPECT_EQ(out[12], 150); // 0.75 * 200 + 0.25 * 0 (wrapped)
    }

    TEST(TerrainLayerTextureContract, ResamplingToTheSameSizeChangesNothing)
    {
        const std::array<u8, 4> values{ 1, 77, 201, 255 };
        const TArray<u8> source = Grey(values);
        TArray<u8> out;
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 2, 2, false, out);
        EXPECT_EQ(out, source);
        // The colour path decodes and re-encodes every channel; that round trip
        // must be the identity on bytes or an unresampled albedo would drift.
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 2, 2, true, out);
        EXPECT_EQ(out, source);
    }

    TEST(TerrainLayerTextureContract, TheSRGBEncodeIsTheStandardCurveAndInvertsTheDecode)
    {
        for (u32 byte = 0; byte < 256; ++byte)
        {
            EXPECT_EQ(TerrainMaterial::LinearToSRGB8(TerrainMaterial::SRGB8ToLinear(static_cast<u8>(byte))), byte)
                << "sRGB byte " << byte << " does not survive decode + encode";
        }

        // Against the definition, at values that are not byte centres.
        for (u32 step = 0; step <= 4000; ++step)
        {
            const f32 linear = static_cast<f32>(step) / 4000.0f;
            const int encoded = TerrainMaterial::LinearToSRGB8(linear);
            const int reference = ReferenceEncode(static_cast<f64>(linear));
            // Equal, except within float rounding of a byte boundary.
            EXPECT_LE(std::abs(encoded - reference), 1) << "linear " << linear;
        }

        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(0.0f), 0);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(-1.0f), 0);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(1.0f), 255);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(7.0f), 255);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(std::numeric_limits<f32>::quiet_NaN()), 0);
    }

    TEST(TerrainLayerTextureContract, ASolidLayerColourSurvivesTheSRGBArray)
    {
        // What a layer without an albedo file goes through: BaseColor (linear)
        // -> LinearToSRGB8 -> the sRGB array -> the sampler's decode. The value
        // that comes back is within half an sRGB step of the authored one, and
        // an sRGB step is at most 2.4 / 1.055 / 255 of linear range (at white).
        constexpr f32 kWorstHalfStep = 0.5f * 2.4f / 1.055f / 255.0f + 1e-4f;
        for (u32 step = 0; step <= 1000; ++step)
        {
            const f32 authored = static_cast<f32>(step) / 1000.0f;
            const f32 sampled = TerrainMaterial::SRGB8ToLinear(TerrainMaterial::LinearToSRGB8(authored));
            EXPECT_NEAR(sampled, authored, kWorstHalfStep) << "authored linear " << authored;
        }
        // The default layer colour, 0.5: stored as 188, sampled as 0.5029.
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(0.5f), 188);
        EXPECT_NEAR(TerrainMaterial::SRGB8ToLinear(188), 0.5029f, 5e-4f);
    }

    TEST(TerrainLayerTextureContract, TheLayerResolutionIsAPowerOfTwoInItsRange)
    {
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(0), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(63), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(64), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(100), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(512), 512u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(1000), 512u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(2048), 2048u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(4096), 4096u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(5000), 4096u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(0xFFFFFFFFu), 4096u);

        // A material is 512 until told otherwise: what every scene was built at.
        TerrainMaterial material;
        EXPECT_EQ(material.GetLayerResolution(), 512u);
        material.SetLayerResolution(1500);
        EXPECT_EQ(material.GetLayerResolution(), 1024u);
    }

    TEST(TerrainLayerTextureContract, ANeutralMacroTexelLeavesTheSurfaceAsItWas)
    {
        using namespace TerrainSurfaceShading;
        const glm::vec3 albedo(0.31f, 0.47f, 0.12f);
        const f32 roughness = 0.83f;

        // Byte 128 in RGB, 255 in alpha: what an untouched macro map holds.
        const glm::vec4 neutral(static_cast<f32>(kMacroNeutralTint) / 255.0f, static_cast<f32>(kMacroNeutralTint) / 255.0f,
                                static_cast<f32>(kMacroNeutralTint) / 255.0f, static_cast<f32>(kMacroDryAlpha) / 255.0f);
        const MacroResult result = ApplyMacro(albedo, roughness, neutral);
        EXPECT_NEAR(result.Albedo.r, albedo.r, 1e-6f);
        EXPECT_NEAR(result.Albedo.g, albedo.g, 1e-6f);
        EXPECT_NEAR(result.Albedo.b, albedo.b, 1e-6f);
        EXPECT_FLOAT_EQ(result.Roughness, roughness);
    }

    TEST(TerrainLayerTextureContract, TheMacroTintScalesAndWetnessDarkensAndSmooths)
    {
        using namespace TerrainSurfaceShading;
        const glm::vec3 albedo(0.4f, 0.4f, 0.4f);
        const f32 roughness = 0.9f;

        // Tint: 64 halves a channel, 255 nearly doubles it, 128 leaves it.
        const MacroResult tinted =
            ApplyMacro(albedo, roughness, glm::vec4(64.0f / 255.0f, 128.0f / 255.0f, 255.0f / 255.0f, 1.0f));
        EXPECT_NEAR(tinted.Albedo.r, 0.4f * 64.0f / 128.0f, 1e-6f);
        EXPECT_NEAR(tinted.Albedo.g, 0.4f, 1e-6f);
        EXPECT_NEAR(tinted.Albedo.b, 0.4f * 255.0f / 128.0f, 1e-6f);
        EXPECT_FLOAT_EQ(tinted.Roughness, roughness);

        // Fully wet (alpha 0): albedo * 0.55, roughness 0.12.
        const MacroResult wet = ApplyMacro(albedo, roughness, glm::vec4(glm::vec3(128.0f / 255.0f), 0.0f));
        EXPECT_NEAR(wet.Albedo.r, 0.4f * 0.55f, 1e-6f);
        EXPECT_NEAR(wet.Roughness, 0.12f, 1e-6f);

        // Half wet: half way on both.
        const MacroResult damp = ApplyMacro(albedo, roughness, glm::vec4(glm::vec3(128.0f / 255.0f), 0.5f));
        EXPECT_NEAR(damp.Albedo.r, 0.4f * 0.775f, 1e-6f);
        EXPECT_NEAR(damp.Roughness, 0.51f, 1e-6f);
    }

    TEST(TerrainLayerTextureContract, TheFarScaleFadesInBetweenTheTwoDistances)
    {
        using namespace TerrainSurfaceShading;
        EXPECT_FLOAT_EQ(FarScaleBlend(0.0f), 0.0f);
        EXPECT_FLOAT_EQ(FarScaleBlend(kNearDistance), 0.0f);
        EXPECT_FLOAT_EQ(FarScaleBlend(0.5f * (kNearDistance + kFarDistance)), 0.5f);
        EXPECT_FLOAT_EQ(FarScaleBlend(kFarDistance), 1.0f);
        EXPECT_FLOAT_EQ(FarScaleBlend(10000.0f), 1.0f);
        f32 previous = 0.0f;
        for (f32 distance = kNearDistance; distance <= kFarDistance; distance += 1.0f)
        {
            const f32 blend = FarScaleBlend(distance);
            EXPECT_GE(blend, previous) << "the far scale faded back out at " << distance << " m";
            previous = blend;
        }

        // The contract values, pinned: a scene's content is authored against
        // them.
        EXPECT_FLOAT_EQ(kFarTilingRatio, 0.137f);
        EXPECT_FLOAT_EQ(kNearDistance, 40.0f);
        EXPECT_FLOAT_EQ(kFarDistance, 160.0f);
        // Not a divisor of one: an integer ratio would line the two grids up.
        EXPECT_GT(std::abs(1.0f / kFarTilingRatio - std::round(1.0f / kFarTilingRatio)), 0.2f);
    }

    TEST(TerrainLayerTextureContract, TheUniformLanesAreZeroWhenTheFeatureIsOff)
    {
        using namespace TerrainSurfaceShading;
        // Zero is what the shaders test for: no far scale, no macro step.
        const glm::vec4 singleScale = PackLayerScale(false);
        EXPECT_FLOAT_EQ(singleScale.x, 0.0f);
        const glm::vec4 noMacro = PackMacroParams(false);
        EXPECT_FLOAT_EQ(noMacro.x, 0.0f);

        const glm::vec4 twoScale = PackLayerScale(true);
        EXPECT_FLOAT_EQ(twoScale.x, kFarTilingRatio);
        EXPECT_FLOAT_EQ(twoScale.y, kNearDistance);
        EXPECT_FLOAT_EQ(twoScale.z, kFarDistance);
        const glm::vec4 macro = PackMacroParams(true);
        EXPECT_FLOAT_EQ(macro.x, 1.0f);
        EXPECT_FLOAT_EQ(macro.y, 255.0f / 128.0f);
        EXPECT_FLOAT_EQ(macro.z, 0.55f);
        EXPECT_FLOAT_EQ(macro.w, 0.12f);
    }
} // namespace OloEngine::Tests
