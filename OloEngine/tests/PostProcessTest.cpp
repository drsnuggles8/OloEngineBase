#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <cstring>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// PostProcessSettings Defaults
// =============================================================================

TEST(PostProcessSettings, DefaultsAreReasonable)
{
	PostProcessSettings pp;

	// Tone mapping defaults
	EXPECT_EQ(pp.Tonemap, TonemapOperator::Reinhard);
	EXPECT_FLOAT_EQ(pp.Exposure, 1.0f);
	EXPECT_FLOAT_EQ(pp.Gamma, 2.2f);

	// All optional effects default to disabled
	EXPECT_FALSE(pp.BloomEnabled);
	EXPECT_FALSE(pp.VignetteEnabled);
	EXPECT_FALSE(pp.ChromaticAberrationEnabled);
	EXPECT_FALSE(pp.FXAAEnabled);
	EXPECT_FALSE(pp.DOFEnabled);
	EXPECT_FALSE(pp.MotionBlurEnabled);
	EXPECT_FALSE(pp.ColorGradingEnabled);
}

TEST(PostProcessSettings, BloomParameterRanges)
{
	PostProcessSettings pp;

	EXPECT_GT(pp.BloomThreshold, 0.0f);
	EXPECT_GT(pp.BloomIntensity, 0.0f);
	EXPECT_GT(pp.BloomIterations, 0);
}

TEST(PostProcessSettings, DOFParameterRanges)
{
	PostProcessSettings pp;

	EXPECT_GT(pp.DOFFocusDistance, 0.0f);
	EXPECT_GT(pp.DOFFocusRange, 0.0f);
	EXPECT_GT(pp.DOFBokehRadius, 0.0f);
}

TEST(PostProcessSettings, MotionBlurParameterRanges)
{
	PostProcessSettings pp;

	EXPECT_GT(pp.MotionBlurStrength, 0.0f);
	EXPECT_GE(pp.MotionBlurSamples, 1);
}

// =============================================================================
// PostProcessUBOData Layout (std140 alignment)
// =============================================================================

TEST(PostProcessUBOData, SizeIs80Bytes)
{
	// The UBO is 20 floats/ints = 80 bytes, matching the GLSL layout
	EXPECT_EQ(PostProcessUBOData::GetSize(), 80u);
	EXPECT_EQ(sizeof(PostProcessUBOData), 80u);
}

TEST(PostProcessUBOData, DefaultsMatchSettings)
{
	PostProcessSettings settings;
	PostProcessUBOData gpu;

	EXPECT_EQ(gpu.TonemapOperator, static_cast<i32>(settings.Tonemap));
	EXPECT_FLOAT_EQ(gpu.Exposure, settings.Exposure);
	EXPECT_FLOAT_EQ(gpu.Gamma, settings.Gamma);
	EXPECT_FLOAT_EQ(gpu.BloomThreshold, settings.BloomThreshold);
	EXPECT_FLOAT_EQ(gpu.BloomIntensity, settings.BloomIntensity);
	EXPECT_FLOAT_EQ(gpu.VignetteIntensity, settings.VignetteIntensity);
	EXPECT_FLOAT_EQ(gpu.VignetteSmoothness, settings.VignetteSmoothness);
	EXPECT_FLOAT_EQ(gpu.ChromaticAberrationIntensity, settings.ChromaticAberrationIntensity);
	EXPECT_FLOAT_EQ(gpu.DOFFocusDistance, settings.DOFFocusDistance);
	EXPECT_FLOAT_EQ(gpu.DOFFocusRange, settings.DOFFocusRange);
	EXPECT_FLOAT_EQ(gpu.DOFBokehRadius, settings.DOFBokehRadius);
	EXPECT_FLOAT_EQ(gpu.MotionBlurStrength, settings.MotionBlurStrength);
	EXPECT_EQ(gpu.MotionBlurSamples, settings.MotionBlurSamples);
}

TEST(PostProcessUBOData, FieldOffsets_Std140Compatible)
{
	// Verify field offsets match the expected std140 layout
	// Each row is 16 bytes (4 floats). The layout is:
	// Row 0: TonemapOperator(i32), Exposure(f32), Gamma(f32), BloomThreshold(f32)
	// Row 1: BloomIntensity, VignetteIntensity, VignetteSmoothness, ChromAbIntensity
	// Row 2: DOFFocusDist, DOFFocusRange, DOFBokehRadius, MotionBlurStrength
	// Row 3: MotionBlurSamples(i32), InvScreenW, InvScreenH, _padding0
	// Row 4: TexelSizeX, TexelSizeY, CameraNear, CameraFar

	EXPECT_EQ(offsetof(PostProcessUBOData, TonemapOperator), 0u);
	EXPECT_EQ(offsetof(PostProcessUBOData, Exposure), 4u);
	EXPECT_EQ(offsetof(PostProcessUBOData, Gamma), 8u);
	EXPECT_EQ(offsetof(PostProcessUBOData, BloomThreshold), 12u);

	EXPECT_EQ(offsetof(PostProcessUBOData, BloomIntensity), 16u);
	EXPECT_EQ(offsetof(PostProcessUBOData, VignetteIntensity), 20u);
	EXPECT_EQ(offsetof(PostProcessUBOData, VignetteSmoothness), 24u);
	EXPECT_EQ(offsetof(PostProcessUBOData, ChromaticAberrationIntensity), 28u);

	EXPECT_EQ(offsetof(PostProcessUBOData, DOFFocusDistance), 32u);
	EXPECT_EQ(offsetof(PostProcessUBOData, DOFFocusRange), 36u);
	EXPECT_EQ(offsetof(PostProcessUBOData, DOFBokehRadius), 40u);
	EXPECT_EQ(offsetof(PostProcessUBOData, MotionBlurStrength), 44u);

	EXPECT_EQ(offsetof(PostProcessUBOData, MotionBlurSamples), 48u);
	EXPECT_EQ(offsetof(PostProcessUBOData, InverseScreenWidth), 52u);
	EXPECT_EQ(offsetof(PostProcessUBOData, InverseScreenHeight), 56u);

	EXPECT_EQ(offsetof(PostProcessUBOData, TexelSizeX), 64u);
	EXPECT_EQ(offsetof(PostProcessUBOData, TexelSizeY), 68u);
	EXPECT_EQ(offsetof(PostProcessUBOData, CameraNear), 72u);
	EXPECT_EQ(offsetof(PostProcessUBOData, CameraFar), 76u);
}

