#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderConstants.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

using namespace OloEngine;

// =============================================================================
// Cascade Split Math Tests
// =============================================================================
// The practical split scheme: C_i = lerp(near*(far/near)^(i/N), near+(far-near)*i/N, lambda)
// ShadowMap::ComputeCSMCascades uses this internally. We replicate the formula
// to verify the expected properties.

class CascadeSplitTest : public ::testing::Test
{
protected:
	// Replicates the practical split scheme from ShadowMap::ComputeCSMCascades
	static std::array<f32, 5> ComputeSplits(f32 nearPlane, f32 farPlane, f32 lambda, u32 cascades = 4)
	{
		std::array<f32, 5> splits{};
		splits[0] = nearPlane;
		for (u32 i = 1; i <= cascades; ++i)
		{
			const f32 p = static_cast<f32>(i) / static_cast<f32>(cascades);
			const f32 logSplit = nearPlane * std::pow(farPlane / nearPlane, p);
			const f32 uniformSplit = nearPlane + (farPlane - nearPlane) * p;
			splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
		}
		return splits;
	}
};

TEST_F(CascadeSplitTest, MonotonicallyIncreasing)
{
	const auto splits = ComputeSplits(0.1f, 200.0f, 0.5f);
	for (u32 i = 0; i < 4; ++i)
	{
		EXPECT_LT(splits[i], splits[i + 1])
			<< "Split " << i << " (" << splits[i] << ") should be less than split "
			<< (i + 1) << " (" << splits[i + 1] << ")";
	}
}

TEST_F(CascadeSplitTest, CoversNearFarRange)
{
	constexpr f32 nearPlane = 0.1f;
	constexpr f32 farPlane = 200.0f;
	const auto splits = ComputeSplits(nearPlane, farPlane, 0.5f);

	EXPECT_FLOAT_EQ(splits[0], nearPlane);
	EXPECT_FLOAT_EQ(splits[4], farPlane);
}

TEST_F(CascadeSplitTest, LambdaZeroIsUniform)
{
	constexpr f32 nearPlane = 0.1f;
	constexpr f32 farPlane = 100.0f;
	const auto splits = ComputeSplits(nearPlane, farPlane, 0.0f);

	// With lambda=0, splits should be uniformly distributed
	const f32 step = (farPlane - nearPlane) / 4.0f;
	for (u32 i = 0; i <= 4; ++i)
	{
		EXPECT_NEAR(splits[i], nearPlane + step * static_cast<f32>(i), 1e-4f)
			<< "Uniform split " << i << " mismatch";
	}
}

TEST_F(CascadeSplitTest, LambdaOneIsLogarithmic)
{
	constexpr f32 nearPlane = 1.0f; // Use nearPlane=1 to simplify log math
	constexpr f32 farPlane = 256.0f;
	const auto splits = ComputeSplits(nearPlane, farPlane, 1.0f);

	// With lambda=1, splits should follow: nearPlane * (farPlane/nearPlane)^(i/N)
	for (u32 i = 0; i <= 4; ++i)
	{
		const f32 p = static_cast<f32>(i) / 4.0f;
		const f32 expected = nearPlane * std::pow(farPlane / nearPlane, p);
		EXPECT_NEAR(splits[i], expected, 1e-3f)
			<< "Logarithmic split " << i << " mismatch";
	}
}

TEST_F(CascadeSplitTest, ClampedMaxShadowDistance)
{
	// When maxShadowDistance < cameraFar, effective far is clamped
	constexpr f32 nearPlane = 0.1f;
	constexpr f32 cameraFar = 1000.0f;
	constexpr f32 maxShadow = 200.0f;
	const f32 effectiveFar = (cameraFar < maxShadow) ? cameraFar : maxShadow;

	const auto splits = ComputeSplits(nearPlane, effectiveFar, 0.5f);
	EXPECT_FLOAT_EQ(splits[4], maxShadow);
	EXPECT_LT(splits[4], cameraFar);
}

TEST_F(CascadeSplitTest, AllSplitsPositive)
{
	const auto splits = ComputeSplits(0.01f, 500.0f, 0.5f);
	for (u32 i = 0; i <= 4; ++i)
	{
		EXPECT_GT(splits[i], 0.0f) << "Split " << i << " should be positive";
	}
}

