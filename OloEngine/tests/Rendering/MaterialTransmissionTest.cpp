// OLO_TEST_LAYER: unit
//
// Contract test for the physical glTF material extensions (issue #970):
// KHR_materials_transmission, KHR_materials_ior and KHR_materials_volume.
//
// Two things are pinned here, and they are the two things a rendering change
// like this can get wrong without a single test going red:
//
//  1. THE NEUTRALITY OF THE DEFAULTS. The whole feature is built on the claim
//     that a material which never touches the new setters is byte-for-byte the
//     material it was before #970 existed -- so the shader skips the closure,
//     the derived extinction is exactly zero, and the IOR reproduces the F0 the
//     PBR shaders already hardcode. If any of that drifts, every legacy
//     material in every scene shifts by a hair, which is precisely the failure
//     mode that survives a screenshot review.
//
//  2. THE BEER-LAMBERT MATH. Material::GetAttenuationSigma is the production
//     derivation the GPU consumes; the shader only does exp(-sigma * thickness).
//     So the CPU side is where the spec formula can be held exactly, against an
//     independently-written reference, including the infinity and zero-channel
//     edges that would otherwise reach GLSL as a NaN.
//
// The GPU half of the story (that the shader actually applies this, on both
// backends) is TransmissionVisualEvidenceTest; this file owns the math and the
// data model.

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace OloEngine;

namespace
{
    constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();

    // KHR_materials_volume, written straight from the spec text rather than
    // from the implementation, so the two can disagree:
    //     transmittance = exp(log(attenuationColor) * thickness / attenuationDistance)
    glm::vec3 SpecTransmittance(const glm::vec3& attenuationColor, f32 attenuationDistance, f32 thickness)
    {
        glm::vec3 result(0.0f);
        for (int channel = 0; channel < 3; ++channel)
            result[channel] = std::exp(std::log(attenuationColor[channel]) * thickness / attenuationDistance);
        return result;
    }

    // What the shader does with the derived coefficient: exp(-sigma * thickness).
    glm::vec3 ShaderTransmittance(const glm::vec3& sigma, f32 thickness)
    {
        glm::vec3 result(0.0f);
        for (int channel = 0; channel < 3; ++channel)
            result[channel] = std::exp(-sigma[channel] * thickness);
        return result;
    }
} // namespace

// ============================================================================
// 1. Neutral defaults
// ============================================================================

TEST(MaterialTransmissionTest, DefaultsAreNeutralSoLegacyMaterialsAreUnchanged)
{
    const Material material;

    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 0.0f);
    EXPECT_FLOAT_EQ(material.GetThicknessFactor(), 0.0f);
    EXPECT_FLOAT_EQ(material.GetIOR(), kDefaultIOR);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().r, 1.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().g, 1.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().b, 1.0f);
    EXPECT_TRUE(std::isinf(material.GetAttenuationDistance()));
    EXPECT_GT(material.GetAttenuationDistance(), 0.0f) << "the default must be POSITIVE infinity";

    // The two gates the renderer gates on.
    EXPECT_FALSE(material.IsTransmissive()) << "the shader must skip the transmission closure entirely";
    EXPECT_FALSE(material.HasVolume());
}

TEST(MaterialTransmissionTest, DefaultSigmaIsExactlyZeroSoTransmittanceIsExactlyOne)
{
    const Material material;
    const glm::vec3 sigma = material.GetAttenuationSigma();

    // Exactly zero, not merely small: exp(-0 * d) is exactly 1.0 for every
    // thickness, which is what makes "no absorption" free of any drift.
    EXPECT_FLOAT_EQ(sigma.r, 0.0f);
    EXPECT_FLOAT_EQ(sigma.g, 0.0f);
    EXPECT_FLOAT_EQ(sigma.b, 0.0f);

    for (const f32 thickness : { 0.0f, 0.5f, 10.0f, 1000.0f })
    {
        const glm::vec3 transmittance = ShaderTransmittance(sigma, thickness);
        EXPECT_FLOAT_EQ(transmittance.r, 1.0f) << "thickness " << thickness;
        EXPECT_FLOAT_EQ(transmittance.g, 1.0f) << "thickness " << thickness;
        EXPECT_FLOAT_EQ(transmittance.b, 1.0f) << "thickness " << thickness;
    }
}