// =============================================================================
// MotionBlurUBOData Layout
// =============================================================================

TEST(MotionBlurUBOData, SizeIs128Bytes)
{
	// Two mat4s = 2 * 64 = 128 bytes
	EXPECT_EQ(MotionBlurUBOData::GetSize(), 128u);
	EXPECT_EQ(sizeof(MotionBlurUBOData), 128u);
}

TEST(MotionBlurUBOData, DefaultsAreIdentityMatrices)
{
	MotionBlurUBOData mb;

	EXPECT_EQ(mb.InverseViewProjection, glm::mat4(1.0f));
	EXPECT_EQ(mb.PrevViewProjection, glm::mat4(1.0f));
}

// =============================================================================
// ShadowUBO Layout Consistency
// =============================================================================

TEST(ShadowUBO, SizeConsistency)
{
	// ShadowUBO should be a specific known size so GLSL declarations match
	using ShadowUBO = ShaderBindingLayout::ShadowUBO;

	// 4 mat4s (cascades) + vec4 (cascade distances) + vec4 (params)
	// + 4 mat4s (spot) + 4 vec4s (point params)
	// + 4 ints + 4 ints (debug + padding)
	constexpr u32 expectedSize =
		4 * sizeof(glm::mat4)  // DirectionalLightSpaceMatrices
		+ sizeof(glm::vec4)    // CascadePlaneDistances
		+ sizeof(glm::vec4)    // ShadowParams
		+ 4 * sizeof(glm::mat4) // SpotLightSpaceMatrices
		+ 4 * sizeof(glm::vec4) // PointLightShadowParams
		+ 4 * sizeof(i32)       // DirectionalShadowEnabled, SpotShadowCount, PointShadowCount, ShadowMapResolution
		+ 4 * sizeof(i32);      // CascadeDebugEnabled + 3 padding

	EXPECT_EQ(ShadowUBO::GetSize(), expectedSize);
}

TEST(ShadowUBO, FieldLayout)
{
	using ShadowUBO = ShaderBindingLayout::ShadowUBO;

	// CascadeDebugEnabled must exist and be after ShadowMapResolution
	ShadowUBO ubo{};
	ubo.CascadeDebugEnabled = 1;
	EXPECT_EQ(ubo.CascadeDebugEnabled, 1);

	// Verify the debug field is at the expected offset
	constexpr u32 offsetAfterResolution =
		4 * sizeof(glm::mat4)   // DirectionalLightSpaceMatrices
		+ sizeof(glm::vec4)     // CascadePlaneDistances
		+ sizeof(glm::vec4)     // ShadowParams
		+ 4 * sizeof(glm::mat4) // SpotLightSpaceMatrices
		+ 4 * sizeof(glm::vec4) // PointLightShadowParams
		+ 4 * sizeof(i32);      // 4 ints (Enabled, SpotCount, PointCount, Resolution)

	EXPECT_EQ(offsetof(ShadowUBO, CascadeDebugEnabled), offsetAfterResolution);
}

// =============================================================================
// TonemapOperator enum values match GLSL defines
// =============================================================================

TEST(TonemapOperator, ValuesMatchGLSLDefines)
{
	// These must match the #define values in PBRCommon.glsl
	EXPECT_EQ(static_cast<i32>(TonemapOperator::None), 0);
	EXPECT_EQ(static_cast<i32>(TonemapOperator::Reinhard), 1);
	EXPECT_EQ(static_cast<i32>(TonemapOperator::ACES), 2);
	EXPECT_EQ(static_cast<i32>(TonemapOperator::Uncharted2), 3);
}

// =============================================================================
// PostProcess UBO binding slot consistency
// =============================================================================

TEST(ShaderBindingLayout, PostProcessUBOSlot)
{
	// PostProcess UBO uses binding 7 (UBO_USER_0)
	EXPECT_EQ(ShaderBindingLayout::UBO_USER_0, 7);
}

TEST(ShaderBindingLayout, PostProcessTextureSlots)
{
	// LUT texture at slot 18, depth at slot 19
	EXPECT_EQ(ShaderBindingLayout::TEX_POSTPROCESS_LUT, 18);
	EXPECT_EQ(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, 19);
}

TEST(ShaderBindingLayout, MotionBlurUBOSlot)
{
	// Motion blur UBO uses binding 8 (UBO_USER_1)
	EXPECT_EQ(ShaderBindingLayout::UBO_USER_1, 8);
}