TEST_F(CascadeSplitTest, DifferentLambdasProduceDifferentDistributions)
{
	const auto uniform = ComputeSplits(0.1f, 200.0f, 0.0f);
	const auto mixed = ComputeSplits(0.1f, 200.0f, 0.5f);
	const auto logarithmic = ComputeSplits(0.1f, 200.0f, 1.0f);

	// Middle splits should differ between distributions
	for (u32 i = 1; i < 4; ++i)
	{
		EXPECT_NE(uniform[i], logarithmic[i])
			<< "Uniform and logarithmic split " << i << " should differ";
	}

	// Mixed should be between uniform and logarithmic for each split
	for (u32 i = 1; i < 4; ++i)
	{
		const f32 lo = std::min(uniform[i], logarithmic[i]);
		const f32 hi = std::max(uniform[i], logarithmic[i]);
		EXPECT_GE(mixed[i], lo - 1e-5f);
		EXPECT_LE(mixed[i], hi + 1e-5f);
	}
}

// =============================================================================
// ShadowMap Light-Space Matrix Tests (no GL context needed)
// =============================================================================
// ComputeCSMCascades, SetSpotLightShadow, SetPointLightShadow are pure math
// on a default-constructed ShadowMap (Init not required for math-only paths).

class ShadowMapMatrixTest : public ::testing::Test
{
protected:
	ShadowMap shadowMap;

	void SetUp() override
	{
		ShadowSettings settings;
		settings.Resolution = 1024;
		settings.MaxShadowDistance = 200.0f;
		settings.CascadeSplitLambda = 0.5f;
		shadowMap.SetSettings(settings);
	}
};

TEST_F(ShadowMapMatrixTest, ComputeCSMCascadesProducesValidMatrices)
{
	const glm::vec3 lightDir(0.0f, -1.0f, 0.0f); // Straight down
	const glm::mat4 view = glm::lookAt(
		glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);

	shadowMap.ComputeCSMCascades(lightDir, view, proj, 0.1f, 1000.0f);

	// All 4 cascade matrices should be non-identity (i.e., computed)
	for (u32 i = 0; i < ShadowMap::MAX_CSM_CASCADES; ++i)
	{
		const glm::mat4& m = shadowMap.GetCSMMatrix(i);
		EXPECT_NE(m, glm::mat4(1.0f))
			<< "Cascade " << i << " matrix should not be identity";
	}
}

TEST_F(ShadowMapMatrixTest, CascadePlaneDistancesMonotonicallyIncrease)
{
	const glm::vec3 lightDir(-0.5f, -1.0f, -0.3f);
	const glm::mat4 view = glm::lookAt(
		glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);

	shadowMap.ComputeCSMCascades(lightDir, view, proj, 0.1f, 500.0f);

	const glm::vec4& distances = shadowMap.GetCascadePlaneDistances();
	EXPECT_GT(distances.x, 0.0f);
	EXPECT_GT(distances.y, distances.x);
	EXPECT_GT(distances.z, distances.y);
	EXPECT_GT(distances.w, distances.z);
}

TEST_F(ShadowMapMatrixTest, CascadeFarPlaneCappedByMaxShadowDistance)
{
	const glm::vec3 lightDir(0.0f, -1.0f, 0.0f);
	const glm::mat4 view = glm::lookAt(
		glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);

	shadowMap.ComputeCSMCascades(lightDir, view, proj, 0.1f, 1000.0f);

	// Last cascade far plane should not exceed MaxShadowDistance (200.0)
	const glm::vec4& distances = shadowMap.GetCascadePlaneDistances();
	EXPECT_LE(distances.w, shadowMap.GetSettings().MaxShadowDistance + 1e-3f);
}

TEST_F(ShadowMapMatrixTest, CSMMatricesProjectKnownPointToValidNDC)
{
	// Use a non-axis-aligned light direction to avoid lookAt degeneracy
	const glm::vec3 lightDir(-0.3f, -1.0f, -0.2f);
	const glm::vec3 camPos(0.0f, 5.0f, 10.0f);
	const glm::vec3 camTarget(0.0f, 5.0f, 0.0f); // Looking forward along -Z
	const glm::mat4 view = glm::lookAt(camPos, camTarget, glm::vec3(0, 1, 0));
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);

	shadowMap.ComputeCSMCascades(lightDir, view, proj, 0.1f, 500.0f);

	// Test point 5 units in front of camera along its view axis
	const glm::vec4 worldPoint(0.0f, 5.0f, 5.0f, 1.0f);
	bool projectedInRange = false;

	for (u32 i = 0; i < ShadowMap::MAX_CSM_CASCADES; ++i)
	{
		const glm::vec4 clipPos = shadowMap.GetCSMMatrix(i) * worldPoint;
		if (std::abs(clipPos.w) > 1e-6f)
		{
			const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
			if (ndc.x >= -1.0f && ndc.x <= 1.0f &&
				ndc.y >= -1.0f && ndc.y <= 1.0f &&
				ndc.z >= -1.0f && ndc.z <= 1.0f)
			{
				projectedInRange = true;
				break;
			}
		}
	}
	EXPECT_TRUE(projectedInRange)
		<< "A point in the camera frustum should project into valid NDC range in at least one cascade";
}

