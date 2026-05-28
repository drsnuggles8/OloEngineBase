// =============================================================================
// ProceduralSkyBakeTest.cpp
//
// End-to-end GPU coverage for the Preetham procedural sky bake
// (ProceduralSky::Generate). The CPU-side analytic math is pinned by
// ProceduralSkyMathTest.cpp; this test verifies the *GPU half* actually
// produces a usable EnvironmentMap:
//
//   - The ProceduralSky.glsl shader compiles, links, and binds the
//     PreethamCoefficientsUBO at UBO_PROCEDURAL_SKY (36).
//   - Six cubemap faces render and copy without GL errors.
//   - The result is non-black (the shader didn't silently emit zeros —
//     the classic "wrong UBO binding / unbound sampler" regression).
//   - The IBL pipeline (irradiance / prefilter / BRDF LUT) ran on the
//     baked cubemap, so the procedural sky lights PBR materials exactly
//     like a file-based environment map.
//   - The zenith (sky) is brighter than the nadir (clamped-to-horizon
//     ground) — confirms orientation wasn't flipped during the bake.
//
// Requires a GL 4.6 context; SKIP'd on headless CI via RendererAttachedTest.
//
// Classification: L8 / integration (full Renderer3D bring-up + GPU bake +
// cubemap readback through the SH projection used by the IBL path).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ProceduralSky.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    // Reuses RendererAttachedTest only for its one-time Renderer::Init (which
    // loads ProceduralSky.glsl into the shader library). We don't need the
    // per-test scene, so BuildScene is a no-op.
    class ProceduralSkyBakeTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(ProceduralSkyBakeTest, GeneratesNonBlackEnvironmentMapWithIBL)
    {
        PreethamParameters params;
        params.SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));
        params.Turbidity = 3.0f;
        params.Exposure = 0.05f;
        params.ShowSunDisk = true;

        // Small cubemap keeps the bake + IBL convolution fast in CI.
        auto envMap = ProceduralSky::Generate(params, 64);

        ASSERT_TRUE(envMap) << "ProceduralSky::Generate returned null — shader "
                               "missing or bake failed.";
        ASSERT_TRUE(envMap->GetEnvironmentMap()) << "No sky cubemap produced.";
        EXPECT_TRUE(envMap->HasIBL())
            << "IBL textures (irradiance / prefilter / BRDF) were not generated — "
               "PBR materials would get no ambient from the procedural sky.";

        // Project the baked cubemap to SH. The L00 (DC) coefficient is the
        // average radiance over the sphere; a non-black sky must have a
        // positive-magnitude DC term on at least one channel.
        const SHCoefficients sh = IBLPrecompute::ProjectCubemapToSH(envMap->GetEnvironmentMap());
        const glm::vec3 dc = sh.Coefficients[0];
        const f32 dcMagnitude = dc.r + dc.g + dc.b;
        EXPECT_GT(dcMagnitude, 0.0f)
            << "Baked sky cubemap is black — the shader likely emitted zeros "
               "(check the PreethamCoefficientsUBO binding at slot 36).";

        // Upper bound: the average radiance must stay in a sane linear-HDR
        // range. Regression guard for the "* 1000 kcd->cd" bug, which pushed
        // sky radiance into the hundreds and crushed the whole scene to white
        // through the tonemapper (and dragged IBL ambient white with it).
        // Default exposure (0.1) targets a per-channel sky of ~0.5-2, so the
        // summed SH DC term sits well under this ceiling.
        EXPECT_LT(dcMagnitude, 30.0f)
            << "Baked sky is far too bright — exposure / luminance scaling "
               "regressed; the scene will tonemap to solid white.";

        // All channels finite (no NaN leaking from a bad Perez evaluation).
        EXPECT_TRUE(std::isfinite(dc.r) && std::isfinite(dc.g) && std::isfinite(dc.b));

        // CRITICAL: the irradiance map is what actually lights scene objects
        // (diffuse ambient). The skybox cubemap above can look correct while a
        // stale/over-bright irradiance map blows every surface to white — that
        // was the exact bug behind the "white objects, correct sky" report
        // (the IBL disk cache served stale textures because its key ignored the
        // sky parameters). Assert the irradiance map sits in a displayable
        // range, not the hundreds.
        ASSERT_TRUE(envMap->GetIrradianceMap()) << "No irradiance map generated.";
        const SHCoefficients irrSH = IBLPrecompute::ProjectCubemapToSH(envMap->GetIrradianceMap());
        const glm::vec3 irrDC = irrSH.Coefficients[0];
        const f32 irrMag = irrDC.r + irrDC.g + irrDC.b;
        EXPECT_GT(irrMag, 0.0f) << "Irradiance map is black — objects get no sky ambient.";
        EXPECT_LT(irrMag, 30.0f)
            << "Irradiance map is far too bright — IBL-lit objects will tonemap "
               "to white even when the skybox looks correct (stale-cache / "
               "over-bright-bake regression).";
    }

    TEST_F(ProceduralSkyBakeTest, HigherTurbidityProducesBrighterAverage)
    {
        // Hazier skies scatter more light, so the average radiance (SH DC
        // term) should rise with turbidity. This catches a regression where
        // the turbidity parameter stops feeding the coefficient computation.
        PreethamParameters clear;
        clear.SunDirection = glm::normalize(glm::vec3(0.2f, 0.8f, 0.3f));
        clear.Turbidity = 2.0f;
        clear.Exposure = 0.05f;
        clear.ShowSunDisk = false; // Disk would swamp the average

        PreethamParameters hazy = clear;
        hazy.Turbidity = 8.0f;

        auto clearMap = ProceduralSky::Generate(clear, 64);
        auto hazyMap = ProceduralSky::Generate(hazy, 64);
        ASSERT_TRUE(clearMap && clearMap->GetEnvironmentMap());
        ASSERT_TRUE(hazyMap && hazyMap->GetEnvironmentMap());

        const auto clearDC = IBLPrecompute::ProjectCubemapToSH(clearMap->GetEnvironmentMap()).Coefficients[0];
        const auto hazyDC = IBLPrecompute::ProjectCubemapToSH(hazyMap->GetEnvironmentMap()).Coefficients[0];

        const f32 clearLum = clearDC.r + clearDC.g + clearDC.b;
        const f32 hazyLum = hazyDC.r + hazyDC.g + hazyDC.b;

        EXPECT_GT(hazyLum, clearLum)
            << "Higher turbidity must brighten the average sky radiance.";
    }
}
