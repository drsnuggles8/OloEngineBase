#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

// ── SHCoefficients basic operations ──

TEST(SphericalHarmonicsTest, ZeroClearsAllCoefficients)
{
    SHCoefficients sh;
    sh.Coefficients[0] = glm::vec3(1.0f, 2.0f, 3.0f);
    sh.Coefficients[4] = glm::vec3(5.0f);

    sh.Zero();

    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_FLOAT_EQ(sh.Coefficients[i].x, 0.0f);
        EXPECT_FLOAT_EQ(sh.Coefficients[i].y, 0.0f);
        EXPECT_FLOAT_EQ(sh.Coefficients[i].z, 0.0f);
    }
}

TEST(SphericalHarmonicsTest, AccumulateAddsCoefficients)
{
    SHCoefficients a;
    a.Zero();
    a.Coefficients[0] = glm::vec3(1.0f, 0.0f, 0.0f);
    a.Coefficients[1] = glm::vec3(0.0f, 2.0f, 0.0f);

    SHCoefficients b;
    b.Zero();
    b.Coefficients[0] = glm::vec3(0.5f, 1.0f, 0.0f);
    b.Coefficients[1] = glm::vec3(0.0f, 0.5f, 3.0f);

    a.Accumulate(b);

    EXPECT_FLOAT_EQ(a.Coefficients[0].x, 1.5f);
    EXPECT_FLOAT_EQ(a.Coefficients[0].y, 1.0f);
    EXPECT_FLOAT_EQ(a.Coefficients[1].y, 2.5f);
    EXPECT_FLOAT_EQ(a.Coefficients[1].z, 3.0f);
}

TEST(SphericalHarmonicsTest, ScaleMultipliesAllCoefficients)
{
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(2.0f, 4.0f, 6.0f);
    sh.Coefficients[3] = glm::vec3(1.0f, 1.0f, 1.0f);

    sh.Scale(0.5f);

    EXPECT_FLOAT_EQ(sh.Coefficients[0].x, 1.0f);
    EXPECT_FLOAT_EQ(sh.Coefficients[0].y, 2.0f);
    EXPECT_FLOAT_EQ(sh.Coefficients[0].z, 3.0f);
    EXPECT_FLOAT_EQ(sh.Coefficients[3].x, 0.5f);
}

// ── GPU Layout roundtrip ──

TEST(SphericalHarmonicsTest, GPULayoutRoundtripPreservesData)
{
    SHCoefficients original;
    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        original.Coefficients[i] = glm::vec3(
            static_cast<f32>(i) * 0.1f,
            static_cast<f32>(i) * 0.2f,
            static_cast<f32>(i) * 0.3f);
    }

    std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};
    original.ToGPULayout(gpuData, 1.0f);

    SHCoefficients restored;
    restored.FromGPULayout(gpuData);

    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_FLOAT_EQ(restored.Coefficients[i].x, original.Coefficients[i].x);
        EXPECT_FLOAT_EQ(restored.Coefficients[i].y, original.Coefficients[i].y);
        EXPECT_FLOAT_EQ(restored.Coefficients[i].z, original.Coefficients[i].z);
    }
}

TEST(SphericalHarmonicsTest, GPULayoutValidityFlag)
{
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(1.0f, 2.0f, 3.0f);

    std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};

    sh.ToGPULayout(gpuData, 1.0f);
    EXPECT_FLOAT_EQ(gpuData[0].w, 1.0f);

    sh.ToGPULayout(gpuData, 0.0f);
    EXPECT_FLOAT_EQ(gpuData[0].w, 0.0f);

    // RGB data preserved regardless of validity flag
    EXPECT_FLOAT_EQ(gpuData[0].x, 1.0f);
    EXPECT_FLOAT_EQ(gpuData[0].y, 2.0f);
    EXPECT_FLOAT_EQ(gpuData[0].z, 3.0f);
}

TEST(SphericalHarmonicsTest, GPULayoutUnusedWComponentsAreZero)
{
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[2] = glm::vec3(5.0f, 6.0f, 7.0f);

    std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};
    sh.ToGPULayout(gpuData, 1.0f);

    // .w should be zero for all coefficients except the first (validity flag)
    for (u32 i = 1; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_FLOAT_EQ(gpuData[i].w, 0.0f);
    }
}

// ── SH Basis function evaluation ──

TEST(SphericalHarmonicsTest, BasisFunctionDCTermIsConstant)
{
    // Y_0^0 (DC term) should be constant regardless of direction
    auto basisPosX = SHBasis::Evaluate(glm::vec3(1.0f, 0.0f, 0.0f));
    auto basisNegY = SHBasis::Evaluate(glm::vec3(0.0f, -1.0f, 0.0f));
    auto basisDiag = SHBasis::Evaluate(glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)));

    EXPECT_FLOAT_EQ(basisPosX[0], SHBasis::Y00);
    EXPECT_FLOAT_EQ(basisNegY[0], SHBasis::Y00);
    EXPECT_FLOAT_EQ(basisDiag[0], SHBasis::Y00);
}

TEST(SphericalHarmonicsTest, BasisFunctionLinearTermsMatchDirection)
{
    glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f));
    auto basis = SHBasis::Evaluate(dir);

    // Y_1^{-1} ~ y, Y_1^0 ~ z, Y_1^1 ~ x
    EXPECT_FLOAT_EQ(basis[1], SHBasis::Y1n1 * dir.y); // y=0
    EXPECT_FLOAT_EQ(basis[2], SHBasis::Y10 * dir.z);  // z=0
    EXPECT_FLOAT_EQ(basis[3], SHBasis::Y11 * dir.x);  // x=1
}