TEST_F(ShadowMapMatrixTest, SpotLightShadowProducesValidPerspectiveMatrix)
{
	const glm::vec3 position(5.0f, 10.0f, 5.0f);
	const glm::vec3 direction(0.0f, -1.0f, 0.0f);
	constexpr f32 outerCutoff = 30.0f; // degrees
	constexpr f32 range = 50.0f;

	shadowMap.SetSpotLightShadow(0, position, direction, outerCutoff, range);

	const glm::mat4& m = shadowMap.GetSpotMatrix(0);
	EXPECT_NE(m, glm::mat4(1.0f)) << "Spot matrix should not be identity";
	EXPECT_NE(m, glm::mat4(0.0f)) << "Spot matrix should not be zero";

	// A point directly below the spot light should project within NDC
	const glm::vec4 targetPoint(5.0f, 0.0f, 5.0f, 1.0f);
	const glm::vec4 clipPos = m * targetPoint;
	ASSERT_GT(std::abs(clipPos.w), 1e-6f);
	const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;

	EXPECT_GE(ndc.x, -1.0f);
	EXPECT_LE(ndc.x, 1.0f);
	EXPECT_GE(ndc.y, -1.0f);
	EXPECT_LE(ndc.y, 1.0f);
	EXPECT_GE(ndc.z, 0.0f);  // Depth should be positive
	EXPECT_LE(ndc.z, 1.0f);
}

TEST_F(ShadowMapMatrixTest, SpotLightIndexOutOfRangeIsIgnored)
{
	const glm::vec3 position(0.0f, 10.0f, 0.0f);
	const glm::vec3 direction(0.0f, -1.0f, 0.0f);

	// Set a valid spotlight first
	shadowMap.SetSpotLightShadow(0, position, direction, 30.0f, 50.0f);
	const glm::mat4 before = shadowMap.GetSpotMatrix(0);

	// Out-of-range index should not crash or change existing data
	shadowMap.SetSpotLightShadow(ShadowMap::MAX_SPOT_SHADOWS, position, direction, 30.0f, 50.0f);
	shadowMap.SetSpotLightShadow(ShadowMap::MAX_SPOT_SHADOWS + 1, position, direction, 30.0f, 50.0f);

	EXPECT_EQ(shadowMap.GetSpotMatrix(0), before) << "Valid data should be unchanged";
}

TEST_F(ShadowMapMatrixTest, PointLightProduces6FaceMatrices)
{
	const glm::vec3 position(0.0f, 5.0f, 0.0f);
	constexpr f32 range = 25.0f;

	shadowMap.SetPointLightShadow(0, position, range);

	// All 6 face matrices should be non-identity and distinct
	for (u32 face = 0; face < 6; ++face)
	{
		const glm::mat4& m = shadowMap.GetPointFaceMatrix(0, face);
		EXPECT_NE(m, glm::mat4(0.0f)) << "Face " << face << " matrix should not be zero";
		EXPECT_NE(m, glm::mat4(1.0f)) << "Face " << face << " matrix should not be identity";
	}

	// Each face matrix should be different from every other face
	for (u32 a = 0; a < 6; ++a)
	{
		for (u32 b = a + 1; b < 6; ++b)
		{
			EXPECT_NE(shadowMap.GetPointFaceMatrix(0, a), shadowMap.GetPointFaceMatrix(0, b))
				<< "Face " << a << " and face " << b << " matrices should differ";
		}
	}
}

TEST_F(ShadowMapMatrixTest, PointLightShadowParamsStorePositionAndRange)
{
	const glm::vec3 position(10.0f, 20.0f, 30.0f);
	constexpr f32 range = 42.0f;

	shadowMap.SetPointLightShadow(0, position, range);

	const glm::vec4& params = shadowMap.GetPointShadowParams(0);
	EXPECT_FLOAT_EQ(params.x, position.x);
	EXPECT_FLOAT_EQ(params.y, position.y);
	EXPECT_FLOAT_EQ(params.z, position.z);
	EXPECT_FLOAT_EQ(params.w, range);
}