TEST(MaterialTransmissionTest, DefaultIorReproducesTheHardcodedDielectricF0)
{
    // The PBR shaders assume F0 = 0.04 for dielectrics. The glTF default IOR of
    // 1.5 is not merely "close" to that -- it is where 0.04 came from:
    //     ((1.5 - 1) / (1.5 + 1))^2 = (0.2)^2 = 0.04
    // It is asserted rather than trusted because the entire no-change claim
    // rests on it: a material at the default IOR reproduces the F0 the shaders
    // already assume, so nothing moves when the field is merely present.
    const f32 ratio = (kDefaultIOR - 1.0f) / (kDefaultIOR + 1.0f);
    EXPECT_FLOAT_EQ(ratio * ratio, 0.04f);
}

// ============================================================================
// 2. Beer-Lambert
// ============================================================================

TEST(MaterialTransmissionTest, DerivedSigmaReproducesTheSpecTransmittance)
{
    Material material;
    const glm::vec3 attenuationColor(0.9f, 0.4f, 0.15f);
    constexpr f32 attenuationDistance = 2.5f;
    material.SetAttenuationColor(attenuationColor);
    material.SetAttenuationDistance(attenuationDistance);

    const glm::vec3 sigma = material.GetAttenuationSigma();

    for (const f32 thickness : { 0.0f, 0.25f, 1.0f, 4.0f })
    {
        const glm::vec3 expected = SpecTransmittance(attenuationColor, attenuationDistance, thickness);
        const glm::vec3 actual = ShaderTransmittance(sigma, thickness);
        for (int channel = 0; channel < 3; ++channel)
        {
            EXPECT_NEAR(actual[channel], expected[channel], 1.0e-6f)
                << "channel " << channel << " at thickness " << thickness;
        }
    }
}

TEST(MaterialTransmissionTest, AttenuationIsDepthDependentAndPhysicallyBounded)
{
    Material material;
    material.SetAttenuationColor(glm::vec3(0.8f, 0.5f, 0.2f));
    material.SetAttenuationDistance(1.0f);
    const glm::vec3 sigma = material.GetAttenuationSigma();

    // Absorption must be a real function of depth -- this is the acceptance
    // criterion "attenuation is visibly depth-dependent where thickness is
    // available", held as a strict inequality rather than an eyeball.
    glm::vec3 previous = ShaderTransmittance(sigma, 0.0f);
    EXPECT_FLOAT_EQ(previous.r, 1.0f) << "thickness 0 is thin-walled: no absorption at all";

    for (const f32 thickness : { 0.5f, 1.0f, 2.0f, 8.0f })
    {
        const glm::vec3 current = ShaderTransmittance(sigma, thickness);
        for (int channel = 0; channel < 3; ++channel)
        {
            EXPECT_LT(current[channel], previous[channel]) << "channel " << channel << " at thickness " << thickness;
            // Bounded: a transmittance outside (0, 1] would either create
            // energy or go negative.
            EXPECT_GT(current[channel], 0.0f);
            EXPECT_LE(current[channel], 1.0f);
        }
        previous = current;
    }

    // A more absorbing channel must absorb faster: blue (0.2) below green (0.5)
    // below red (0.8) at the same depth. This is what makes coloured glass
    // colour-shift with thickness instead of merely darkening.
    const glm::vec3 atUnitDepth = ShaderTransmittance(sigma, 1.0f);
    EXPECT_LT(atUnitDepth.b, atUnitDepth.g);
    EXPECT_LT(atUnitDepth.g, atUnitDepth.r);
}