TEST(SphericalHarmonicsTest, BasisFunctionOppositeDirections)
{
    glm::vec3 dirUp = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 dirDown = glm::vec3(0.0f, -1.0f, 0.0f);

    auto basisUp = SHBasis::Evaluate(dirUp);
    auto basisDown = SHBasis::Evaluate(dirDown);

    // DC term should be equal
    EXPECT_FLOAT_EQ(basisUp[0], basisDown[0]);

    // Linear terms should negate
    EXPECT_FLOAT_EQ(basisUp[1], -basisDown[1]); // Y component
}

// ── Irradiance evaluation ──

TEST(SphericalHarmonicsTest, ConstantLightProducesConstantIrradiance)
{
    // If all SH coefficients encode uniform white light (DC term only),
    // irradiance should be the same in every direction
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(1.0f); // DC term

    glm::vec3 irr1 = SHBasis::EvaluateIrradiance(sh, glm::vec3(1, 0, 0));
    glm::vec3 irr2 = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, 1, 0));
    glm::vec3 irr3 = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, 0, 1));

    EXPECT_NEAR(irr1.x, irr2.x, 1e-5f);
    EXPECT_NEAR(irr1.x, irr3.x, 1e-5f);
    EXPECT_NEAR(irr1.y, irr2.y, 1e-5f);
    EXPECT_NEAR(irr1.z, irr2.z, 1e-5f);
}

TEST(SphericalHarmonicsTest, IrradianceIsNonNegative)
{
    // EvaluateIrradiance clamps to zero
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[3] = glm::vec3(-100.0f); // Strong negative in one direction

    glm::vec3 irr = SHBasis::EvaluateIrradiance(sh, glm::vec3(1, 0, 0));
    EXPECT_GE(irr.x, 0.0f);
    EXPECT_GE(irr.y, 0.0f);
    EXPECT_GE(irr.z, 0.0f);
}

TEST(SphericalHarmonicsTest, DirectionalLightHigherInLitDirection)
{
    // Simulate light coming from +Y by setting SH coefficients with
    // a strong Y-direction linear term
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(0.5f); // DC
    sh.Coefficients[1] = glm::vec3(1.0f); // Y-direction linear term

    glm::vec3 irrUp = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, 1, 0));
    glm::vec3 irrDown = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, -1, 0));

    EXPECT_GT(irrUp.x, irrDown.x);
}

// ── LightProbeVolumeComponent helpers ──

TEST(LightProbeVolumeComponentTest, TotalProbeCount)
{
    LightProbeVolumeComponent vol;
    vol.m_Resolution = glm::ivec3(4, 3, 2);
    EXPECT_EQ(vol.GetTotalProbeCount(), 24);
}

TEST(LightProbeVolumeComponentTest, TotalProbeCountSingle)
{
    LightProbeVolumeComponent vol;
    vol.m_Resolution = glm::ivec3(1, 1, 1);
    EXPECT_EQ(vol.GetTotalProbeCount(), 1);
}

TEST(LightProbeVolumeComponentTest, GridIndexLinearization)
{
    LightProbeVolumeComponent vol;
    vol.m_Resolution = glm::ivec3(4, 3, 2);

    // Index (0,0,0) should be 0
    EXPECT_EQ(vol.GridIndex(0, 0, 0), 0);

    // Index (1,0,0) should be 1
    EXPECT_EQ(vol.GridIndex(1, 0, 0), 1);

    // Index (0,1,0) should be dimX = 4
    EXPECT_EQ(vol.GridIndex(0, 1, 0), 4);

    // Index (0,0,1) should be dimX * dimY = 12
    EXPECT_EQ(vol.GridIndex(0, 0, 1), 12);

    // Last probe
    EXPECT_EQ(vol.GridIndex(3, 2, 1), 23);
}

TEST(LightProbeVolumeComponentTest, WorldToGridCorners)
{
    LightProbeVolumeComponent vol;
    vol.m_BoundsMin = glm::vec3(0.0f);
    vol.m_BoundsMax = glm::vec3(10.0f);
    vol.m_Resolution = glm::ivec3(3, 3, 3);

    // Min corner maps to grid (0,0,0)
    glm::vec3 gridMin = vol.WorldToGrid(glm::vec3(0.0f));
    EXPECT_NEAR(gridMin.x, 0.0f, 1e-5f);
    EXPECT_NEAR(gridMin.y, 0.0f, 1e-5f);
    EXPECT_NEAR(gridMin.z, 0.0f, 1e-5f);

    // Max corner maps to grid (2,2,2) for resolution 3
    glm::vec3 gridMax = vol.WorldToGrid(glm::vec3(10.0f));
    EXPECT_NEAR(gridMax.x, 2.0f, 1e-5f);
    EXPECT_NEAR(gridMax.y, 2.0f, 1e-5f);
    EXPECT_NEAR(gridMax.z, 2.0f, 1e-5f);

    // Center maps to grid (1,1,1)
    glm::vec3 gridCenter = vol.WorldToGrid(glm::vec3(5.0f));
    EXPECT_NEAR(gridCenter.x, 1.0f, 1e-5f);
    EXPECT_NEAR(gridCenter.y, 1.0f, 1e-5f);
    EXPECT_NEAR(gridCenter.z, 1.0f, 1e-5f);
}

// ── Size / layout static asserts (compile-time verification) ──

TEST(SphericalHarmonicsTest, SizeConstants)
{
    EXPECT_EQ(SH_COEFFICIENT_COUNT, 9u);
    EXPECT_EQ(SH_GPU_FLOATS_PER_PROBE, 36u);
    EXPECT_EQ(SH_GPU_BYTES_PER_PROBE, 144u);
    EXPECT_EQ(sizeof(SHCoefficients), 9u * sizeof(glm::vec3));
}
