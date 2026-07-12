// OLO_TEST_LAYER: integration
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/FluidRenderData.h"
#include "OloEngine/Renderer/FluidShading.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// FluidRenderUBO — Alignment & Size (issue #630, pillar B)
// =============================================================================

TEST(FluidRenderMath, FluidRenderUBOAlignment)
{
    EXPECT_EQ(sizeof(UBOStructures::FluidRenderUBO) % 16, 0u)
        << "FluidRenderUBO must be 16-byte aligned for std140";
}

TEST(FluidRenderMath, FluidRenderUBOSizeStable)
{
    // 5 x glm::vec4 + 1 x glm::uvec4 = 6 x 16 = 96 bytes — the GLSL twin in
    // include/FluidRenderCommon.glsl assumes exactly this layout.
    EXPECT_EQ(sizeof(UBOStructures::FluidRenderUBO), 96u);
}

TEST(FluidRenderMath, FluidRenderUBOGetSizeMatchesSizeof)
{
    EXPECT_EQ(UBOStructures::FluidRenderUBO::GetSize(), sizeof(UBOStructures::FluidRenderUBO));
}

TEST(FluidRenderMath, FluidRenderUBOFieldOffsets)
{
    static_assert(std::is_trivially_copyable_v<UBOStructures::FluidRenderUBO>);
    EXPECT_EQ(offsetof(UBOStructures::FluidRenderUBO, TintRadius), 0u);
    EXPECT_EQ(offsetof(UBOStructures::FluidRenderUBO, AbsorptionParams), 16u);
    EXPECT_EQ(offsetof(UBOStructures::FluidRenderUBO, FoamParams), 32u);
    EXPECT_EQ(offsetof(UBOStructures::FluidRenderUBO, SmoothParams), 48u);
    EXPECT_EQ(offsetof(UBOStructures::FluidRenderUBO, ScreenParams), 64u);
    EXPECT_EQ(offsetof(UBOStructures::FluidRenderUBO, Counts), 80u);
}

// =============================================================================
// Binding-point pins — ShaderBindingLayout must agree with the shaders
// =============================================================================

TEST(FluidRenderMath, FluidRenderUBOBindingSlot)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_FLUID_RENDER, 48u);
}

TEST(FluidRenderMath, FluidRenderUBOKnownBinding)
{
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_FLUID_RENDER, "FluidRenderUBO"));
    EXPECT_FALSE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_FLUID_RENDER, "WaterParams"));
}

TEST(FluidRenderMath, FluidTextureBindingSlots)
{
    EXPECT_EQ(ShaderBindingLayout::TEX_FLUID_DEPTH, 54u);
    EXPECT_EQ(ShaderBindingLayout::TEX_FLUID_THICKNESS, 55u);
}

TEST(FluidRenderMath, FluidTextureBindingsKnown)
{
    EXPECT_TRUE(ShaderBindingLayout::IsKnownTextureBinding(ShaderBindingLayout::TEX_FLUID_DEPTH, "u_FluidDepth"));
    EXPECT_TRUE(ShaderBindingLayout::IsKnownTextureBinding(ShaderBindingLayout::TEX_FLUID_THICKNESS, "u_FluidThickness"));
    EXPECT_FALSE(ShaderBindingLayout::IsKnownTextureBinding(ShaderBindingLayout::TEX_FLUID_DEPTH, "u_SceneDepth"));
}

TEST(FluidRenderMath, FluidSSBOBindingSlots)
{
    // The splat shaders (include/FluidSplatCommon.glsl) hard-code these.
    EXPECT_EQ(ShaderBindingLayout::SSBO_FLUID_POSITIONS, 21u);
    EXPECT_EQ(ShaderBindingLayout::SSBO_FLUID_VELOCITIES, 22u);
    EXPECT_EQ(ShaderBindingLayout::SSBO_FLUID_COUNTERS, 28u);
}

// =============================================================================
// FluidRenderData — POD contract
// =============================================================================

TEST(FluidRenderMath, FluidRenderDataTriviallyCopyable)
{
    static_assert(std::is_trivially_copyable_v<FluidRenderData>,
                  "FluidRenderData moves through per-frame draw lists by memcpy semantics");
    EXPECT_TRUE(std::is_trivially_copyable_v<FluidRenderData>);
}

TEST(FluidRenderMath, FluidRenderDataMemcpyRoundTrip)
{
    FluidRenderData source{};
    source.PositionsSSBOId = 101u;
    source.VelocitiesSSBOId = 102u;
    source.CountersSSBOId = 103u;
    source.ParticleUpperBound = 65536u;
    source.ParticleRadius = 0.075f;
    source.Tint = glm::vec3(0.2f, 0.55f, 0.9f);
    source.AbsorptionColor = glm::vec3(0.5f, 0.08f, 0.02f);
    source.AbsorptionScale = 1.5f;
    source.FoamSpeedThreshold = 2.5f;
    source.EntityID = 42;

    std::byte staging[sizeof(FluidRenderData)];
    std::memcpy(staging, &source, sizeof(FluidRenderData));

    FluidRenderData copy{};
    std::memcpy(&copy, staging, sizeof(FluidRenderData));

    EXPECT_EQ(std::memcmp(&source, &copy, sizeof(FluidRenderData)), 0)
        << "byte-exact round-trip through a staging buffer";
    EXPECT_EQ(copy.PositionsSSBOId, 101u);
    EXPECT_EQ(copy.ParticleUpperBound, 65536u);
    EXPECT_EQ(copy.EntityID, 42);
}