TEST(MaterialTransmissionTest, InfiniteAttenuationDistanceMeansNoAbsorptionEvenWhenTinted)
{
    Material material;
    material.SetAttenuationColor(glm::vec3(0.1f, 0.2f, 0.3f)); // strongly tinted
    material.SetAttenuationDistance(kInfinity);                // ... but infinitely far away

    const glm::vec3 sigma = material.GetAttenuationSigma();

    // finite / +inf is exactly 0 in IEEE-754, which is the whole reason the
    // derivation happens on the CPU: no infinity ever reaches GLSL, and no
    // inf * 0 NaN can be produced there.
    EXPECT_FLOAT_EQ(sigma.r, 0.0f);
    EXPECT_FLOAT_EQ(sigma.g, 0.0f);
    EXPECT_FLOAT_EQ(sigma.b, 0.0f);
}

TEST(MaterialTransmissionTest, SigmaIsAlwaysFiniteAndNonNegative)
{
    // The hostile-input sweep: every combination a malformed glTF or a
    // hand-edited YAML could present. None may produce a NaN, an infinity or a
    // negative coefficient, because all three would reach the material UBO.
    const glm::vec3 colors[] = {
        glm::vec3(1.0f),
        glm::vec3(0.0f),
        glm::vec3(-1.0f),
        glm::vec3(2.0f),
        glm::vec3(std::numeric_limits<f32>::quiet_NaN()),
        glm::vec3(kInfinity),
        glm::vec3(0.0f, 1.0f, 0.5f),
    };
    // The denormal entries are the ones that matter: -log(kMinAttenuationChannel)
    // is about 9.2, so dividing it by ~1e-38 overflows to +inf unless the setter
    // floors the distance — and an infinite sigma is a NaN at thickness 0.
    const f32 distances[] = { kInfinity,
                              1.0f,
                              0.0f,
                              -3.0f,
                              1.0e-6f,
                              1.0e-38f,
                              std::numeric_limits<f32>::denorm_min(),
                              std::numeric_limits<f32>::min(),
                              std::numeric_limits<f32>::quiet_NaN() };

    for (const glm::vec3& color : colors)
    {
        for (const f32 distance : distances)
        {
            Material material;
            material.SetAttenuationColor(color);
            material.SetAttenuationDistance(distance);

            const glm::vec3 sigma = material.GetAttenuationSigma();
            for (int channel = 0; channel < 3; ++channel)
            {
                EXPECT_TRUE(std::isfinite(sigma[channel]))
                    << "color " << color[channel] << " distance " << distance;
                EXPECT_GE(sigma[channel], 0.0f) << "color " << color[channel] << " distance " << distance;
            }

            // And the value the shader computes from it stays in (0, 1].
            for (const f32 thickness : { 0.0f, 1.0f, 100.0f })
            {
                const glm::vec3 transmittance = ShaderTransmittance(sigma, thickness);
                for (int channel = 0; channel < 3; ++channel)
                {
                    EXPECT_TRUE(std::isfinite(transmittance[channel]));
                    EXPECT_GE(transmittance[channel], 0.0f);
                    EXPECT_LE(transmittance[channel], 1.0f);
                }
            }
        }
    }
}

// ============================================================================
// 3. Setter sanitizing
// ============================================================================

TEST(MaterialTransmissionTest, TransmissionAndThicknessRejectNonFiniteAndOutOfRange)
{
    Material material;

    material.SetTransmissionFactor(std::numeric_limits<f32>::quiet_NaN());
    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 0.0f);

    material.SetTransmissionFactor(5.0f);
    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 1.0f) << "clamped, not rejected";

    material.SetTransmissionFactor(-1.0f);
    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 0.0f);

    material.SetTransmissionFactor(0.5f);
    EXPECT_TRUE(material.IsTransmissive());

    material.SetThicknessFactor(-2.0f);
    EXPECT_FLOAT_EQ(material.GetThicknessFactor(), 0.0f);
    EXPECT_FALSE(material.HasVolume());

    material.SetThicknessFactor(kInfinity);
    EXPECT_FLOAT_EQ(material.GetThicknessFactor(), 0.0f) << "an infinite thickness is not a volume, it is a bug";

    material.SetThicknessFactor(3.0f);
    EXPECT_TRUE(material.HasVolume());
}

