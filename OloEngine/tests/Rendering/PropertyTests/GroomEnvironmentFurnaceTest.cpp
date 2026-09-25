#include "OloEnginePCH.h"

// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GroomEnvironmentFurnaceTest.cpp — the coat's environment term under a white
// furnace, through the REAL irradiance cube (issue #1450).
//
// THE CLAIM. A uniform sky of radiance L, baked into an irradiance cube by the
// production IBLPrecompute producers, must reach the coat at the same radiance
// scale it reaches a Lambertian surface lit by that cube. Measured as the L
// each reader implies:
//
//   coat:       probe value / GroomFibreAmbientResponse(params, sinThetaO)
//   Lambertian: probe value / ((1 - F) * albedo)
//
// Both must equal L. Before #1450 the coat divided the cube sample by pi, so
// its implied L was L / pi while the Lambertian one was L. The cube stores
// NORMALIZED irradiance E / pi (docs/agent-rules/lighting-signal-contract.md),
// and the old comment took it for E.
//
// ALL THREE PRODUCERS: the importance-sampled convolution (the production
// default), the L2 SH projection (EnvironmentMapComponent's
// m_UseSphericalHarmonics) and the plain convolution (the fallback when the
// advanced shader is missing). A groom samples whichever one the scene baked,
// so each has to hand it the same quantity.
//
// Negative control, run when this was written: restoring the `* (1.0 /
// OLO_GROOM_FIBRE_PI)` in oloGroomFibreEnvironmentRadiance turns every
// coat/Lambertian ratio below into 0.318.
//
// Skips cleanly without a GL 4.6 context, like every probe fixture here.
// Classification: shaderpipe (production shader includes and production bakes,
// compared against physics-derived answers).
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The probe's column layout, named in its header.
        constexpr u32 kCoatColumns = 4;
        constexpr u32 kLambertColumn = 4;
        constexpr u32 kRawColumn = 5;
        constexpr u32 kColumnCount = 6;

        // THE FIXTURE, mirrored from GroomEnvironmentFurnaceProbe.glsl.
        constexpr glm::vec3 kUniformRadiance{ 1.2f, 0.9f, 0.6f };
        constexpr glm::vec3 kAlbedo{ 0.8f, 0.5f, 0.2f };
        constexpr f32 kRoughness = 0.5f;
        constexpr std::array<f32, kCoatColumns> kSinThetaO{ 0.0f, 0.3f, 0.6f, 0.85f };

        // 3 %: the plain convolution integrates on a fixed 0.025 rad grid, and
        // PbrIrradianceTest holds it to 2 % of unity for a white sky. The bug
        // this pins is a factor of pi, so the band is two orders of magnitude
        // tighter than the failure.
        constexpr f32 kRelativeTolerance = 3.0e-2f;

        // The brown fibre the probe hard-codes, from the authored side, as
        // GroomFibreGpuParityTest builds it.
        [[nodiscard]] GroomFibreParams ProbeParams()
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Melanin;
            authored.Eumelanin = 1.3f;
            authored.Pheomelanin = 0.0f;
            authored.LongitudinalRoughness = 0.3f;
            authored.AzimuthalRoughness = 0.3f;
            authored.TiltDegrees = 2.0f;
            authored.IndexOfRefraction = kGroomFibreDefaultIOR;
            authored.Intensity = 1.0f;
            authored.HSamples = 4;
            return MakeGroomFibreParams(authored);
        }

        // (1 - F) * albedo: what a dielectric Lambertian body keeps of a uniform
        // field, with F Schlick's roughness-aware fit at the probe's view angle.
        // Written from the physics, as LightingSignalContractGpuTest writes it.
        [[nodiscard]] glm::vec3 LambertianReflectance()
        {
            const glm::vec3 view = glm::normalize(glm::vec3(0.3f, 0.0f, 1.0f));
            const f32 cosTheta = view.z;
            constexpr f32 kF0 = 0.04f;
            const f32 fresnel = kF0 + ((std::max(1.0f - kRoughness, kF0) - kF0) * std::pow(1.0f - cosTheta, 5.0f));
            return (1.0f - fresnel) * kAlbedo;
        }

        enum class Producer : u8
        {
            ImportanceSampled,
            SphericalHarmonics,
            PlainConvolution,
        };

        [[nodiscard]] const char* ToString(Producer producer)
        {
            switch (producer)
            {
                case Producer::ImportanceSampled:
                    return "importance-sampled convolution";
                case Producer::SphericalHarmonics:
                    return "L2 SH projection";
                case Producer::PlainConvolution:
                    return "plain convolution";
            }
            return "?";
        }

        // A uniform sky of radiance kUniformRadiance, with the mip chain the
        // importance-sampled producer reads.
        [[nodiscard]] Ref<TextureCubemap> MakeUniformSky()
        {
            constexpr u32 kResolution = 32;
            CubemapSpecification spec{};
            spec.Width = kResolution;
            spec.Height = kResolution;
            spec.Format = ImageFormat::RGBA32F;
            spec.GenerateMips = true;
            Ref<TextureCubemap> sky = TextureCubemap::Create(spec);
            if (!sky)
            {
                return nullptr;
            }
            std::vector<f32> face(static_cast<sizet>(kResolution) * kResolution * 4u);
            for (sizet i = 0; i < face.size(); i += 4u)
            {
                face[i + 0u] = kUniformRadiance.r;
                face[i + 1u] = kUniformRadiance.g;
                face[i + 2u] = kUniformRadiance.b;
                face[i + 3u] = 1.0f;
            }
            for (u32 f = 0; f < 6u; ++f)
            {
                if (!sky->SetFaceDataMip(f, 0, face.data(), static_cast<u32>(face.size() * sizeof(f32))))
                {
                    ADD_FAILURE() << "SetFaceDataMip failed for face " << f;
                    return nullptr;
                }
            }
            sky->GenerateMipmaps();
            return sky;
        }

        // The irradiance cube, baked from `sky` by `producer` into the cube
        // shape EnvironmentMap::GenerateIrradianceMapWithConfig creates.
        [[nodiscard]] Ref<TextureCubemap> Bake(const Ref<TextureCubemap>& sky, Producer producer)
        {
            IBLConfiguration config;
            CubemapSpecification spec{};
            spec.Width = config.IrradianceResolution;
            spec.Height = config.IrradianceResolution;
            spec.Format = ImageFormat::RGBA32F;
            spec.GenerateMips = false;
            Ref<TextureCubemap> irradiance = TextureCubemap::Create(spec);
            if (!irradiance)
            {
                return nullptr;
            }

            ShaderLibrary library;
            switch (producer)
            {
                case Producer::ImportanceSampled:
                    library.Load("IrradianceConvolutionAdvanced", "assets/shaders/IrradianceConvolutionAdvanced.glsl");
                    IBLPrecompute::GenerateIrradianceMapAdvanced(sky, irradiance, library, config);
                    break;
                case Producer::SphericalHarmonics:
                    library.Load("IrradianceFromSH", "assets/shaders/IrradianceFromSH.glsl");
                    config.UseSphericalHarmonics = true;
                    (void)IBLPrecompute::GenerateIrradianceMapFromSH(sky, irradiance, library, config);
                    break;
                case Producer::PlainConvolution:
                    library.Load("IrradianceConvolution", "assets/shaders/IrradianceConvolution.glsl");
                    IBLPrecompute::GenerateIrradianceMap(sky, irradiance, library);
                    break;
            }
            return irradiance;
        }

        struct FurnaceProbe
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            FurnaceProbe()
            {
                FramebufferSpecification spec{};
                spec.Width = kColumnCount;
                spec.Height = 1;
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/GroomEnvironmentFurnaceProbe.glsl");
            }

            void Draw(const Ref<TextureCubemap>& irradiance)
            {
                GLStateGuard guard("GroomEnvironmentFurnace::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(kColumnCount), 1);
                ::glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                ::glClear(GL_COLOR_BUFFER_BIT);
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                // TEX_USER_0, the slot GroomStrand.glsl samples the cube from.
                irradiance->Bind(10);
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            [[nodiscard]] std::array<glm::vec3, kColumnCount> Read() const
            {
                std::vector<f32> pixels;
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), kColumnCount, 1, pixels);
                std::array<glm::vec3, kColumnCount> columns{};
                if (pixels.size() != static_cast<sizet>(kColumnCount) * 4u)
                {
                    ADD_FAILURE() << "probe readback returned " << pixels.size() << " floats";
                    return columns;
                }
                for (u32 c = 0; c < kColumnCount; ++c)
                {
                    const sizet base = static_cast<sizet>(c) * 4u;
                    EXPECT_EQ(pixels[base + 3u], 1.0f) << "column " << c << " was never written";
                    columns[c] = glm::vec3(pixels[base], pixels[base + 1u], pixels[base + 2u]);
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        EXPECT_TRUE(std::isfinite(columns[c][ch])) << "column " << c << " is non-finite";
                    }
                }
                return columns;
            }
        };

        void ExpectRelative(f32 actual, f32 expected, const std::string& what)
        {
            EXPECT_NEAR(actual, expected, kRelativeTolerance * std::abs(expected)) << what;
        }
    } // namespace

    TEST(GroomEnvironmentFurnace, TheCoatSeesTheSameSkyAsALambertianSurfaceFromEveryProducer)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FurnaceProbe probe;
        ASSERT_TRUE(probe.m_Shader) << "the probe shader was not created at all";
        ASSERT_TRUE(probe.m_Shader->IsReady())
            << "GroomEnvironmentFurnaceProbe.glsl did not compile; check OloEngine.log. Every assertion below would "
               "otherwise measure a frame no shader wrote.";

        const Ref<TextureCubemap> sky = MakeUniformSky();
        ASSERT_TRUE(sky);

        const GroomFibreParams params = ProbeParams();
        const glm::vec3 lambertian = LambertianReflectance();

        for (const Producer producer :
             { Producer::ImportanceSampled, Producer::SphericalHarmonics, Producer::PlainConvolution })
        {
            SCOPED_TRACE(ToString(producer));
            const Ref<TextureCubemap> irradiance = Bake(sky, producer);
            ASSERT_TRUE(irradiance);

            probe.Draw(irradiance);
            const std::array<glm::vec3, kColumnCount> columns = probe.Read();

            for (int ch = 0; ch < 3; ++ch)
            {
                const std::string channel = " (channel " + std::to_string(ch) + ")";

                // The cube itself, and the Lambertian rung reading it: the
                // producer stores E / pi, which for a uniform sky IS L.
                ExpectRelative(columns[kRawColumn][ch], kUniformRadiance[ch],
                               std::string("oloGroomFibreEnvironmentRadiance does not return the sky's radiance L "
                                           "for a uniform sky; a reading of L / pi is issue #1450") +
                                   channel);
                const f32 lambertianL = columns[kLambertColumn][ch] / lambertian[ch];
                ExpectRelative(lambertianL, kUniformRadiance[ch],
                               "the Lambertian IBL rung does not reflect (1 - F) * albedo * L from this cube, so the "
                               "cube is not storing E / pi" +
                                   channel);

                // The coat, at four view angles and four cube directions.
                for (u32 c = 0; c < kCoatColumns; ++c)
                {
                    const f32 response = GroomFibreAmbientResponse(params, kSinThetaO[c]).Sum()[ch];
                    ASSERT_GT(response, 1.0e-4f) << "the fibre reflects nothing at sinThetaO " << kSinThetaO[c]
                                                 << ", so the ratio below would be meaningless";
                    const f32 coatL = columns[c][ch] / response;
                    ExpectRelative(coatL / lambertianL, 1.0f,
                                   "THE COAT SEES A DIFFERENT SKY FROM THE LAMBERTIAN SURFACE beside it: implied L " +
                                       std::to_string(coatL) + " vs " + std::to_string(lambertianL) +
                                       " at sinThetaO " + std::to_string(kSinThetaO[c]) +
                                       ". A ratio of 0.318 is the extra 1/pi of issue #1450" + channel);
                }
            }
        }
    }
} // namespace OloEngine::Tests
