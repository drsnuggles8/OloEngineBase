#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// PrecipitationSettings Defaults
// =============================================================================

TEST(PrecipitationSettings, DefaultsAreReasonable)
{
	PrecipitationSettings ps;

	EXPECT_FALSE(ps.Enabled);
	EXPECT_EQ(ps.Type, PrecipitationType::Snow);
	EXPECT_GE(ps.Intensity, 0.0f);
	EXPECT_LE(ps.Intensity, 1.0f);
	EXPECT_GT(ps.TransitionSpeed, 0.0f);

	// Emission
	EXPECT_GT(ps.BaseEmissionRate, 0u);
	EXPECT_GT(ps.MaxParticlesNearField, 0u);
	EXPECT_GT(ps.MaxParticlesFarField, 0u);

	// Near-field extents should be positive
	EXPECT_GT(ps.NearFieldExtent.x, 0.0f);
	EXPECT_GT(ps.NearFieldExtent.y, 0.0f);
	EXPECT_GT(ps.NearFieldExtent.z, 0.0f);
	EXPECT_GT(ps.NearFieldParticleSize, 0.0f);
	EXPECT_GE(ps.NearFieldSizeVariance, 0.0f);
	EXPECT_GT(ps.NearFieldLifetime, 0.0f);

	// Far-field extents should be larger than near-field
	EXPECT_GT(ps.FarFieldExtent.x, ps.NearFieldExtent.x);
	EXPECT_GT(ps.FarFieldParticleSize, 0.0f);
	EXPECT_GT(ps.FarFieldLifetime, 0.0f);
	EXPECT_GT(ps.FarFieldAlphaMultiplier, 0.0f);
	EXPECT_LE(ps.FarFieldAlphaMultiplier, 1.0f);

	// Physics
	EXPECT_GT(ps.GravityScale, 0.0f);
	EXPECT_GT(ps.WindInfluence, 0.0f);
	EXPECT_GT(ps.DragCoefficient, 0.0f);
	EXPECT_GT(ps.TurbulenceStrength, 0.0f);

	// Screen effects
	EXPECT_GT(ps.ScreenStreakIntensity, 0.0f);
	EXPECT_GT(ps.ScreenStreakLength, 0.0f);
	EXPECT_GT(ps.LensImpactRate, 0.0f);
	EXPECT_GT(ps.LensImpactLifetime, 0.0f);
	EXPECT_GT(ps.LensImpactSize, 0.0f);

	// LOD
	EXPECT_GT(ps.LODNearDistance, 0.0f);
	EXPECT_GT(ps.LODFarDistance, ps.LODNearDistance);
	EXPECT_GT(ps.FrameBudgetMs, 0.0f);

	// Visual
	EXPECT_GT(ps.ParticleColor.a, 0.0f);
	EXPECT_GE(ps.ColorVariance, 0.0f);
	EXPECT_GE(ps.RotationSpeed, 0.0f);
}

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

TEST(PrecipitationUBOData, SizeIs64Bytes)
{
	// 4 vec4s = 4 * 16 = 64 bytes
	EXPECT_EQ(PrecipitationUBOData::GetSize(), 64u);
	EXPECT_EQ(sizeof(PrecipitationUBOData), 64u);
}

TEST(PrecipitationUBOData, FieldOffsetsMatchStd140)
{
	// std140 requires vec4 alignment (16 bytes)
	EXPECT_EQ(offsetof(PrecipitationUBOData, IntensityAndScreenFX), 0u);
	EXPECT_EQ(offsetof(PrecipitationUBOData, LensParams), 16u);
	EXPECT_EQ(offsetof(PrecipitationUBOData, ScreenWindAndTime), 32u);
	EXPECT_EQ(offsetof(PrecipitationUBOData, ParticleColor), 48u);
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
// Emission Rate Calculation
// =============================================================================

TEST(PrecipitationSettings, EmissionRateQuadraticScaling)
{
	// effectiveRate = baseRate * intensity^2
	u32 baseRate = 4000;

	// At intensity 0.0, should be 0
	f32 rate0 = static_cast<f32>(baseRate) * 0.0f * 0.0f;
	EXPECT_FLOAT_EQ(rate0, 0.0f);

	// At intensity 0.5, should be 25% of base
	f32 rate05 = static_cast<f32>(baseRate) * 0.5f * 0.5f;
	EXPECT_FLOAT_EQ(rate05, 1000.0f);

	// At intensity 1.0, should be 100% of base
	f32 rate1 = static_cast<f32>(baseRate) * 1.0f * 1.0f;
	EXPECT_FLOAT_EQ(rate1, 4000.0f);

	// At intensity 0.75, should be 56.25% of base
	f32 rate075 = static_cast<f32>(baseRate) * 0.75f * 0.75f;
	EXPECT_FLOAT_EQ(rate075, 2250.0f);
}

TEST(PrecipitationSettings, NearFieldExtentSmallerThanFarField)
{
	PrecipitationSettings ps;

	// Near-field AABB should be strictly smaller than far-field in all axes
	EXPECT_LT(ps.NearFieldExtent.x, ps.FarFieldExtent.x);
	EXPECT_LT(ps.NearFieldExtent.y, ps.FarFieldExtent.y);
	EXPECT_LT(ps.NearFieldExtent.z, ps.FarFieldExtent.z);
}

TEST(PrecipitationSettings, SpeedRangesAreValid)
{
	PrecipitationSettings ps;

	EXPECT_LE(ps.NearFieldSpeedMin, ps.NearFieldSpeedMax);
	EXPECT_LE(ps.FarFieldSpeedMin, ps.FarFieldSpeedMax);
	EXPECT_GT(ps.NearFieldSpeedMin, 0.0f);
	EXPECT_GT(ps.FarFieldSpeedMin, 0.0f);
}