TEST(MaterialTransmissionTest, IorRejectsTheNonsenseBandBelowOne)
{
    Material material;

    // glTF's explicit "no refraction" sentinel survives untouched.
    material.SetIOR(0.0f);
    EXPECT_FLOAT_EQ(material.GetIOR(), 0.0f);

    // Strictly between 0 and 1 is nonsense for a dielectric and would give a
    // negative F0, so it falls back to the default rather than being clamped
    // to 1.0 (which would silently mean "F0 = 0", i.e. no specular at all).
    material.SetIOR(0.5f);
    EXPECT_FLOAT_EQ(material.GetIOR(), kDefaultIOR);

    material.SetIOR(std::numeric_limits<f32>::quiet_NaN());
    EXPECT_FLOAT_EQ(material.GetIOR(), kDefaultIOR);

    material.SetIOR(1000.0f);
    EXPECT_FLOAT_EQ(material.GetIOR(), kMaxIOR);

    // An ordinary authored value is kept exactly.
    material.SetIOR(2.42f); // diamond
    EXPECT_FLOAT_EQ(material.GetIOR(), 2.42f);
}

TEST(MaterialTransmissionTest, AttenuationDistanceTreatsNonPositiveAsInfinite)
{
    Material material;

    material.SetAttenuationDistance(0.0f);
    EXPECT_TRUE(std::isinf(material.GetAttenuationDistance()));

    material.SetAttenuationDistance(-1.0f);
    EXPECT_TRUE(std::isinf(material.GetAttenuationDistance()));

    material.SetAttenuationDistance(std::numeric_limits<f32>::quiet_NaN());
    EXPECT_TRUE(std::isinf(material.GetAttenuationDistance()));

    // +infinity is accepted verbatim -- it is the glTF default, not an error.
    material.SetAttenuationDistance(kInfinity);
    EXPECT_TRUE(std::isinf(material.GetAttenuationDistance()));

    material.SetAttenuationDistance(4.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationDistance(), 4.0f);
}

TEST(MaterialTransmissionTest, AFiniteAttenuationDistanceIsFlooredSoSigmaCannotOverflow)
{
    // Rejecting only NaN and non-positive distances is not enough. A denormal
    // distance is finite, positive and passes every other check, but
    // -log(kMinAttenuationChannel) / 1e-38 overflows to +infinity — which then
    // reaches the material UBO and produces exp(-inf * 0) = NaN at thickness 0.
    Material material;
    material.SetAttenuationColor(glm::vec3(kMinAttenuationChannel));

    for (const f32 tiny : { 1.0e-38f, std::numeric_limits<f32>::denorm_min(), std::numeric_limits<f32>::min(), 0.5e-4f })
    {
        material.SetAttenuationDistance(tiny);
        EXPECT_GE(material.GetAttenuationDistance(), kMinAttenuationDistance) << "input " << tiny;

        const glm::vec3 sigma = material.GetAttenuationSigma();
        for (int channel = 0; channel < 3; ++channel)
        {
            EXPECT_TRUE(std::isfinite(sigma[channel])) << "input " << tiny;
            // ... and the value the shader derives from it stays a number.
            EXPECT_FALSE(std::isnan(std::exp(-sigma[channel] * 0.0f))) << "input " << tiny;
        }
    }

    // A distance at or above the floor is kept exactly.
    material.SetAttenuationDistance(1.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationDistance(), 1.0f);
}

