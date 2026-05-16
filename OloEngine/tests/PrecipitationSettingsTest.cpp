#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// PrecipitationSettings Defaults
// =============================================================================

// Defaults-sanity blocks intentionally retired — they pinned design choices
// in the header rather than contracts; see docs/testing.md §4.1.

TEST(PrecipitationSettings, TypeEnumValues)
{
    EXPECT_EQ(static_cast<int>(PrecipitationType::Snow), 0);
    EXPECT_EQ(static_cast<int>(PrecipitationType::Rain), 1);
    EXPECT_EQ(static_cast<int>(PrecipitationType::Hail), 2);
    EXPECT_EQ(static_cast<int>(PrecipitationType::Sleet), 3);
}

TEST(PrecipitationSettings, IntensityEnumValues)
{
    EXPECT_EQ(static_cast<int>(PrecipitationIntensity::None), 0);
    EXPECT_EQ(static_cast<int>(PrecipitationIntensity::Light), 1);
    EXPECT_EQ(static_cast<int>(PrecipitationIntensity::Moderate), 2);
    EXPECT_EQ(static_cast<int>(PrecipitationIntensity::Heavy), 3);
    EXPECT_EQ(static_cast<int>(PrecipitationIntensity::Blizzard), 4);
}

// =============================================================================
// PrecipitationUBOData Layout (std140 alignment)
// =============================================================================

TEST(PrecipitationUBOData, SizeIs80Bytes)
{
    // 5 vec4s = 5 * 16 = 80 bytes
    EXPECT_EQ(PrecipitationUBOData::GetSize(), 80u);
    EXPECT_EQ(sizeof(PrecipitationUBOData), 80u);
}

TEST(PrecipitationUBOData, FieldOffsetsMatchStd140)
{
    // std140 requires vec4 alignment (16 bytes)
    EXPECT_EQ(offsetof(PrecipitationUBOData, IntensityAndScreenFX), 0u);
    EXPECT_EQ(offsetof(PrecipitationUBOData, LensParams), 16u);
    EXPECT_EQ(offsetof(PrecipitationUBOData, ScreenWindAndTime), 32u);
    EXPECT_EQ(offsetof(PrecipitationUBOData, ParticleColor), 48u);
    EXPECT_EQ(offsetof(PrecipitationUBOData, TypeParams), 64u);
}

// =============================================================================
// Binding Index Tests
// =============================================================================

TEST(PrecipitationBindings, UBOBindingIs18)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_PRECIPITATION, 18u);
}

TEST(PrecipitationBindings, TextureBindingIs31)
{
    EXPECT_EQ(ShaderBindingLayout::TEX_PRECIPITATION_NOISE, 31u);
}

TEST(PrecipitationBindings, UBOBindingIsKnown)
{
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_PRECIPITATION, "Precipitation"));
}

TEST(PrecipitationBindings, TextureBindingIsKnown)
{
    EXPECT_TRUE(ShaderBindingLayout::IsKnownTextureBinding(ShaderBindingLayout::TEX_PRECIPITATION_NOISE, "PrecipitationNoise"));
}

// =============================================================================
// Emission Rate Calculation — covered by PrecipitationEmitterTest
// =============================================================================

// NearFieldExtentSmallerThanFarField and SpeedRangesAreValid retired —
// these compared default-constructed values against each other (design
// choices in the settings header, not invariants). docs/testing.md §4.1.
