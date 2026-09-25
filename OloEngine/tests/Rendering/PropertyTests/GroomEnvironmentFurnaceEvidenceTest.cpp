#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomEnvironmentFurnaceEvidenceTest — issue #1450, the pixels.
//
// A coat and a Lambertian sphere under a UNIFORM SKY and nothing else: no
// light, no skybox draw, no AO. The sphere's albedo is the coat's authored
// base colour, which in BaseColor pigment mode IS the fibre's head-on albedo,
// so the two should read at about the same radiance. Before #1450 the coat's
// environment term divided the (already E / pi) irradiance cube by pi again,
// and the coat read about a third of the sphere.
//
// WHAT IS ASSERTED, on the HDR SceneColor (before tone mapping):
//
//   1. THE COAT SEES THE SKY AT ITS RADIANCE. The coat's bright end (the 90th
//      percentile of its green channel) over sky radiance times head-on albedo
//      lands near 1. A strand whose tangent faces the viewer reflects less (the
//      cos(theta_o) in GroomFibreAmbientResponse), which is why this is a
//      percentile and not the mean. The /pi reads about 0.3.
//   2. THE COAT MATCHES THE LAMBERTIAN SPHERE beside it, as a ratio of that
//      percentile to the sphere's centre.
//   3. NOTHING BUT THE SKY LIGHTS IT. With IBL off, the coat is black, so 1
//      and 2 measure the environment term and not a stray light.
//   4. THE SKY'S IBL INTENSITY SCALES THE COAT AS IT SCALES THE SPHERE.
//
// On all three rendering paths and for both producers the scene can pick (the
// importance-sampled convolution and the L2 SH projection). The coat draws
// through GroomStrand.glsl on every path, so these cells show the one term is
// wired identically everywhere, not three copies of it.
//
// Writes OloEditor/assets/tests/visual/GroomEnvFurnace_GL_<Path>_<Producer>.png.
// Every Vulkan cell is live-only (these fixtures need a GL 4.6 context and skip
// without one) and is evidenced in the PR body.
// =============================================================================

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        // The sky, and the colour both subjects share.
        constexpr glm::vec3 kSkyRadiance{ 1.0f, 0.95f, 0.85f };
        constexpr glm::vec3 kAlbedo{ 0.55f, 0.42f, 0.30f };

        // Where the two subjects sit, and the camera that frames them.
        constexpr glm::vec3 kSpherePosition{ -1.3f, 0.0f, 0.0f };
        constexpr glm::vec3 kGroomPosition{ 1.2f, -0.2f, 0.0f };
        constexpr glm::vec3 kEye{ 0.0f, 0.2f, 5.0f };
        constexpr f32 kFovDegrees = 60.0f;

        struct Producer
        {
            const char* Name;
            bool SphericalHarmonics;
        };
        constexpr std::array<Producer, 2> kProducers{ {
            { "Convolution", false },
            { "SH", true },
        } };

        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        constexpr std::array<PathCase, 3> kPaths{ {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };

        [[nodiscard]] Ref<TextureCubemap> MakeUniformSky(const glm::vec3& radiance)
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
                face[i + 0u] = radiance.r;
                face[i + 1u] = radiance.g;
                face[i + 2u] = radiance.b;
                face[i + 3u] = 1.0f;
            }
            for (u32 f = 0; f < 6u; ++f)
            {
                if (!sky->SetFaceDataMip(f, 0, face.data(), static_cast<u32>(face.size() * sizeof(f32))))
                {
                    return nullptr;
                }
            }
            sky->GenerateMipmaps();
            return sky;
        }

        // The pixel a world point projects to, for the camera Capture uses:
        // at kEye, looking down -Z, no rotation.
        [[nodiscard]] glm::ivec2 ProjectToPixel(const glm::vec3& world)
        {
            const glm::vec3 view = world - kEye;
            const f32 tanHalf = std::tan(glm::radians(kFovDegrees) * 0.5f);
            const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);
            const f32 ndcX = (view.x / -view.z) / (tanHalf * aspect);
            const f32 ndcY = (view.y / -view.z) / tanHalf;
            return { static_cast<i32>((ndcX * 0.5f + 0.5f) * static_cast<f32>(kWidth)),
                     static_cast<i32>((ndcY * 0.5f + 0.5f) * static_cast<f32>(kHeight)) };
        }

        struct Frame
        {
            std::vector<f32> Hdr; // RGBA, bottom-up, straight from SceneColor
        };

        // The green channel of every pixel the coat covers, found by
        // differencing against the same frame with the strands off. Only the
        // coat's half of the frame is searched, so the sphere cannot leak in.
        [[nodiscard]] std::vector<f32> CoatGreen(const Frame& coat, const Frame& strandless)
        {
            std::vector<f32> values;
            const i32 splitX = (ProjectToPixel(kSpherePosition).x + ProjectToPixel(kGroomPosition).x) / 2;
            for (u32 y = 0; y < kHeight; ++y)
            {
                for (u32 x = static_cast<u32>(std::max(splitX, 0)); x < kWidth; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                    f32 delta = 0.0f;
                    for (int c = 0; c < 3; ++c)
                    {
                        delta = std::max(delta, std::abs(coat.Hdr[i + c] - strandless.Hdr[i + c]));
                    }
                    if (delta > 1.0e-3f)
                    {
                        values.push_back(coat.Hdr[i + 1u]);
                    }
                }
            }
            return values;
        }

        [[nodiscard]] f32 Percentile(std::vector<f32> values, f32 p)
        {
            if (values.empty())
            {
                return 0.0f;
            }
            const sizet k = std::min(values.size() - 1u, static_cast<sizet>(p * static_cast<f32>(values.size())));
            std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
            return values[k];
        }

        // The mean green of a 7x7 patch near the sphere's projected centre,
        // where it faces the camera. Offset by 12 px (about 8 degrees of
        // normal): the frame carries a bright dot at the exact centre that is
        // not surface shading, and read there the sphere measured x1.29 instead
        // of x1.02.
        [[nodiscard]] f32 SphereCentreGreen(const Frame& frame)
        {
            const glm::ivec2 centre = ProjectToPixel(kSpherePosition) + glm::ivec2(-12, 12);
            f32 sum = 0.0f;
            u32 count = 0;
            for (i32 dy = -3; dy <= 3; ++dy)
            {
                for (i32 dx = -3; dx <= 3; ++dx)
                {
                    const i32 x = centre.x + dx;
                    const i32 y = centre.y + dy;
                    if (x < 0 || y < 0 || x >= static_cast<i32>(kWidth) || y >= static_cast<i32>(kHeight))
                    {
                        continue;
                    }
                    sum += frame.Hdr[(static_cast<sizet>(y) * kWidth + static_cast<sizet>(x)) * 4u + 1u];
                    ++count;
                }
            }
            return count ? sum / static_cast<f32>(count) : 0.0f;
        }
    } // namespace

    class GroomEnvironmentFurnaceEvidenceTest : public RendererAttachedTest
    {
      public:
        Entity m_GroomEntity;
        Entity m_SkyEntity;
        Ref<TextureCubemap> m_Sky;
        bool m_PreviousAliasing = false;

        void BuildScene() override
        {
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                const fs::path projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: GroomEnvFurnaceEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& settings = Renderer3D::GetRendererSettings();
            settings.EditorDebugDrawsEnabled = true;
            settings.ShowComponentGizmos = false;
            settings.ShowGrid = false;
            settings.ShowWorldAxisHelper = false;

            // SceneColor is read after the frame, and a transient's texture can
            // hold a later pass's output by then. Aliasing off makes the name
            // mean what it says.
            m_PreviousAliasing = Levers::DisableTransientAliasing();
            Levers::SetDisableTransientAliasing(true);

            m_Sky = MakeUniformSky(kSkyRadiance);
            ASSERT_TRUE(m_Sky);
            m_SkyEntity = scene.CreateEntity("UniformSky");
            auto& env = m_SkyEntity.AddComponent<EnvironmentMapComponent>();
            // NO SKYBOX DRAW. The background stays the clear colour, so a coat
            // pixel blended with its background at an edge is darker than the
            // coat, never brighter; the percentile below relies on that.
            env.m_EnableSkybox = false;
            env.m_EnableIBL = true;
            SetProducer(kProducers[0]);

            {
                Entity sphere = scene.CreateEntity("LambertianReference");
                auto& tc = sphere.GetComponent<TransformComponent>();
                tc.Translation = kSpherePosition;
                tc.Scale = glm::vec3(0.9f);
                auto& mc = sphere.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = sphere.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(kAlbedo, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            Ref<GroomAsset> groom = BuildFurnaceGroom();
            ASSERT_TRUE(groom);
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(handle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            m_GroomEntity.GetComponent<TransformComponent>().Translation = kGroomPosition;
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = handle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = 4000;
            // The deterministic tier: the stochastic one is noise per frame by
            // design, and this test reads single pixels.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);

            auto& fibre = m_GroomEntity.AddComponent<GroomFibreComponent>();
            fibre.m_Enabled = true;
            fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::BaseColor);
            fibre.m_BaseColor = kAlbedo;
            // Unit intensity: the physical scale is the thing under test.
            fibre.m_Intensity = 1.0f;
        }

        // The base fixture restores the renderer settings (the path included);
        // the lever is process-wide state it does not know about.
        void TearDown() override
        {
            Levers::SetDisableTransientAliasing(m_PreviousAliasing);
            RendererAttachedTest::TearDown();
        }

        // The EnvironmentMap is rebuilt per producer, through the same
        // CreateFromCubemap + IBLConfiguration the scene's own loader uses.
        void SetProducer(const Producer& producer)
        {
            IBLConfiguration config;
            config.UseSphericalHarmonics = producer.SphericalHarmonics;
            // A generated cubemap has no file to key a cache entry on.
            config.UseDiskCache = false;
            auto& env = m_SkyEntity.GetComponent<EnvironmentMapComponent>();
            env.m_UseSphericalHarmonics = producer.SphericalHarmonics;
            env.m_EnvironmentMap = EnvironmentMap::CreateFromCubemap(m_Sky, config);
            ASSERT_TRUE(env.m_EnvironmentMap) << producer.Name << ": EnvironmentMap::CreateFromCubemap failed";
            ASSERT_TRUE(env.m_EnvironmentMap->HasIBL()) << producer.Name << ": the environment baked no IBL";
        }

        // A dense hemisphere of short strands, facing every way, so a good share
        // of them run across the view (sin(theta_o) near 0) wherever the camera
        // sits. Wide enough that most coat pixels are fully covered.
        static Ref<GroomAsset> BuildFurnaceGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr u32 kStrands = 4000;
            constexpr u32 kPoints = 6;
            constexpr f32 kRadius = 0.8f;
            constexpr f32 kLength = 0.6f;
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0; s < kStrands; ++s)
            {
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kStrands);
                const f32 cosTheta = 1.0f - t;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                const f32 phi = kGoldenAngle * static_cast<f32>(s);
                const glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const glm::vec3 root = normal * kRadius;

                std::vector<glm::vec3> points;
                std::vector<f32> widths;
                for (u32 p = 0; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.8f * along * along;
                    points.push_back(point);
                    widths.push_back(0.012f * (1.0f - (0.5f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % 25u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("EnvFurnaceGroom");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        // Renders, reads SceneColor as float, and (when named) writes the
        // tone-mapped frame as the reviewer's PNG.
        void Capture(const std::string& saveAs, Frame& out)
        {
            EditorCamera camera(kFovDegrees, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(kEye, 0.0f, 0.0f);

            RunEditorFrames(camera, 2);

            auto hdr = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(hdr) << "no SceneColor framebuffer for '" << saveAs << "'";
            ReadbackRgbaFloat(hdr->GetColorAttachmentRendererID(0), kWidth, kHeight, out.Hdr);
            ASSERT_EQ(out.Hdr.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            if (saveAs.empty())
            {
                return;
            }
            auto ldr = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!ldr)
            {
                ldr = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            ASSERT_TRUE(ldr) << "no composited framebuffer for '" << saveAs << "'";
            std::vector<u8> pixels;
            ReadbackRgba8(ldr->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> row(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = pixels.data() + (static_cast<sizet>(y) * rowBytes);
                u8* bottom = pixels.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                std::memcpy(row.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, row.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (saveAs + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       pixels.data(), static_cast<int>(rowBytes)),
                      0)
                << "stbi_write_png failed to write '" << path << "'";
        }

        void SetStrandsVisible(bool visible)
        {
            m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = visible;
        }
    };

    TEST_F(GroomEnvironmentFurnaceEvidenceTest, TheCoatReadsTheSkyAtTheSphereBesideItsRadiance)
    {
        for (const Producer& producer : kProducers)
        {
            SetProducer(producer);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            for (const PathCase& pathCase : kPaths)
            {
                const std::string cell = std::string(pathCase.Name) + "_" + producer.Name;
                SCOPED_TRACE(cell);
                Renderer3D::GetRendererSettings().Path = pathCase.Path;
                Renderer3D::ApplyRendererSettings();

                Frame strandless;
                SetStrandsVisible(false);
                Capture("", strandless);
                SetStrandsVisible(true);
                Frame lit;
                Capture("GroomEnvFurnace_GL_" + cell, lit);
                if (::testing::Test::HasFatalFailure())
                {
                    return;
                }

                const std::vector<f32> coat = CoatGreen(lit, strandless);
                const f32 coatBright = Percentile(coat, 0.9f);
                const f32 sphere = SphereCentreGreen(lit);
                const f32 expected = kSkyRadiance.g * kAlbedo.g;
                std::printf("[groom-env-furnace] %-22s coat px %6zu  p90 %.4f (x%.3f of L*albedo)  sphere %.4f "
                            "(x%.3f)  coat/sphere %.3f\n",
                            cell.c_str(), coat.size(), coatBright, coatBright / expected, sphere, sphere / expected,
                            sphere > 0.0f ? coatBright / sphere : 0.0f);

                // A coat that drew almost nothing would pass any ratio below.
                EXPECT_GT(coat.size(), 5000u) << "the coat barely rendered";

                // The reference itself: a dielectric body keeps (1 - F) of
                // albedo * L head-on, plus its rough specular lobe's small share
                // of the sky. Measured x1.02.
                EXPECT_GT(sphere / expected, 0.90f) << "the Lambertian sphere reads darker than the sky allows";
                EXPECT_LT(sphere / expected, 1.20f) << "the Lambertian sphere reads brighter than the sky allows";

                // 1. The coat's bright end is the head-on albedo times the sky.
                //    Measured 0.92; the extra 1/pi of #1450 read 0.29.
                EXPECT_GT(coatBright / expected, 0.70f)
                    << "THE COAT IS TOO DARK FOR THE SKY LIGHTING IT: its bright end is " << coatBright / expected
                    << " of sky radiance times albedo. About 0.32 is issue #1450's extra 1/pi.";
                EXPECT_LT(coatBright / expected, 1.10f)
                    << "the coat is brighter than a fibre of this albedo can be under this sky";

                // 2. And so it matches the sphere of the same albedo. Measured
                //    0.90 with the fix and 0.29 without it.
                EXPECT_GT(coatBright / sphere, 0.75f) << "the coat reads far darker than the Lambertian sphere";
                EXPECT_LT(coatBright / sphere, 1.15f) << "the coat reads brighter than the Lambertian sphere";
            }
        }
    }

    TEST_F(GroomEnvironmentFurnaceEvidenceTest, TheSkysIblIntensityScalesTheCoatWithTheSphere)
    {
        // The sky's IBL intensity scales every lit surface's IBL rung
        // (Renderer3D::GetGlobalIBLIntensity). It must scale the coat's
        // environment term too, or the slider brightens a body and leaves its
        // coat behind. The strand pass carries it in FibreLobe.w.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        auto& env = m_SkyEntity.GetComponent<EnvironmentMapComponent>();

        Frame strandless;
        SetStrandsVisible(false);
        Capture("", strandless);
        SetStrandsVisible(true);

        std::array<f32, 2> coat{};
        std::array<f32, 2> sphere{};
        constexpr std::array<f32, 2> kIntensities{ 1.0f, 2.0f };
        for (sizet i = 0; i < kIntensities.size(); ++i)
        {
            env.m_IBLIntensity = kIntensities[i];
            Frame frame;
            Capture(i == 0 ? std::string() : std::string("GroomEnvFurnaceIbl2_GL_Forward"), frame);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            coat[i] = Percentile(CoatGreen(frame, strandless), 0.9f);
            sphere[i] = SphereCentreGreen(frame);
        }
        std::printf("[groom-env-furnace] IBL intensity 1 -> 2: coat %.4f -> %.4f (x%.3f), sphere %.4f -> %.4f "
                    "(x%.3f)\n",
                    coat[0], coat[1], coat[0] > 0.0f ? coat[1] / coat[0] : 0.0f, sphere[0], sphere[1],
                    sphere[0] > 0.0f ? sphere[1] / sphere[0] : 0.0f);

        ASSERT_GT(coat[0], 0.05f) << "the coat did not render";
        ASSERT_GT(sphere[0], 0.05f) << "the sphere did not render";
        EXPECT_NEAR(sphere[1] / sphere[0], 2.0f, 0.1f) << "the sphere's IBL rung did not follow the intensity";
        EXPECT_NEAR(coat[1] / coat[0], 2.0f, 0.1f)
            << "THE COAT IGNORES THE SKY'S IBL INTENSITY while the sphere beside it doubles";
    }

    TEST_F(GroomEnvironmentFurnaceEvidenceTest, WithTheSkyOffNothingLightsTheCoat)
    {
        // The control for the test above: its ratios mean something only if the
        // sky is the coat's only light.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        Frame strandlessLit;
        SetStrandsVisible(false);
        Capture("", strandlessLit);
        SetStrandsVisible(true);
        Frame lit;
        Capture("", lit);
        const f32 litBright = Percentile(CoatGreen(lit, strandlessLit), 0.9f);

        m_SkyEntity.GetComponent<EnvironmentMapComponent>().m_EnableIBL = false;
        Frame dark;
        Capture("GroomEnvFurnaceSkyOff_GL_Forward", dark);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // The same pixels as the lit coat, read in the unlit frame.
        f32 darkBright = 0.0f;
        {
            std::vector<f32> values;
            for (sizet i = 0; i + 3u < lit.Hdr.size(); i += 4u)
            {
                const f32 delta = std::abs(lit.Hdr[i + 1u] - strandlessLit.Hdr[i + 1u]);
                if (delta > 1.0e-3f && (i / 4u) % kWidth > kWidth / 2u)
                {
                    values.push_back(dark.Hdr[i + 1u]);
                }
            }
            darkBright = Percentile(values, 0.9f);
        }
        std::printf("[groom-env-furnace] sky off: coat p90 %.5f vs %.4f with the sky\n", darkBright, litBright);

        ASSERT_GT(litBright, 0.05f) << "the lit arm rendered no coat, so this control proves nothing";
        EXPECT_LT(darkBright, 0.02f * litBright)
            << "the coat is lit with the sky off, so something other than the environment term reaches it";
    }
} // namespace OloEngine::Tests
