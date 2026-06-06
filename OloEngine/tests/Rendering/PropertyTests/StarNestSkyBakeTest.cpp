// =============================================================================
// StarNestSkyBakeTest.cpp
//
// End-to-end GPU coverage for the Star Nest nebula sky bake
// (StarNestSky::Generate, issue #292). The CPU-side math is pinned by
// StarNestSkyMathTest.cpp; this verifies the GPU half produces a usable
// EnvironmentMap:
//
//   - The StarNestSky.glsl shader compiles, links, and binds the StarNestSkyUBO
//     at UBO_STAR_NEST_SKY (39).
//   - Six cubemap faces render and copy without GL errors.
//   - The result is non-black (no silently-emitted zeros — the classic wrong
//     UBO binding regression) and finite (no NaN leaking from the raymarch).
//   - The IBL pipeline (irradiance / prefilter / BRDF LUT) ran on the baked
//     cubemap, so the nebula lights and reflects off PBR materials exactly like
//     a file-based environment map — which IS the "skybox reflectiveness".
//   - The baked sky radiance stays in a sane linear-HDR range (won't tonemap
//     the whole scene to white).
//
// Requires a GL 4.6 context; SKIP'd on headless CI via RendererAttachedTest.
//
// Classification: L8 / integration (full Renderer3D bring-up + GPU bake +
// cubemap readback through the SH projection used by the IBL path).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/StarNestSky.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    // Reuses RendererAttachedTest only for its one-time Renderer::Init (which
    // loads StarNestSky.glsl into the shader library). No per-test scene needed.
    class StarNestSkyBakeTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(StarNestSkyBakeTest, GeneratesNonBlackEnvironmentMapWithIBL)
    {
        StarNestParameters params;
        // Bump intensity so the small CI cubemap has a clearly non-black average
        // (the default nebula is mostly dark space with sparse bright cores).
        params.Intensity = 4.0f;

        // Small cubemap keeps the bake + IBL convolution fast in CI.
        auto envMap = StarNestSky::Generate(params, 64);

        ASSERT_TRUE(envMap) << "StarNestSky::Generate returned null — shader "
                               "missing or bake failed.";
        ASSERT_TRUE(envMap->GetEnvironmentMap()) << "No nebula cubemap produced.";
        EXPECT_TRUE(envMap->HasIBL())
            << "IBL textures (irradiance / prefilter / BRDF) were not generated — "
               "PBR materials would get no ambient from the nebula sky.";

        // L00 (DC) SH term = average radiance over the sphere. A non-black sky
        // must have a positive-magnitude DC term on at least one channel.
        const SHCoefficients sh = IBLPrecompute::ProjectCubemapToSH(envMap->GetEnvironmentMap());
        const glm::vec3 dc = sh.Coefficients[0];
        const f32 dcMagnitude = dc.r + dc.g + dc.b;
        EXPECT_GT(dcMagnitude, 0.0f)
            << "Baked nebula cubemap is black — the shader likely emitted zeros "
               "(check the StarNestSkyUBO binding at slot 39).";
        EXPECT_LT(dcMagnitude, 30.0f)
            << "Baked nebula is far too bright — intensity / scaling regressed; "
               "the scene will tonemap to solid white.";
        EXPECT_TRUE(std::isfinite(dc.r) && std::isfinite(dc.g) && std::isfinite(dc.b))
            << "NaN leaked from the raymarch into the cubemap.";

        // The irradiance map is what actually lights scene objects (diffuse
        // ambient). Assert it's populated and in a displayable range, not the
        // hundreds (the "white objects, correct sky" stale/over-bright-IBL bug).
        ASSERT_TRUE(envMap->GetIrradianceMap()) << "No irradiance map generated.";
        const SHCoefficients irrSH = IBLPrecompute::ProjectCubemapToSH(envMap->GetIrradianceMap());
        const glm::vec3 irrDC = irrSH.Coefficients[0];
        const f32 irrMag = irrDC.r + irrDC.g + irrDC.b;
        EXPECT_GT(irrMag, 0.0f) << "Irradiance map is black — objects get no sky ambient.";
        EXPECT_LT(irrMag, 30.0f)
            << "Irradiance map is far too bright — IBL-lit objects will tonemap "
               "to white even when the skybox looks correct.";
    }

    TEST_F(StarNestSkyBakeTest, HigherIntensityProducesBrighterAverage)
    {
        // Intensity is a linear output multiplier, so the average radiance (SH
        // DC term) must rise with it. Catches a regression where the intensity
        // parameter stops feeding the bake.
        StarNestParameters dim;
        dim.Intensity = 1.0f;
        StarNestParameters bright;
        bright.Intensity = 6.0f;

        auto dimMap = StarNestSky::Generate(dim, 64);
        auto brightMap = StarNestSky::Generate(bright, 64);
        ASSERT_TRUE(dimMap && dimMap->GetEnvironmentMap());
        ASSERT_TRUE(brightMap && brightMap->GetEnvironmentMap());

        const auto dimDC = IBLPrecompute::ProjectCubemapToSH(dimMap->GetEnvironmentMap()).Coefficients[0];
        const auto brightDC = IBLPrecompute::ProjectCubemapToSH(brightMap->GetEnvironmentMap()).Coefficients[0];

        const f32 dimLum = dimDC.r + dimDC.g + dimDC.b;
        const f32 brightLum = brightDC.r + brightDC.g + brightDC.b;

        EXPECT_GT(brightLum, dimLum)
            << "Higher intensity must brighten the average nebula radiance.";
    }
} // namespace OloEngine::Tests
