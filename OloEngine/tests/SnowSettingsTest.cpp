#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// SnowSettings Defaults
// =============================================================================

TEST(SnowSettings, DefaultsAreReasonable)
{
	SnowSettings snow;

	EXPECT_FALSE(snow.Enabled);
	EXPECT_FALSE(snow.SSSBlurEnabled);

	// Coverage
	EXPECT_LT(snow.HeightStart, snow.HeightFull);
	EXPECT_GT(snow.SlopeStart, snow.SlopeFull);
	EXPECT_GT(snow.SlopeStart, 0.0f);
	EXPECT_LT(snow.SlopeStart, 1.0f);

	// Material
	EXPECT_GT(snow.Roughness, 0.0f);
	EXPECT_LE(snow.Roughness, 1.0f);
	EXPECT_GT(snow.Albedo.r, 0.9f);
	EXPECT_GT(snow.Albedo.g, 0.9f);
	EXPECT_GT(snow.Albedo.b, 0.9f);

	// SSS
	EXPECT_GT(snow.SSSIntensity, 0.0f);
	EXPECT_LE(snow.SSSIntensity, 1.0f);

	// Sparkle
	EXPECT_GT(snow.SparkleIntensity, 0.0f);
	EXPECT_GT(snow.SparkleDensity, 0.0f);
	EXPECT_GT(snow.SparkleScale, 0.0f);

	// Normal perturbation
	EXPECT_GT(snow.NormalPerturbStrength, 0.0f);

	// Blur
	EXPECT_GT(snow.SSSBlurRadius, 0.0f);
	EXPECT_GT(snow.SSSBlurFalloff, 0.0f);
}

// =============================================================================
// SnowUBOData Layout (std140 alignment)
// =============================================================================

TEST(SnowUBOData, SizeIs80Bytes)
{
	// 5 vec4s = 5 * 16 = 80 bytes
	EXPECT_EQ(SnowUBOData::GetSize(), 80u);
	EXPECT_EQ(sizeof(SnowUBOData), 80u);
}

TEST(SnowUBOData, FieldOffsets_Std140Compatible)
{
	// Each vec4 is 16 bytes, std140 aligned
	// Row 0: CoverageParams   (HeightStart, HeightFull, SlopeStart, SlopeFull)
	// Row 1: AlbedoAndRoughness (Albedo.rgb, Roughness)
	// Row 2: SSSColorAndIntensity (SSSColor.rgb, SSSIntensity)
	// Row 3: SparkleParams    (SparkleIntensity, SparkleDensity, SparkleScale, NormalPerturbStrength)
	// Row 4: Flags            (Enabled, pad, pad, pad)

	EXPECT_EQ(offsetof(SnowUBOData, CoverageParams), 0u);
	EXPECT_EQ(offsetof(SnowUBOData, AlbedoAndRoughness), 16u);
	EXPECT_EQ(offsetof(SnowUBOData, SSSColorAndIntensity), 32u);
	EXPECT_EQ(offsetof(SnowUBOData, SparkleParams), 48u);
	EXPECT_EQ(offsetof(SnowUBOData, Flags), 64u);
}

