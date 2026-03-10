#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Scene/Components.h"

#include <cstring>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// ShadowUBO — Regression: all i32 fields must default to 0
// Prevents accidental cascade-debug-visualization or wrong shadow state.
// =============================================================================

TEST(RenderingRegression, ShadowUBOIntFieldsDefaultZeroed)
{
	UBOStructures::ShadowUBO ubo{};
	EXPECT_EQ(ubo.DirectionalShadowEnabled, 0)
		<< "DirectionalShadowEnabled must default to 0";
	EXPECT_EQ(ubo.SpotShadowCount, 0)
		<< "SpotShadowCount must default to 0";
	EXPECT_EQ(ubo.PointShadowCount, 0)
		<< "PointShadowCount must default to 0";
	EXPECT_EQ(ubo.ShadowMapResolution, 0)
		<< "ShadowMapResolution must default to 0";
	EXPECT_EQ(ubo.CascadeDebugEnabled, 0)
		<< "CascadeDebugEnabled must default to 0 — non-zero causes"
		   " red/green/blue/yellow cascade tinting";
}

// =============================================================================
// WaterComponent — Regression: wavelength must be non-zero
// Zero wavelength causes sub-pixel wave noise (invisible waves).
// =============================================================================

TEST(RenderingRegression, WaterWavelengthMustBePositive)
{
	WaterComponent wc{};
	EXPECT_GT(wc.m_Wavelength0, 0.0f)
		<< "Wavelength0 must be > 0 for visible waves";
	EXPECT_GT(wc.m_Wavelength1, 0.0f)
		<< "Wavelength1 must be > 0 for visible waves";
}

TEST(RenderingRegression, WaterWavelengthCopied)
{
	WaterComponent src{};
	src.m_Wavelength0 = 25.0f;
	src.m_Wavelength1 = 35.0f;

	WaterComponent copied(src);
	EXPECT_FLOAT_EQ(copied.m_Wavelength0, 25.0f);
	EXPECT_FLOAT_EQ(copied.m_Wavelength1, 35.0f);

	WaterComponent assigned{};
	assigned = src;
	EXPECT_FLOAT_EQ(assigned.m_Wavelength0, 25.0f);
	EXPECT_FLOAT_EQ(assigned.m_Wavelength1, 35.0f);
}

// =============================================================================
// DirectionalLightComponent — Regression: cascade debug defaults to false
// =============================================================================

TEST(RenderingRegression, CascadeDebugDefaultDisabled)
{
	DirectionalLightComponent dlc{};
	EXPECT_FALSE(dlc.m_CascadeDebugVisualization)
		<< "CascadeDebugVisualization must default to false";
}

// =============================================================================
// PBR Material UBO — IBL enable flag is part of PBR UBO
// =============================================================================

TEST(RenderingRegression, PBRMaterialUBOHasIBLField)
{
	UBOStructures::PBRMaterialUBO ubo{};
	// The EnableIBL field must exist and default to 0 (disabled).
	EXPECT_EQ(ubo.EnableIBL, 0)
		<< "EnableIBL must exist in PBRMaterialUBO and default to 0";
	// IBLIntensity must default to 1.0 (full strength, no damping).
	EXPECT_FLOAT_EQ(ubo.IBLIntensity, 1.0f)
		<< "IBLIntensity must default to 1.0";
}
