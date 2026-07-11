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
    // Grew from 32 B with issue #435: ShadowAndAttenuation carries the base
    // shadow-atlas entry (x) and the quadratic attenuation coefficient (y)
    // so clustered shading matches the brute-force path's falloff.
    EXPECT_EQ(sizeof(GPUPointLight), 48u);
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
    // uvec4 Params + vec4 TileScale + vec4 DepthSlicing (clustered, #435)
    EXPECT_EQ(sizeof(UBOStructures::ForwardPlusUBO), 48u);
    EXPECT_EQ(sizeof(UBOStructures::ForwardPlusUBO) % 16, 0u); // std140
}

// =============================================================================
// LightGridConfig tests (clustered froxel grid, issue #435)
// =============================================================================

TEST(ForwardPlus, DefaultGridConfig)
{
    LightGridConfig config;
    EXPECT_EQ(config.ClusterCountX, ClusteredLighting::kClusterCountX);
    EXPECT_EQ(config.ClusterCountY, ClusteredLighting::kClusterCountY);
    EXPECT_EQ(config.ClusterCountZ, ClusteredLighting::kClusterCountZ);
    EXPECT_EQ(config.MaxLightsPerCluster, ClusteredLighting::kMaxLightsPerCluster);

    // The compute shader's shared-memory array (MAX_SHARED_LIGHTS = 256 in
    // LightCulling.comp) is the hard per-cluster cap; the config default must
    // not exceed it or overflowing lights would be silently dropped
    // inconsistently between the count and the written indices.
    EXPECT_LE(config.MaxLightsPerCluster, 256u);
}

TEST(ForwardPlus, ClusterGridTotalMatchesDimensions)
{
    LightGridConfig config;
    const u32 total = config.ClusterCountX * config.ClusterCountY * config.ClusterCountZ;
    EXPECT_EQ(total, ClusteredLighting::kTotalClusters);
    // GPU memory sanity: the fixed-count index list must stay well under
    // typical SSBO budgets (kTotalClusters * kMaxLightsPerCluster * 4 B).
    const u64 indexBytes = static_cast<u64>(total) * config.MaxLightsPerCluster * sizeof(u32);
    EXPECT_LT(indexBytes, 64ull * 1024ull * 1024ull);
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
    EXPECT_FLOAT_EQ(sl.SpotParams.y, 2.0f);        // quadratic attenuation coefficient
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