TEST_F(ShadowMapMatrixTest, PointLightIndexOutOfRangeIsIgnored)
{
	const glm::vec3 pos(1.0f, 2.0f, 3.0f);
	shadowMap.SetPointLightShadow(0, pos, 10.0f);
	const glm::vec4 before = shadowMap.GetPointShadowParams(0);

	// Out-of-range should silently return
	shadowMap.SetPointLightShadow(ShadowMap::MAX_POINT_SHADOWS, pos, 10.0f);
	EXPECT_EQ(shadowMap.GetPointShadowParams(0), before);
}

TEST_F(ShadowMapMatrixTest, PointLightFaceMatricesProject90DegreeFOV)
{
	const glm::vec3 position(0.0f, 0.0f, 0.0f);
	constexpr f32 range = 25.0f;

	shadowMap.SetPointLightShadow(0, position, range);

	// A point on the +X axis should project to the center of face 0 (+X face)
	const glm::vec4 testPoint(10.0f, 0.0f, 0.0f, 1.0f);
	const glm::vec4 clipPos = shadowMap.GetPointFaceMatrix(0, 0) * testPoint;
	ASSERT_GT(std::abs(clipPos.w), 1e-6f);
	const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;

	// Should be roughly centered in X/Y
	EXPECT_NEAR(ndc.x, 0.0f, 0.15f);
	EXPECT_NEAR(ndc.y, 0.0f, 0.15f);
	// Depth should be in [0,1]
	EXPECT_GT(ndc.z, 0.0f);
	EXPECT_LT(ndc.z, 1.0f);
}

// =============================================================================
// ShadowMap Per-Frame State Tests
// =============================================================================

TEST_F(ShadowMapMatrixTest, BeginFrameResetsPerFrameState)
{
	shadowMap.SetDirectionalShadowEnabled(true);
	shadowMap.SetSpotShadowCount(3);
	shadowMap.SetPointShadowCount(2);

	shadowMap.BeginFrame();

	EXPECT_EQ(shadowMap.GetSpotShadowCount(), 0);
	EXPECT_EQ(shadowMap.GetPointShadowCount(), 0);
}

TEST_F(ShadowMapMatrixTest, CascadeDebugToggle)
{
	EXPECT_FALSE(shadowMap.IsCascadeDebugEnabled());

	shadowMap.SetCascadeDebugEnabled(true);
	EXPECT_TRUE(shadowMap.IsCascadeDebugEnabled());

	shadowMap.SetCascadeDebugEnabled(false);
	EXPECT_FALSE(shadowMap.IsCascadeDebugEnabled());
}

// =============================================================================
// ShadowSettings Tests
// =============================================================================

TEST(ShadowSettingsTest, DefaultValues)
{
	ShadowSettings settings;
	EXPECT_EQ(settings.Resolution, static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE));
	EXPECT_FLOAT_EQ(settings.Bias, ShaderConstants::SHADOW_BIAS);
	EXPECT_FLOAT_EQ(settings.NormalBias, 0.01f);
	EXPECT_FLOAT_EQ(settings.Softness, 1.0f);
	EXPECT_FLOAT_EQ(settings.MaxShadowDistance, 200.0f);
	EXPECT_FLOAT_EQ(settings.CascadeSplitLambda, 0.5f);
	EXPECT_TRUE(settings.Enabled);
}

TEST(ShadowSettingsTest, SetSettingsUpdatesValues)
{
	ShadowMap sm;
	ShadowSettings custom;
	custom.Resolution = 2048;
	custom.Bias = 0.01f;
	custom.MaxShadowDistance = 500.0f;
	custom.CascadeSplitLambda = 0.7f;

	sm.SetSettings(custom);

	EXPECT_EQ(sm.GetResolution(), 2048u);
	EXPECT_FLOAT_EQ(sm.GetSettings().Bias, 0.01f);
	EXPECT_FLOAT_EQ(sm.GetSettings().MaxShadowDistance, 500.0f);
	EXPECT_FLOAT_EQ(sm.GetSettings().CascadeSplitLambda, 0.7f);
}

TEST(ShadowSettingsTest, EnableDisableToggle)
{
	ShadowMap sm;
	EXPECT_TRUE(sm.IsEnabled()); // Default

	sm.SetEnabled(false);
	EXPECT_FALSE(sm.IsEnabled());

	sm.SetEnabled(true);
	EXPECT_TRUE(sm.IsEnabled());
}

// =============================================================================
// UBO Structure Layout Tests
// =============================================================================

TEST(ShadowUBOTest, StructSizeIsNonZero)
{
	EXPECT_GT(UBOStructures::ShadowUBO::GetSize(), 0u);
}

