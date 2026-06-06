// =============================================================================
// StarNestSkyMathTest.cpp
//
// Pins the CPU half of the Star Nest nebula sky (issue #292) in
// OloEngine/Renderer/StarNestSky.cpp. Pure CPU math — no GPU context — so it
// runs on headless CI.
//
// What gets pinned:
//
//   - ComputeUBO packs the authoring parameters into the std140 layout the
//     StarNestSky.glsl shader consumes (a field mix-up would silently feed the
//     shader the wrong knob).
//
//   - ComputeUBO clamps / sanitises adversarial inputs: NaN/inf offsets fall
//     back to the default, step size / tile stay strictly positive (the shader
//     divides / mods by them), iteration + volume-step counts stay within the
//     shader's fixed loop ceilings (feeding more would silently truncate).
//
//   - HashParameters is stable for identical inputs and changes when any
//     parameter or the resolution changes (drives rebake-on-drift).
//
//   - The CPU mirror of the raymarch (EvaluateAtDirection) stays finite and
//     non-negative for every direction and reacts monotonically to the
//     brightness / intensity knobs. The GLSL is a direct port of this code, so
//     pinning the C++ side makes a shader-side typo observable without a GPU.
//
// Classification: L4 / shaderpipe (CPU mirror of shader math).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/StarNestSky.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        StarNestParameters StandardParams()
        {
            // Defaults already reproduce the reference shadertoy; return a copy
            // so individual tests can perturb single fields.
            return StarNestParameters{};
        }
    } // namespace

    // ---------- UBO packing ----------

    TEST(StarNestSkyMath, ComputeUBOPacksFieldsInOrder)
    {
        StarNestParameters p = StandardParams();
        p.Offset = glm::vec3(0.2f, 0.4f, 0.6f);
        p.Rotation1 = 0.11f;
        p.Rotation2 = 0.22f;
        p.Formuparam = 0.5f;
        p.StepSize = 0.12f;
        p.Tile = 0.9f;
        p.Brightness = 0.002f;
        p.DarkMatter = 0.25f;
        p.DistFading = 0.7f;
        p.Saturation = 0.8f;
        p.Intensity = 1.5f;
        p.Iterations = 15;
        p.VolSteps = 18;

        const StarNestSkyUBO ubo = StarNestSky::ComputeUBO(p);

        EXPECT_FLOAT_EQ(ubo.Offset.x, 0.2f);
        EXPECT_FLOAT_EQ(ubo.Offset.y, 0.4f);
        EXPECT_FLOAT_EQ(ubo.Offset.z, 0.6f);
        EXPECT_FLOAT_EQ(ubo.Offset.w, 0.11f); // rotation1

        EXPECT_FLOAT_EQ(ubo.Params0.x, 0.5f);  // formuparam
        EXPECT_FLOAT_EQ(ubo.Params0.y, 0.12f); // stepsize
        EXPECT_FLOAT_EQ(ubo.Params0.z, 0.9f);  // tile
        EXPECT_FLOAT_EQ(ubo.Params0.w, 0.22f); // rotation2

        EXPECT_FLOAT_EQ(ubo.Params1.x, 0.002f); // brightness
        EXPECT_FLOAT_EQ(ubo.Params1.y, 0.25f);  // darkmatter
        EXPECT_FLOAT_EQ(ubo.Params1.z, 0.7f);   // distfading
        EXPECT_FLOAT_EQ(ubo.Params1.w, 0.8f);   // saturation

        EXPECT_FLOAT_EQ(ubo.Params2.x, 1.5f);  // intensity
        EXPECT_FLOAT_EQ(ubo.Params2.y, 15.0f); // iterations
        EXPECT_FLOAT_EQ(ubo.Params2.z, 18.0f); // volsteps
    }

    // ---------- Sanitisation / clamping ----------

    TEST(StarNestSkyMath, NonFiniteOffsetFallsBackToDefault)
    {
        StarNestParameters p = StandardParams();
        p.Offset = glm::vec3(std::nanf(""), 0.5f, 0.5f);
        const StarNestSkyUBO ubo = StarNestSky::ComputeUBO(p);

        EXPECT_TRUE(std::isfinite(ubo.Offset.x));
        EXPECT_TRUE(std::isfinite(ubo.Offset.y));
        EXPECT_TRUE(std::isfinite(ubo.Offset.z));
        // Default offset is (1.0, 0.5, 0.5).
        EXPECT_FLOAT_EQ(ubo.Offset.x, 1.0f);
    }

    TEST(StarNestSkyMath, StepSizeAndTileStayStrictlyPositive)
    {
        // The shader divides by dot(p,p) and mods by tile*2; a zero/negative
        // step or tile would produce a degenerate or inverted field.
        StarNestParameters p = StandardParams();
        p.StepSize = 0.0f;
        p.Tile = -1.0f;
        const StarNestSkyUBO ubo = StarNestSky::ComputeUBO(p);

        EXPECT_GT(ubo.Params0.y, 0.0f) << "step size must clamp positive";
        EXPECT_GT(ubo.Params0.z, 0.0f) << "tile must clamp positive";
    }

    TEST(StarNestSkyMath, LoopCountsClampToShaderCeilings)
    {
        StarNestParameters p = StandardParams();
        p.Iterations = 9999;
        p.VolSteps = -5;
        const StarNestSkyUBO ubo = StarNestSky::ComputeUBO(p);

        EXPECT_FLOAT_EQ(ubo.Params2.y, static_cast<f32>(kStarNestMaxIterations));
        EXPECT_FLOAT_EQ(ubo.Params2.z, 1.0f) << "volsteps must clamp to >= 1";
    }

    TEST(StarNestSkyMath, SaturationAndFadesStayInUnitRange)
    {
        StarNestParameters p = StandardParams();
        p.Saturation = 5.0f;
        p.DistFading = 5.0f;
        const StarNestSkyUBO ubo = StarNestSky::ComputeUBO(p);

        EXPECT_LE(ubo.Params1.z, 1.0f); // distfading
        EXPECT_LE(ubo.Params1.w, 1.0f); // saturation
        EXPECT_GE(ubo.Params1.z, 0.0f);
        EXPECT_GE(ubo.Params1.w, 0.0f);
    }

    // ---------- Hash stability ----------

    TEST(StarNestSkyMath, HashChangesWhenParametersChange)
    {
        StarNestParameters p1 = StandardParams();
        StarNestParameters p2 = p1;
        p2.Formuparam += 0.01f;

        EXPECT_NE(StarNestSky::HashParameters(p1, 256),
                  StarNestSky::HashParameters(p2, 256));
        EXPECT_NE(StarNestSky::HashParameters(p1, 256),
                  StarNestSky::HashParameters(p1, 512));
        EXPECT_EQ(StarNestSky::HashParameters(p1, 256),
                  StarNestSky::HashParameters(p1, 256));

        StarNestParameters p3 = p1;
        p3.VolSteps += 1;
        EXPECT_NE(StarNestSky::HashParameters(p1, 256),
                  StarNestSky::HashParameters(p3, 256));
    }

    // ---------- CPU raymarch evaluator ----------

    TEST(StarNestSkyMath, EvaluateIsFiniteAndNonNegativeEverywhere)
    {
        const StarNestSkyUBO ubo = StarNestSky::ComputeUBO(StandardParams());

        // 26 directions on the unit sphere.
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z)
                {
                    if (x == 0 && y == 0 && z == 0)
                        continue;
                    const glm::vec3 dir = glm::normalize(glm::vec3(x, y, z));
                    const glm::vec3 rgb = StarNestSky::EvaluateAtDirection(ubo, dir);
                    for (int i = 0; i < 3; ++i)
                    {
                        EXPECT_TRUE(std::isfinite(rgb[i]))
                            << "dir = (" << dir.x << "," << dir.y << "," << dir.z << ")";
                        EXPECT_GE(rgb[i], 0.0f);
                    }
                }
    }

    TEST(StarNestSkyMath, EvaluateIsDeterministic)
    {
        const StarNestSkyUBO ubo = StarNestSky::ComputeUBO(StandardParams());
        const glm::vec3 dir = glm::normalize(glm::vec3(0.3f, 0.6f, 0.7f));
        const glm::vec3 a = StarNestSky::EvaluateAtDirection(ubo, dir);
        const glm::vec3 b = StarNestSky::EvaluateAtDirection(ubo, dir);
        EXPECT_EQ(a, b);
    }

    TEST(StarNestSkyMath, HigherIntensityIsBrighter)
    {
        StarNestParameters dim = StandardParams();
        dim.Intensity = 1.0f;
        StarNestParameters bright = StandardParams();
        bright.Intensity = 4.0f;

        const glm::vec3 dir = glm::normalize(glm::vec3(0.2f, 0.5f, 0.8f));
        const glm::vec3 dimRGB = StarNestSky::EvaluateAtDirection(StarNestSky::ComputeUBO(dim), dir);
        const glm::vec3 brightRGB = StarNestSky::EvaluateAtDirection(StarNestSky::ComputeUBO(bright), dir);

        const auto luma = [](const glm::vec3& c)
        { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };
        // Intensity is a linear multiplier, so 4x intensity is strictly brighter.
        EXPECT_GT(luma(brightRGB), luma(dimRGB));
    }

    TEST(StarNestSkyMath, ZeroBrightnessKeepsTheBaseFieldFinite)
    {
        // brightness scales only the coloured term; the fade accumulation
        // (v += fade) still produces a non-negative finite base glow.
        StarNestParameters p = StandardParams();
        p.Brightness = 0.0f;
        const glm::vec3 rgb = StarNestSky::EvaluateAtDirection(
            StarNestSky::ComputeUBO(p), glm::normalize(glm::vec3(0.1f, 0.2f, 0.9f)));
        for (int i = 0; i < 3; ++i)
        {
            EXPECT_TRUE(std::isfinite(rgb[i]));
            EXPECT_GE(rgb[i], 0.0f);
        }
    }
} // namespace OloEngine::Tests