TEST(MaterialTransmissionTest, AttenuationColorIsClampedAwayFromZero)
{
    Material material;

    // A channel of exactly 0 is legal glTF ("infinitely absorbing") but would
    // make log(0) = -inf and hence an infinite sigma, which is a NaN waiting to
    // happen at thickness 0. It is floored instead.
    material.SetAttenuationColor(glm::vec3(0.0f));
    EXPECT_GT(material.GetAttenuationColor().r, 0.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().r, kMinAttenuationChannel);

    material.SetAttenuationColor(glm::vec3(2.0f, -1.0f, 0.5f));
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().r, 1.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().g, kMinAttenuationChannel);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().b, 0.5f);
}

// ============================================================================
// 4. The GPU mirror
// ============================================================================

// The shader's Fresnel term, mirrored here so the ior < 1 case is pinned on the
// CPU too. glTF's "no refraction" sentinel is 0.0, and the naive formula turns it
// into a PERFECT MIRROR — F0 = ((0-1)/(0+1))^2 = 1 — which drives the transmission
// weight to zero and renders fully clear glass solid. oloIorToF0 returns 0 below
// 1.0 instead; this test states the same contract in a place CI always runs.
TEST(MaterialTransmissionTest, TheNoRefractionSentinelMustNotReadAsAMirror)
{
    const auto naiveF0 = [](f32 ior)
    {
        const f32 r = (ior - 1.0f) / (ior + 1.0f);
        return r * r;
    };
    // The trap, stated explicitly: this is what the shader must NOT do at ior 0.
    EXPECT_FLOAT_EQ(naiveF0(0.0f), 1.0f);

    // What oloIorToF0 does instead.
    const auto shaderF0 = [&naiveF0](f32 ior)
    { return ior < 1.0f ? 0.0f : naiveF0(ior); };
    EXPECT_FLOAT_EQ(shaderF0(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(shaderF0(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(shaderF0(kDefaultIOR), 0.04f);
    EXPECT_GT(shaderF0(2.42f), 0.04f);

    // And the sentinel survives the setter, so the shader really does see it.
    Material material;
    material.SetIOR(0.0f);
    EXPECT_FLOAT_EQ(material.GetIOR(), 0.0f);
}

TEST(MaterialTransmissionTest, MaterialUboCarriesThePhysicalBlockBeforeTheHeapOffsets)
{
    // The header static_asserts this too, so this test is the readable
    // statement of the same contract: the physical scalars sit between the
    // PBRModel selector and the trailing heap-offset lanes, and the block is
    // 176 bytes. Every .glsl mirroring PBRMaterialProperties depends on both.
    using UBO = ShaderBindingLayout::PBRMaterialUBO;

    EXPECT_EQ(sizeof(UBO), 176u);
    EXPECT_EQ(offsetof(UBO, TransmissionFactor), 96u);
    EXPECT_EQ(offsetof(UBO, IOR), 100u);
    EXPECT_EQ(offsetof(UBO, ThicknessFactor), 104u);
    EXPECT_EQ(offsetof(UBO, AttenuationSigmaR), 108u);
    EXPECT_EQ(offsetof(UBO, AttenuationSigmaG), 112u);
    EXPECT_EQ(offsetof(UBO, AttenuationSigmaB), 116u);
    EXPECT_EQ(offsetof(UBO, HeapOffsets), 128u) << "the heap-offset lanes must stay LAST (issue #691)";

    // A default-constructed UBO is neutral, which is what a draw that never
    // touched a physical material uploads.
    const UBO ubo{};
    EXPECT_FLOAT_EQ(ubo.TransmissionFactor, 0.0f);
    EXPECT_FLOAT_EQ(ubo.ThicknessFactor, 0.0f);
    EXPECT_FLOAT_EQ(ubo.AttenuationSigmaR, 0.0f);
    EXPECT_FLOAT_EQ(ubo.AttenuationSigmaG, 0.0f);
    EXPECT_FLOAT_EQ(ubo.AttenuationSigmaB, 0.0f);
    EXPECT_FLOAT_EQ(ubo.IOR, kDefaultIOR);
}