TEST(SnowUBOData, DefaultsMatchSettings)
{
	SnowSettings settings;
	SnowUBOData gpu;

	// Coverage
	EXPECT_FLOAT_EQ(gpu.CoverageParams.x, settings.HeightStart);
	EXPECT_FLOAT_EQ(gpu.CoverageParams.y, settings.HeightFull);
	EXPECT_FLOAT_EQ(gpu.CoverageParams.z, settings.SlopeStart);
	EXPECT_FLOAT_EQ(gpu.CoverageParams.w, settings.SlopeFull);

	// Albedo + Roughness
	EXPECT_FLOAT_EQ(gpu.AlbedoAndRoughness.x, settings.Albedo.r);
	EXPECT_FLOAT_EQ(gpu.AlbedoAndRoughness.y, settings.Albedo.g);
	EXPECT_FLOAT_EQ(gpu.AlbedoAndRoughness.z, settings.Albedo.b);
	EXPECT_FLOAT_EQ(gpu.AlbedoAndRoughness.w, settings.Roughness);

	// SSS
	EXPECT_FLOAT_EQ(gpu.SSSColorAndIntensity.x, settings.SSSColor.r);
	EXPECT_FLOAT_EQ(gpu.SSSColorAndIntensity.y, settings.SSSColor.g);
	EXPECT_FLOAT_EQ(gpu.SSSColorAndIntensity.z, settings.SSSColor.b);
	EXPECT_FLOAT_EQ(gpu.SSSColorAndIntensity.w, settings.SSSIntensity);

	// Sparkle
	EXPECT_FLOAT_EQ(gpu.SparkleParams.x, settings.SparkleIntensity);
	EXPECT_FLOAT_EQ(gpu.SparkleParams.y, settings.SparkleDensity);
	EXPECT_FLOAT_EQ(gpu.SparkleParams.z, settings.SparkleScale);
	EXPECT_FLOAT_EQ(gpu.SparkleParams.w, settings.NormalPerturbStrength);

	// Disabled by default
	EXPECT_FLOAT_EQ(gpu.Flags.x, 0.0f);
}

// =============================================================================
// SSSUBOData Layout (std140 alignment)
// =============================================================================

TEST(SSSUBOData, SizeIs32Bytes)
{
	// 2 vec4s = 2 * 16 = 32 bytes
	EXPECT_EQ(SSSUBOData::GetSize(), 32u);
	EXPECT_EQ(sizeof(SSSUBOData), 32u);
}

TEST(SSSUBOData, FieldOffsets_Std140Compatible)
{
	// Row 0: BlurParams (BlurRadius, BlurFalloff, ScreenWidth, ScreenHeight)
	// Row 1: Flags      (Enabled, pad, pad, pad)

	EXPECT_EQ(offsetof(SSSUBOData, BlurParams), 0u);
	EXPECT_EQ(offsetof(SSSUBOData, Flags), 16u);
}

TEST(SSSUBOData, DefaultsMatchSettings)
{
	SnowSettings settings;
	SSSUBOData gpu;

	EXPECT_FLOAT_EQ(gpu.BlurParams.x, settings.SSSBlurRadius);
	EXPECT_FLOAT_EQ(gpu.BlurParams.y, settings.SSSBlurFalloff);

	// Disabled by default
	EXPECT_FLOAT_EQ(gpu.Flags.x, 0.0f);
}

// =============================================================================
// UBO Binding Indices
// =============================================================================

TEST(ShaderBindingLayout, SnowAndSSSBindingsExist)
{
	EXPECT_EQ(ShaderBindingLayout::UBO_SNOW, 13u);
	EXPECT_EQ(ShaderBindingLayout::UBO_SSS, 14u);

	// Must not collide with existing bindings
	EXPECT_NE(ShaderBindingLayout::UBO_SNOW, ShaderBindingLayout::UBO_SSS);
	EXPECT_NE(ShaderBindingLayout::UBO_SNOW, ShaderBindingLayout::UBO_CAMERA);
	EXPECT_NE(ShaderBindingLayout::UBO_SNOW, ShaderBindingLayout::UBO_SHADOW);
	EXPECT_NE(ShaderBindingLayout::UBO_SNOW, ShaderBindingLayout::UBO_USER_0);
	EXPECT_NE(ShaderBindingLayout::UBO_SSS, ShaderBindingLayout::UBO_CAMERA);
	EXPECT_NE(ShaderBindingLayout::UBO_SSS, ShaderBindingLayout::UBO_SHADOW);
	EXPECT_NE(ShaderBindingLayout::UBO_SSS, ShaderBindingLayout::UBO_USER_0);
}