TEST(ShadowUBOTest, StructSizeMultipleOf16)
{
	// GPU UBOs typically require 16-byte alignment (std140 layout)
	EXPECT_EQ(UBOStructures::ShadowUBO::GetSize() % 16, 0u);
}

TEST(ShadowUBOTest, MaxConstants)
{
	EXPECT_EQ(UBOStructures::ShadowUBO::MAX_CSM_CASCADES, 4u);
	EXPECT_EQ(UBOStructures::ShadowUBO::MAX_SPOT_SHADOWS, 4u);
	EXPECT_EQ(UBOStructures::ShadowUBO::MAX_POINT_SHADOWS, 4u);
}

TEST(ShadowUBOTest, DefaultInitializationZeroed)
{
	UBOStructures::ShadowUBO ubo{};
	EXPECT_EQ(ubo.DirectionalShadowEnabled, 0);
	EXPECT_EQ(ubo.SpotShadowCount, 0);
	EXPECT_EQ(ubo.PointShadowCount, 0);
	EXPECT_EQ(ubo.ShadowMapResolution, 0);
	EXPECT_EQ(ubo.CascadeDebugEnabled, 0);
}

// =============================================================================
// Texture2DArray Type Tests (no GL context — interface/type checks only)
// =============================================================================

TEST(Texture2DArrayTest, IsRefCounted)
{
	static_assert(std::is_base_of_v<RefCounted, Texture2DArray>,
		"Texture2DArray must be RefCounted");
	SUCCEED();
}

TEST(Texture2DArrayTest, IsAbstract)
{
	static_assert(std::is_abstract_v<Texture2DArray>,
		"Texture2DArray must be abstract (platform-implemented)");
	SUCCEED();
}

TEST(Texture2DArraySpecificationTest, DefaultValues)
{
	Texture2DArraySpecification spec;
	EXPECT_EQ(spec.Width, 1024u);
	EXPECT_EQ(spec.Height, 1024u);
	EXPECT_EQ(spec.Layers, 1u);
	EXPECT_EQ(spec.Format, Texture2DArrayFormat::DEPTH_COMPONENT32F);
	EXPECT_FALSE(spec.DepthComparisonMode);
}

TEST(Texture2DArraySpecificationTest, FormatEnumValues)
{
	// Ensure all expected formats exist
	EXPECT_NE(Texture2DArrayFormat::DEPTH_COMPONENT32F, Texture2DArrayFormat::RGBA8);
	EXPECT_NE(Texture2DArrayFormat::RGBA16F, Texture2DArrayFormat::RGBA32F);
}

// =============================================================================
// ShaderBindingLayout Shadow Constants Tests
// =============================================================================

TEST(ShaderBindingLayoutTest, ShadowUBOBinding)
{
	EXPECT_EQ(ShaderBindingLayout::UBO_SHADOW, 6u);
}

TEST(ShaderBindingLayoutTest, ShadowTextureBindings)
{
	EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW, 8u);
	EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW_SPOT, 13u);
	EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW_POINT_0, 14u);
	EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW_POINT_1, 15u);
	EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW_POINT_2, 16u);
	EXPECT_EQ(ShaderBindingLayout::TEX_SHADOW_POINT_3, 17u);
}

TEST(ShaderBindingLayoutTest, ShadowBindingsDoNotConflict)
{
	// Shadow bindings should not overlap with other bindings
	EXPECT_NE(ShaderBindingLayout::UBO_SHADOW, ShaderBindingLayout::UBO_CAMERA);
	EXPECT_NE(ShaderBindingLayout::UBO_SHADOW, ShaderBindingLayout::UBO_LIGHTS);
	EXPECT_NE(ShaderBindingLayout::UBO_SHADOW, ShaderBindingLayout::UBO_MODEL);
	EXPECT_NE(ShaderBindingLayout::UBO_SHADOW, ShaderBindingLayout::UBO_MULTI_LIGHTS);

	EXPECT_NE(ShaderBindingLayout::TEX_SHADOW, ShaderBindingLayout::TEX_DIFFUSE);
	EXPECT_NE(ShaderBindingLayout::TEX_SHADOW, ShaderBindingLayout::TEX_NORMAL);
	EXPECT_NE(ShaderBindingLayout::TEX_SHADOW_SPOT, ShaderBindingLayout::TEX_SHADOW);
	EXPECT_NE(ShaderBindingLayout::TEX_SHADOW_POINT_0, ShaderBindingLayout::TEX_SHADOW_SPOT);
}