// =============================================================================
// FluidShading::Transmittance — Beer–Lambert contract
//
// Formula (Beer–Lambert law, I = I0 * exp(-alpha * d)):
//   T[c] = exp(-absorptionColor[c] * absorptionScale * thickness)
// GLSL mirror: OloEditor/assets/shaders/FluidComposite.glsl (fragment stage,
// "Beer–Lambert absorption") — keep formula-identical.
// =============================================================================

TEST(FluidRenderMath, TransmittanceFullAtZeroThickness)
{
    const glm::vec3 t = FluidShading::Transmittance(glm::vec3(0.45f, 0.06f, 0.01f), 1.0f, 0.0f);
    EXPECT_NEAR(t.x, 1.0f, 1.0e-6f);
    EXPECT_NEAR(t.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(t.z, 1.0f, 1.0e-6f);
}

TEST(FluidRenderMath, TransmittanceHalfLifePin)
{
    // exp(-1 * 1 * ln(2)) == 0.5 per channel.
    const f32 lnTwo = std::log(2.0f);
    const glm::vec3 t = FluidShading::Transmittance(glm::vec3(1.0f), 1.0f, lnTwo);
    EXPECT_NEAR(t.x, 0.5f, 1.0e-5f);
    EXPECT_NEAR(t.y, 0.5f, 1.0e-5f);
    EXPECT_NEAR(t.z, 0.5f, 1.0e-5f);
}

TEST(FluidRenderMath, TransmittanceMonotonicDecreasing)
{
    const glm::vec3 absorption(0.45f, 0.06f, 0.01f);
    constexpr f32 kThicknesses[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };

    glm::vec3 previous = FluidShading::Transmittance(absorption, 1.0f, 0.0f);
    for (const f32 thickness : kThicknesses)
    {
        const glm::vec3 current = FluidShading::Transmittance(absorption, 1.0f, thickness);
        EXPECT_LT(current.x, previous.x) << "thickness " << thickness;
        EXPECT_LT(current.y, previous.y) << "thickness " << thickness;
        EXPECT_LT(current.z, previous.z) << "thickness " << thickness;
        EXPECT_GT(current.x, 0.0f);
        EXPECT_GT(current.y, 0.0f);
        EXPECT_GT(current.z, 0.0f);
        previous = current;
    }
}

TEST(FluidRenderMath, TransmittanceChannelIndependence)
{
    // A zero-absorption channel transmits fully regardless of the others.
    const glm::vec3 redOnly = FluidShading::Transmittance(glm::vec3(0.9f, 0.0f, 0.0f), 1.0f, 3.0f);
    EXPECT_LT(redOnly.x, 1.0f);
    EXPECT_NEAR(redOnly.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(redOnly.z, 1.0f, 1.0e-6f);

    // Changing one channel's absorption must not perturb another channel.
    const glm::vec3 a = FluidShading::Transmittance(glm::vec3(0.5f, 0.1f, 0.2f), 1.0f, 2.0f);
    const glm::vec3 b = FluidShading::Transmittance(glm::vec3(0.5f, 0.9f, 0.2f), 1.0f, 2.0f);
    EXPECT_NEAR(a.x, b.x, 1.0e-6f);
    EXPECT_NEAR(a.z, b.z, 1.0e-6f);
    EXPECT_LT(b.y, a.y);
}

TEST(FluidRenderMath, TransmittanceNaNAndRangeGuards)
{
    const glm::vec3 absorption(0.45f, 0.06f, 0.01f);
    constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
    constexpr f32 kInf = std::numeric_limits<f32>::infinity();

    // Non-finite or non-positive thickness / scale — "no absorption", never NaN.
    for (const f32 badThickness : { kNaN, kInf, -kInf, -1.0f, 0.0f })
    {
        const glm::vec3 t = FluidShading::Transmittance(absorption, 1.0f, badThickness);
        EXPECT_NEAR(t.x, 1.0f, 1.0e-6f) << "thickness " << badThickness;
        EXPECT_NEAR(t.y, 1.0f, 1.0e-6f) << "thickness " << badThickness;
        EXPECT_NEAR(t.z, 1.0f, 1.0e-6f) << "thickness " << badThickness;
    }
    for (const f32 badScale : { kNaN, kInf, -1.0f, 0.0f })
    {
        const glm::vec3 t = FluidShading::Transmittance(absorption, badScale, 1.0f);
        EXPECT_NEAR(t.x, 1.0f, 1.0e-6f) << "scale " << badScale;
        EXPECT_NEAR(t.y, 1.0f, 1.0e-6f) << "scale " << badScale;
        EXPECT_NEAR(t.z, 1.0f, 1.0e-6f) << "scale " << badScale;
    }

    // A single non-finite / negative colour channel is neutralised; the
    // healthy channels still absorb.
    const glm::vec3 t = FluidShading::Transmittance(glm::vec3(kNaN, 0.5f, -2.0f), 1.0f, 2.0f);
    EXPECT_NEAR(t.x, 1.0f, 1.0e-6f);
    EXPECT_LT(t.y, 1.0f);
    EXPECT_TRUE(std::isfinite(t.y));
    EXPECT_NEAR(t.z, 1.0f, 1.0e-6f);
}
