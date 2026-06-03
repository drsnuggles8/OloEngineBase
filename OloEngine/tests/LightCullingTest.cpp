#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/LightCulling/TiledForwardPlus.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// GPU struct layout tests (must match GLSL std430)
// =============================================================================

TEST(ForwardPlus, GPUPointLightSize)
{
    EXPECT_EQ(sizeof(GPUPointLight), 32u);
    EXPECT_EQ(sizeof(GPUPointLight) % 16, 0u); // vec4-aligned
}

TEST(ForwardPlus, GPUSpotLightSize)
{
    EXPECT_EQ(sizeof(GPUSpotLight), 64u);
    EXPECT_EQ(sizeof(GPUSpotLight) % 16, 0u);
}

TEST(ForwardPlus, GPUSphereAreaLightSize)
{
    EXPECT_EQ(sizeof(GPUSphereAreaLight), 48u);
    EXPECT_EQ(sizeof(GPUSphereAreaLight) % 16, 0u); // vec4-aligned, std430-compatible
}

TEST(ForwardPlus, ForwardPlusUBOSize)
{
    EXPECT_EQ(sizeof(UBOStructures::ForwardPlusUBO), 16u);
    EXPECT_EQ(sizeof(UBOStructures::ForwardPlusUBO) % 16, 0u); // std140
}

// =============================================================================
// LightGridConfig tests
// =============================================================================

TEST(ForwardPlus, DefaultGridConfig)
{
    LightGridConfig config;
    EXPECT_EQ(config.TileSizePixels, 16u);
    EXPECT_EQ(config.MaxLightsPerTile, 256u);
}

TEST(ForwardPlus, TileCountCalculation)
{
    // 1920x1080 with 16px tiles
    u32 width = 1920;
    u32 height = 1080;
    u32 tileSize = 16;

    u32 tileCountX = (width + tileSize - 1) / tileSize;
    u32 tileCountY = (height + tileSize - 1) / tileSize;

    EXPECT_EQ(tileCountX, 120u);
    EXPECT_EQ(tileCountY, 68u); // ceil(1080/16) = 67.5 -> 68
}

TEST(ForwardPlus, TileCountCalculationNonMultiple)
{
    // 1366x768 with 16px tiles (non-power-of-two resolution)
    u32 width = 1366;
    u32 height = 768;
    u32 tileSize = 16;

    u32 tileCountX = (width + tileSize - 1) / tileSize;
    u32 tileCountY = (height + tileSize - 1) / tileSize;

    EXPECT_EQ(tileCountX, 86u); // ceil(1366/16) = 85.375 -> 86
    EXPECT_EQ(tileCountY, 48u); // 768/16 = 48 exact
}

TEST(ForwardPlus, TileCountWith32pxTiles)
{
    u32 width = 1920;
    u32 height = 1080;
    u32 tileSize = 32;

    u32 tileCountX = (width + tileSize - 1) / tileSize;
    u32 tileCountY = (height + tileSize - 1) / tileSize;

    EXPECT_EQ(tileCountX, 60u);
    EXPECT_EQ(tileCountY, 34u); // ceil(1080/32) = 33.75 -> 34
}

// =============================================================================
// Binding constants tests (no conflicts)
// =============================================================================

TEST(ForwardPlus, SSBOBindingsDontConflict)
{
    // All Forward+ SSBO bindings must be unique
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_GLOBAL_INDEX);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_SPHERE_AREA_LIGHTS);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_SPHERE_AREA_LIGHTS);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES, ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES, ShaderBindingLayout::SSBO_FPLUS_SPHERE_AREA_LIGHTS);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID, ShaderBindingLayout::SSBO_FPLUS_SPHERE_AREA_LIGHTS);
    EXPECT_NE(ShaderBindingLayout::SSBO_FPLUS_GLOBAL_INDEX, ShaderBindingLayout::SSBO_FPLUS_SPHERE_AREA_LIGHTS);
}

TEST(ForwardPlus, UBOBindingIsUnique)
{
    // Forward+ UBO must not collide with other well-known bindings
    EXPECT_NE(ShaderBindingLayout::UBO_FORWARD_PLUS, ShaderBindingLayout::UBO_CAMERA);
    EXPECT_NE(ShaderBindingLayout::UBO_FORWARD_PLUS, ShaderBindingLayout::UBO_MATERIAL);
    EXPECT_NE(ShaderBindingLayout::UBO_FORWARD_PLUS, ShaderBindingLayout::UBO_MODEL);
    EXPECT_NE(ShaderBindingLayout::UBO_FORWARD_PLUS, ShaderBindingLayout::UBO_MULTI_LIGHTS);
    EXPECT_NE(ShaderBindingLayout::UBO_FORWARD_PLUS, ShaderBindingLayout::UBO_SHADOW);
}

// =============================================================================
// ForwardPlusMode tests
// =============================================================================

TEST(ForwardPlus, ModeEnumValues)
{
    EXPECT_EQ(static_cast<int>(ForwardPlusMode::Auto), 0);
    EXPECT_EQ(static_cast<int>(ForwardPlusMode::Always), 1);
    EXPECT_EQ(static_cast<int>(ForwardPlusMode::Never), 2);
}

