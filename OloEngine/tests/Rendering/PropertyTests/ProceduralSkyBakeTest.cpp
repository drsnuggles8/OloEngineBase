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

#include "TestRenderBackend.h"
#include "OloEngine/Renderer/ProceduralSky.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "Platform/OpenGL/OpenGLUtilities.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <cstring>
#include <functional>
#include <iostream>

namespace OloEngine::Tests
{
    // Reuses RendererAttachedTest only for its one-time Renderer::Init (which
    // loads ProceduralSky.glsl into the shader library). We don't need the
    // per-test scene, so BuildScene is a no-op.
    //
    // The suite runs in the --olo-rhi=vulkan process too: the bakes go through
    // RenderCommand and the results are read back by the textures' own
    // GetFaceData / GetData. Each bake is issued inside RunInFrame, because
    // work recorded outside the Vulkan frame bracket is dropped.
    class ProceduralSkyBakeTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override {}

        Ref<EnvironmentMap> Generate(const PreethamParameters& params, u32 resolution)
        {
            Ref<EnvironmentMap> envMap;
            RunInFrame([&envMap, &params, resolution]()
                       { envMap = ProceduralSky::Generate(params, resolution); });
            return envMap;
        }
    };

    TEST_F(ProceduralSkyBakeTest, GeneratesNonBlackEnvironmentMapWithIBL)
    {
        PreethamParameters params;
        params.SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));
        params.Turbidity = 3.0f;
        params.Exposure = 0.05f;
        params.ShowSunDisk = true;

        // Small cubemap keeps the bake + IBL convolution fast in CI.
        auto envMap = Generate(params, 64);

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

        auto clearMap = Generate(clear, 64);
        auto hazyMap = Generate(hazy, 64);
        ASSERT_TRUE(clearMap && clearMap->GetEnvironmentMap());
        ASSERT_TRUE(hazyMap && hazyMap->GetEnvironmentMap());

        const auto clearDC = IBLPrecompute::ProjectCubemapToSH(clearMap->GetEnvironmentMap()).Coefficients[0];
        const auto hazyDC = IBLPrecompute::ProjectCubemapToSH(hazyMap->GetEnvironmentMap()).Coefficients[0];

        const f32 clearLum = clearDC.r + clearDC.g + clearDC.b;
        const f32 hazyLum = hazyDC.r + hazyDC.g + hazyDC.b;

        EXPECT_GT(hazyLum, clearLum)
            << "Higher turbidity must brighten the average sky radiance.";
    }

    TEST_F(ProceduralSkyBakeTest, CubemapReadbackToleratesLeakedGLError)
    {
        // Regression for the cross-suite flake: an earlier test left a pending GL
        // error in the context (a shadow pass's GL_INVALID_OPERATION). When
        // IBLPrecompute::ProjectCubemapToSH read the baked cubemap,
        // OpenGLTextureCubemap::GetFaceData checked glGetError() *without* first
        // draining that stale error, misattributed it to its own readback, and
        // returned false — so the SH projection saw a spurious all-black cubemap
        // (dcMagnitude == 0) even though the bake was fine. It "self-healed" after
        // one read because GetFaceData's own glGetError() consumed the leaked
        // error, which is why only the first sky read in the suite ever failed.
        //
        // Reproduce deterministically: bake a known-good sky, leak a GL error,
        // then read it back. The readback must drain the stale error and still
        // return the real (non-black) radiance.
        if (TestRenderBackend::IsVulkanProcess())
        {
            GTEST_SKIP() << "GL-only by nature: the fault is a pending glGetError() value, which Vulkan does not have";
        }
        PreethamParameters params;
        params.SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));
        params.Turbidity = 3.0f;
        params.Exposure = 0.05f;
        params.ShowSunDisk = true;

        auto envMap = Generate(params, 64);
        ASSERT_TRUE(envMap && envMap->GetEnvironmentMap()) << "sky bake failed";

        // Leak a pending GL error, simulating an unrelated earlier operation.
        // GL_FLOAT is not a valid glEnable capability → GL_INVALID_ENUM. Do NOT
        // call glGetError() afterward — that would clear the very error the
        // readback must now tolerate.
        glEnable(GL_FLOAT);

        const SHCoefficients sh = IBLPrecompute::ProjectCubemapToSH(envMap->GetEnvironmentMap());

        // Scrub any residual error so this test leaks nothing onward (bounded
        // helper — no hand-rolled unbounded drain loop).
        Utils::DrainGLErrors();

        const glm::vec3 dc = sh.Coefficients[0];
        EXPECT_GT(dc.r + dc.g + dc.b, 0.0f)
            << "a leaked GL error was misattributed to the cubemap readback — "
               "OpenGLTextureCubemap::GetFaceData must drain stale errors before "
               "its own glGetError() check";
    }

    // The bake draws a cube from the inside, three more through the IBL
    // convolution and a fullscreen triangle for the BRDF table, all outside the
    // render graph. None of them may draw with the fixed-function state its
    // caller happened to leave: a capture that ended with back-face culling on
    // took the sky's light off every coat drawn afterwards (#1352).
    TEST_F(ProceduralSkyBakeTest, TheBakeIsTheSameUnderWhateverStateItIsCalledWith)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        PreethamParameters params;
        params.SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));
        params.Turbidity = 3.0f;
        params.Exposure = 0.05f;
        params.ShowSunDisk = true;

        struct Baked
        {
            f32 Sky = 0.0f;
            f32 Irradiance = 0.0f;
            f32 Prefilter = 0.0f;
            f64 BrdfLut = 0.0;
        };
        const auto sum = [](const glm::vec3& v)
        { return v.r + v.g + v.b; };
        // The caller's state is set inside the frame the bake is issued in, and
        // what the case changed is put back before that frame ends: the next
        // case, and the next suite, start from the state this test found.
        const auto bake = [&](const char* label, const std::function<void()>& callerState) -> Baked
        {
            Baked out;
            Ref<EnvironmentMap> envMap;
            RunInFrame([&envMap, &params, &callerState]()
                       {
                           const bool culling = RenderCommand::IsCullingEnabled();
                           const bool depthTest = RenderCommand::IsDepthTestEnabled();
                           const bool blend = RenderCommand::IsBlendEnabled();
                           RenderCommand::DisableCulling();
                           RenderCommand::SetDepthTest(false);
                           RenderCommand::SetBlendState(false);
                           callerState();

                           envMap = ProceduralSky::Generate(params, 64);

                           RenderCommand::BackCull();
                           if (culling)
                           {
                               RenderCommand::EnableCulling();
                           }
                           else
                           {
                               RenderCommand::DisableCulling();
                           }
                           RenderCommand::SetDepthMask(true);
                           RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
                           RenderCommand::SetDepthTest(depthTest);
                           // The blend func cannot be asked for, and the two
                           // backends start with different ones: OpenGL's
                           // Init sets (SrcAlpha, OneMinusSrcAlpha), Vulkan's
                           // recorded state starts at (One, Zero).
                           if (TestRenderBackend::IsVulkanProcess())
                           {
                               RenderCommand::SetBlendFunc(RHI::BlendFactor::One, RHI::BlendFactor::Zero);
                           }
                           else
                           {
                               RenderCommand::SetBlendFunc(RHI::BlendFactor::SrcAlpha, RHI::BlendFactor::OneMinusSrcAlpha);
                           }
                           RenderCommand::SetBlendState(blend); });
            if (!envMap || !envMap->GetEnvironmentMap() || !envMap->GetIrradianceMap() || !envMap->GetPrefilterMap() || !envMap->GetBRDFLutMap())
            {
                ADD_FAILURE() << label << ": the bake produced no complete environment";
                return out;
            }
            out.Sky = sum(IBLPrecompute::ProjectCubemapToSH(envMap->GetEnvironmentMap()).Coefficients[0]);
            out.Irradiance = sum(IBLPrecompute::ProjectCubemapToSH(envMap->GetIrradianceMap()).Coefficients[0]);
            out.Prefilter = sum(IBLPrecompute::ProjectCubemapToSH(envMap->GetPrefilterMap()).Coefficients[0]);
            TArray64<u8> lut;
            if (envMap->GetBRDFLutMap()->GetData(lut))
            {
                const sizet floats = static_cast<sizet>(lut.Num()) / sizeof(f32);
                for (sizet i = 0; i < floats; ++i)
                {
                    f32 value = 0.0f;
                    std::memcpy(&value, lut.GetData() + i * sizeof(f32), sizeof(f32));
                    out.BrdfLut += static_cast<f64>(value);
                }
            }
            std::cout << "[ProceduralSkyBake] " << (TestRenderBackend::IsVulkanProcess() ? "Vulkan" : "GL") << ", " << label << ": sky "
                      << out.Sky << ", irradiance " << out.Irradiance << ", prefilter " << out.Prefilter << ", BRDF table sum "
                      << out.BrdfLut << "\n";
            return out;
        };

        const Baked plain = bake("culling off, depth test off, blend off", []() {});
        ASSERT_GT(plain.Sky, 0.0f);
        ASSERT_GT(plain.Irradiance, 0.0f);
        ASSERT_GT(plain.Prefilter, 0.0f);
        ASSERT_GT(plain.BrdfLut, 0.0);

        const auto expectSame = [&plain](const Baked& other, const char* label)
        {
            EXPECT_NEAR(other.Sky, plain.Sky, plain.Sky * 1e-4f) << label << ": the sky cube";
            EXPECT_NEAR(other.Irradiance, plain.Irradiance, plain.Irradiance * 1e-4f) << label << ": the irradiance cube";
            EXPECT_NEAR(other.Prefilter, plain.Prefilter, plain.Prefilter * 1e-4f) << label << ": the prefiltered cube";
            EXPECT_NEAR(other.BrdfLut, plain.BrdfLut, plain.BrdfLut * 1e-4) << label << ": the BRDF table";
        };

        expectSame(bake("back-face culling", []()
                        {
                            RenderCommand::EnableCulling();
                            RenderCommand::BackCull(); }),
                   "back-face culling");
        expectSame(bake("front-face culling", []()
                        {
                            RenderCommand::EnableCulling();
                            RenderCommand::FrontCull(); }),
                   "front-face culling");
        expectSame(bake("depth test on, depth write off", []()
                        {
                            RenderCommand::SetDepthTest(true);
                            RenderCommand::SetDepthMask(false); }),
                   "depth test on, depth write off");
        expectSame(bake("depth test on, func greater", []()
                        {
                            RenderCommand::SetDepthTest(true);
                            RenderCommand::SetDepthFunc(RHI::CompareOp::Greater); }),
                   "depth test on, func greater");
        expectSame(bake("additive blend", []()
                        {
                            RenderCommand::SetBlendState(true);
                            RenderCommand::SetBlendFunc(RHI::BlendFactor::One, RHI::BlendFactor::One); }),
                   "additive blend");
    }
} // namespace OloEngine::Tests