// =============================================================================
// GPUPointLight packing tests
// =============================================================================

TEST(ForwardPlus, PointLightPacking)
{
    GPUPointLight pl;
    pl.PositionAndRadius = glm::vec4(1.0f, 2.0f, 3.0f, 10.0f);
    pl.ColorAndIntensity = glm::vec4(0.5f, 0.5f, 0.5f, 2.0f);

    EXPECT_FLOAT_EQ(pl.PositionAndRadius.x, 1.0f);
    EXPECT_FLOAT_EQ(pl.PositionAndRadius.y, 2.0f);
    EXPECT_FLOAT_EQ(pl.PositionAndRadius.z, 3.0f);
    EXPECT_FLOAT_EQ(pl.PositionAndRadius.w, 10.0f); // range
    EXPECT_FLOAT_EQ(pl.ColorAndIntensity.w, 2.0f);  // intensity
}

TEST(ForwardPlus, SpotLightPacking)
{
    GPUSpotLight sl;
    sl.PositionAndRadius = glm::vec4(0.0f, 5.0f, 0.0f, 15.0f);
    sl.DirectionAndAngle = glm::vec4(0.0f, -1.0f, 0.0f, 0.9f);
    sl.ColorAndIntensity = glm::vec4(1.0f, 1.0f, 1.0f, 3.0f);
    sl.SpotParams = glm::vec4(0.95f, 2.0f, 0.0f, 0.0f);

    EXPECT_FLOAT_EQ(sl.DirectionAndAngle.w, 0.9f); // cos(outerAngle)
    EXPECT_FLOAT_EQ(sl.SpotParams.x, 0.95f);       // cos(innerAngle)
    EXPECT_FLOAT_EQ(sl.SpotParams.y, 2.0f);        // falloff
}

TEST(ForwardPlus, SphereAreaLightPacking)
{
    GPUSphereAreaLight al;
    al.PositionAndRadius = glm::vec4(7.0f, -3.0f, 2.0f, 0.75f);
    al.ColorAndIntensity = glm::vec4(0.1f, 0.2f, 0.4f, 5.0f);
    al.RangeAndPadding = glm::vec4(20.0f, 0.0f, 0.0f, 0.0f);

    EXPECT_FLOAT_EQ(al.PositionAndRadius.x, 7.0f);
    EXPECT_FLOAT_EQ(al.PositionAndRadius.w, 0.75f); // emitter radius
    EXPECT_FLOAT_EQ(al.ColorAndIntensity.w, 5.0f);  // intensity
    EXPECT_FLOAT_EQ(al.RangeAndPadding.x, 20.0f);   // range
}

// Light-index encoding contract: the LightCulling.comp packs a 2-bit type tag
// in the top bits of each index. Decoder lives in ForwardPlusCommon.glsl.
// Pinning the bit layout here so a future shader edit that breaks it shows up
// as a test failure rather than as silently mis-shaded lights. Uses the
// canonical OloEngine::ForwardPlusLightIndex constants so the test validates
// the real shared contract, not a local copy.
TEST(ForwardPlus, LightIndexEncodingLayout)
{
    using namespace OloEngine::ForwardPlusLightIndex;

    // Tags must be distinct and within the top 2 bits
    EXPECT_EQ(TYPE_TAG_POINT & TYPE_TAG_MASK, TYPE_TAG_POINT);
    EXPECT_EQ(TYPE_TAG_SPHERE_AREA & TYPE_TAG_MASK, TYPE_TAG_SPHERE_AREA);
    EXPECT_EQ(TYPE_TAG_SPOT & TYPE_TAG_MASK, TYPE_TAG_SPOT);
    EXPECT_NE(TYPE_TAG_POINT, TYPE_TAG_SPHERE_AREA);
    EXPECT_NE(TYPE_TAG_POINT, TYPE_TAG_SPOT);
    EXPECT_NE(TYPE_TAG_SPHERE_AREA, TYPE_TAG_SPOT);

    // Round-trip: encode then decode
    const u32 idx = 42u;
    const u32 packedSpot = TYPE_TAG_SPOT | idx;
    const u32 packedArea = TYPE_TAG_SPHERE_AREA | idx;
    const u32 packedPoint = TYPE_TAG_POINT | idx;
    EXPECT_EQ(packedSpot & INDEX_MASK, idx);
    EXPECT_EQ(packedArea & INDEX_MASK, idx);
    EXPECT_EQ(packedPoint & INDEX_MASK, idx);

    // The legacy single-bit spot encoding (0x80000000) must still decode to
    // the spot type tag — required for backward compatibility with shaders
    // that haven't been recompiled.
    EXPECT_EQ(0x80000000u, TYPE_TAG_SPOT);

    // Max usable index = 2^30 - 1 (≈ 1 billion). Comfortable for any scene.
    EXPECT_EQ(INDEX_MASK, (1u << TYPE_TAG_SHIFT) - 1u);
}
