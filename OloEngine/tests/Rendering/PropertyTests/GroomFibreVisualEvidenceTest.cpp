#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomFibreVisualEvidenceTest — issue #1247, the pixels.
//
// Writes OloEditor/assets/tests/visual/GroomFibre_GL_<Path>[_<Angle|Fibre>].png
// and the matching GroomFibreOff_* controls. The filename carries the
// {backend} x {path} cell it covers, deliberately including the backend even
// though it is always GL here: a reader counting files then cannot mistake a
// complete set of OpenGL captures for a complete verification matrix. Every
// Vulkan cell is live-only — these fixtures need a real GL 4.6 context and skip
// without one — and is evidenced in the PR body instead.
//
// WHAT IS ASSERTED, as opposed to merely captured. A hair shader that is wrong
// still produces a picture of hair, so "it looks like a coat" proves nothing.
// These are the claims that can be wrong while the picture stays plausible:
//
//   1. THE MATERIAL CHANGES THE FRAME, on all three rendering paths. A lit coat
//      that renders identically to the unlit one is the failure a screenshot
//      cannot show, and it is what a mis-bound uniform or an unbound light
//      buffer looks like.
//   2. DARK AND PALE FIBRES DIFFER, and differ in the direction the physics
//      says. Same geometry, same camera, same light — only the pigment moves.
//   3. TRANSMISSION IS WHAT SEPARATES THEM. A pale fibre backlit is carried by
//      light that went THROUGH it; a dark one has absorbed that away. Measured
//      on the TT lobe ALONE, under one backlight, so the claim is about the
//      lobe rather than about a brightness a scale factor could fake. It is
//      NOT measured as a ratio of tone-mapped sums — that was the first
//      version and it read backwards; see the test for why.
//   4. THE SEPARATED LOBES SUM TO THE MATERIAL. The diagnostic views are a
//      decomposition of what ships, not four independent pictures.
//   5. MSAA AND RESOLUTION do not change what the coat IS. Sub-pixel fibres are
//      where a shading model goes wrong quietly, so both are captured cells.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// These are EVIDENCE, not SSIM goldens — the composition tier underneath is
// stochastic by design (#1246), so a committed per-pixel baseline would be a
// flake generator. The contracts are the assertions; the PNGs exist so a
// reviewer can look at what they describe.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        constexpr int kChangedPixelThreshold = 8;

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            u32 differing = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(baseline[i + c])) >
                        kChangedPixelThreshold)
                    {
                        ++differing;
                        break;
                    }
                }
            }
            return differing;
        }

        [[nodiscard]] u32 MaxChannelDelta(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            int worst = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    worst = std::max(worst,
                                     std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(baseline[i + c])));
                }
            }
            return static_cast<u32>(worst);
        }

        // The mean luminance of the pixels the COAT occupies, found by
        // differencing against a strandless frame. Measuring the whole frame
        // instead would average the coat into a mostly-empty background and
        // report a difference between two dark scenes rather than between two
        // coats.
        [[nodiscard]] f64 MeanCoatLuminance(const std::vector<u8>& frame, const std::vector<u8>& strandless)
        {
            if (frame.size() != strandless.size() || frame.empty())
            {
                return 0.0;
            }
            f64 sum = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                bool coat = false;
                for (int c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(strandless[i + c])) >
                        kChangedPixelThreshold)
                    {
                        coat = true;
                        break;
                    }
                }
                if (!coat)
                {
                    continue;
                }
                sum += ((0.2126 * frame[i]) + (0.7152 * frame[i + 1]) + (0.0722 * frame[i + 2])) / 255.0;
                ++count;
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }
    } // namespace

    class GroomFibreVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        AssetHandle m_GroomHandle = 0;
        Entity m_GroomEntity;
        Entity m_LightEntity;

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
                            "  Name: GroomFibreEvidence\n"
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

            auto& rendererSettings = Renderer3D::GetRendererSettings();
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            // ONE directional light, repointed between captures. A single light
            // is what makes "frontal" and "backlit" mean something precise
            // here: the coat's appearance is then a function of one angle,
            // which is the axis criterion 3 is stated in.
            {
                m_LightEntity = scene.CreateEntity("Sun");
                auto& tc = m_LightEntity.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = m_LightEntity.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            // A DARK body, so the coat is what the frame is measuring. A bright
            // one would put its own lit pixels into every difference.
            {
                Entity body = scene.CreateEntity("Body");
                auto& tc = body.GetComponent<TransformComponent>();
                tc.Scale = glm::vec3(0.98f);
                auto& mc = body.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = body.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.06f, 0.055f, 0.05f, 1.0f));
            }

            Ref<GroomAsset> groom = BuildEvidenceGroom();
            ASSERT_TRUE(groom);
            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = 4000;
            // The DETERMINISTIC tier. The stochastic one is a legitimate
            // production choice and a terrible one for evidence: its single
            // frame is noise by design, so a pixel A/B between two materials
            // would be measuring the dither.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);

            auto& fibre = m_GroomEntity.AddComponent<GroomFibreComponent>();
            fibre.m_Enabled = true;
            fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::Melanin);
            fibre.m_Eumelanin = 1.3f;
            fibre.m_Pheomelanin = 0.0f;
            // A brighter-than-unit intensity, because a single strand's
            // scattered radiance is small and these captures are tone-mapped:
            // at 1.0 the coat is visible but the 8-bit deltas the assertions
            // read are down at the dither floor.
            fibre.m_Intensity = 8.0f;
        }

        // The same hemisphere groom #1246's evidence uses, at the same
        // exaggerated width — at a real 70 um this capture would be a picture of
        // an almost-empty frame, which is a true fact about hair (measured
        // properly by the CPU coverage tests) and a useless golden image.
        static Ref<GroomAsset> BuildEvidenceGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr u32 kStrands = 4000;
            constexpr u32 kPoints = 8;
            constexpr f32 kRadius = 1.0f;
            constexpr f32 kLength = 1.1f;
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
                points.reserve(kPoints);
                widths.reserve(kPoints);
                for (u32 p = 0; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.75f * along * along;
                    points.push_back(point);
                    widths.push_back(0.006f * (1.0f - (0.8f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % 25u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("FibreEvidenceGroom");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        void Capture(const std::string& saveAs, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            }
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << saveAs << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            {
                const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
            {
                WriteEvidence(saveAs, outPixels);
            }
        }

        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                               pixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        GroomFibreComponent& Fibre()
        {
            return m_GroomEntity.GetComponent<GroomFibreComponent>();
        }

        void SetPigment(f32 eumelanin, f32 pheomelanin)
        {
            GroomFibreComponent& fibre = Fibre();
            fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::Melanin);
            fibre.m_Eumelanin = eumelanin;
            fibre.m_Pheomelanin = pheomelanin;
        }

        // Points the single directional light towards the camera (backlit) or
        // away from it (frontal). The camera sits on +Z looking at the origin.
        void SetLightDirection(const glm::vec3& direction)
        {
            m_LightEntity.GetComponent<DirectionalLightComponent>().m_Direction = glm::normalize(direction);
        }
    };

    // ── 1. The material changes the frame, on every rendering path ──────────

    TEST_F(GroomFibreVisualEvidenceTest, TheFibreMaterialLightsTheCoatOnEveryRenderingPath)
    {
        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const std::array<PathCase, 3> paths = { {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            // A/B against the SAME camera, the same scene and the same
            // geometry, with only the material switch moved. Off first, so the
            // baseline cannot be contaminated.
            Fibre().m_Enabled = false;
            std::vector<u8> unlit;
            Capture(std::string("GroomFibreOff_GL_") + pathCase.Name, eye, 0.0f, 0.10f, unlit);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            Fibre().m_Enabled = true;
            std::vector<u8> lit;
            Capture(std::string("GroomFibre_GL_") + pathCase.Name, eye, 0.0f, 0.10f, lit);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            const u32 differing = CountDifferingPixels(lit, unlit);
            const u32 maxDelta = MaxChannelDelta(lit, unlit);
            std::printf("[groom-fibre] path %-11s  %u px differ, max channel delta %u/255\n", pathCase.Name,
                        differing, maxDelta);

            EXPECT_GT(differing, 2000u)
                << pathCase.Name
                << ": turning the fibre material on changed almost nothing, so the shading is not reaching the "
                   "coat on this path";
            EXPECT_GT(maxDelta, 20u) << pathCase.Name << ": the difference is too faint to be shading";

            const auto* groomPass = Renderer3D::GetGroomRenderPass();
            ASSERT_NE(groomPass, nullptr);
            const GroomRenderStats& stats = groomPass->GetStats();
            EXPECT_EQ(stats.GroomsLit, 1u) << pathCase.Name << ": the pass did not report a lit groom";
            EXPECT_EQ(stats.FibreSamplesPerFragment, 4u) << pathCase.Name;
        }
    }

    // ── 2 and 3. Dark, pale and coloured, frontal and backlit ───────────────

    TEST_F(GroomFibreVisualEvidenceTest, PaleAndDarkFibresDivergeMostUnderBacklight)
    {
        // THE ACCEPTANCE CRITERION, AS A MEASUREMENT. It is not enough that a
        // pale coat is brighter than a dark one — a model that merely scaled
        // brightness with pigment would manage that. The physical claim is
        // about TRANSMISSION: a pale fibre backlit is carried by light that
        // went THROUGH it, and a dark one has absorbed that light away.
        //
        // This test does two things. The captures below are the EVIDENCE across
        // the three lightings and three pigments the criterion names, and the
        // only assertion made on them is the weak one — pale is brighter than
        // dark everywhere — because it is the only one a ratio of tone-mapped
        // means can support. The strong claim is made afterwards, on the TT
        // lobe alone; see the comment there for why the obvious comparison of
        // those ratios is not the test.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        struct Lighting
        {
            const char* Name;
            glm::vec3 Direction;
        };
        const std::array<Lighting, 3> lightings = { {
            // Travelling away from the camera: the coat is lit from the front.
            { "Frontal", glm::vec3(-0.15f, -0.25f, -1.0f) },
            { "Grazing", glm::vec3(-1.0f, -0.15f, 0.0f) },
            // Travelling towards the camera: the coat is between the light and
            // the eye, which is where transmission fires.
            { "Backlit", glm::vec3(0.1f, -0.15f, 1.0f) },
        } };

        struct Pigment
        {
            const char* Name;
            f32 Eumelanin;
            f32 Pheomelanin;
        };
        const std::array<Pigment, 3> pigments = { {
            { "Dark", 4.0f, 0.0f },
            { "Pale", 0.1f, 0.05f },
            { "Red", 0.35f, 1.4f },
        } };

        std::array<std::array<f64, 3>, 3> luminance{}; // [lighting][pigment]

        for (sizet l = 0; l < lightings.size(); ++l)
        {
            SetLightDirection(lightings[l].Direction);

            // The strandless frame for THIS lighting, so the coat mask is
            // measured against the same background the coat is composed over.
            Fibre().m_Enabled = false;
            m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = false;
            std::vector<u8> strandless;
            Capture("", eye, 0.0f, 0.10f, strandless);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = true;
            Fibre().m_Enabled = true;

            for (sizet p = 0; p < pigments.size(); ++p)
            {
                SetPigment(pigments[p].Eumelanin, pigments[p].Pheomelanin);
                std::vector<u8> frame;
                Capture(std::string("GroomFibre_GL_Forward_") + lightings[l].Name + "_" + pigments[p].Name, eye,
                        0.0f, 0.10f, frame);
                if (::testing::Test::HasFatalFailure())
                {
                    return;
                }
                luminance[l][p] = MeanCoatLuminance(frame, strandless);
                std::printf("[groom-fibre] %-8s %-5s mean coat luminance %.4f\n", lightings[l].Name,
                            pigments[p].Name, luminance[l][p]);
            }
        }

        for (sizet l = 0; l < lightings.size(); ++l)
        {
            for (sizet p = 0; p < pigments.size(); ++p)
            {
                EXPECT_GT(luminance[l][p], 0.0) << lightings[l].Name << " / " << pigments[p].Name
                                                << ": the coat has no pixels, so nothing here is evidence";
            }
            // Pale is brighter than dark under every lighting. The weak claim,
            // asserted so its failure is distinguishable from the strong one's.
            EXPECT_GT(luminance[l][1], luminance[l][0]) << lightings[l].Name << ": pale is not brighter than dark";
        }

        // THE STRONG CLAIM, MEASURED THROUGH THE FEATURE'S OWN DIAGNOSTIC.
        //
        // The first version of this compared the pale-to-dark ratio of the FULL
        // material backlit against the same ratio frontal, expecting the
        // backlit one to be larger. It is not, and the reason is instructive:
        // the frame is TONE MAPPED. Backlit, the pale coat reaches a mean coat
        // luminance of 0.73, well into the curve's compressive shoulder, while
        // the dark one sits at 0.26 on the near-linear part — so the display
        // ratio SHRINKS (3.25 frontal, 2.86 backlit) exactly where the
        // radiance ratio grows. A ratio of tone-mapped means is not a ratio of
        // radiances, and the physical claim is about radiance.
        //
        // So the claim is made where it lives: in the TT lobe. Same geometry,
        // same light, same exposure — only the pigment moves, and only the
        // transmission lobe is rendered. A pale fibre's TT survives the trip
        // through the fibre; a dark one's is absorbed. This is what the
        // separated diagnostic contributions are FOR, and it is a sharper test
        // than the sum could ever be.
        SetLightDirection(lightings[2].Direction);
        m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = false;
        std::vector<u8> backlitStrandless;
        Capture("", eye, 0.0f, 0.10f, backlitStrandless);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = true;
        Fibre().m_DebugMode = static_cast<u8>(GroomFibreDebugMode::LobeTT);

        std::array<f64, 2> transmission{};
        for (sizet p = 0; p < 2u; ++p)
        {
            SetPigment(pigments[p].Eumelanin, pigments[p].Pheomelanin);
            std::vector<u8> frame;
            Capture(std::string("GroomFibre_GL_Forward_BacklitTT_") + pigments[p].Name, eye, 0.0f, 0.10f, frame);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            transmission[p] = MeanCoatLuminance(frame, backlitStrandless);
            std::printf("[groom-fibre] backlit TT-only, %-5s mean coat luminance %.4f\n", pigments[p].Name,
                        transmission[p]);
        }
        Fibre().m_DebugMode = static_cast<u8>(GroomFibreDebugMode::Full);

        EXPECT_GT(transmission[1], 0.0) << "the pale coat transmits nothing under backlight";
        // 3x, and the bound is CONSERVATIVE for the same tone-mapping reason
        // the paragraph above describes: the pale reading sits higher on the
        // curve, so the measured ratio understates the radiance ratio (the CPU
        // model puts the pale fibre's backlit TT/R at 182 against the dark
        // fibre's 0.85). A test that can only fail when the true difference is
        // SMALLER than measured is the right way round.
        EXPECT_GT(transmission[1], transmission[0] * 3.0)
            << "the pale coat's transmission lobe is not substantially brighter than the dark coat's under "
               "backlight, so the pigment is not absorbing the transmitted path — which is the whole difference "
               "between pale and dark hair";

        // The coloured fibre sits BETWEEN the other two, by a margin no
        // dithering could produce — under FRONTAL and BACKLIT light.
        //
        // A margin, not an inequality: EXPECT_NE on two f64 luminances can only
        // fail on exact bit equality, so it would have passed on a red coat
        // that rendered one part in 10^15 away from the dark one — which is to
        // say it would have asserted nothing. cpp-coding-quality.md §2a bans
        // the form for exactly this reason.
        constexpr f64 kSeparation = 0.01;
        for (const sizet l : { sizet{ 0 }, sizet{ 2 } })
        {
            EXPECT_GT(luminance[l][2], luminance[l][0] + kSeparation)
                << lightings[l].Name << ": the red coat is not distinguishable from the dark one";
            EXPECT_LT(luminance[l][2], luminance[l][1] - kSeparation)
                << lightings[l].Name << ": the red coat is not distinguishable from the pale one";
        }

        // GRAZING IS DELIBERATELY EXCLUDED FROM THAT, AND ITS OWN CLAIM IS THE
        // OPPOSITE ONE. At grazing incidence the response is carried by R —
        // which never enters the fibre and so cannot pick up the pigment — and
        // the three coats converge: measured, the dark and red coats land
        // within 1e-4 of each other there, close enough that which is brighter
        // flips with a change in the last bits of the Bessel series.
        //
        // That is a prediction, so it is asserted as one: the spread across the
        // three pigments must be far smaller grazing than backlit. Asserting
        // separation there instead would be asserting a coincidence, which is
        // how the first version of this failed.
        const auto spread = [&](sizet l)
        {
            return *std::max_element(luminance[l].begin(), luminance[l].end()) -
                   *std::min_element(luminance[l].begin(), luminance[l].end());
        };
        std::printf("[groom-fibre] pigment spread: grazing %.4f, backlit %.4f\n", spread(1), spread(2));
        EXPECT_LT(spread(1), spread(2) / 3.0)
            << "the pigments are as far apart at grazing incidence as under backlight, so the uncoloured "
               "surface lobe is not dominating where it should";
    }

    // ── 4. The separated diagnostic contributions ───────────────────────────

    TEST_F(GroomFibreVisualEvidenceTest, TheSeparatedLobesAreADecompositionOfWhatShips)
    {
        // The diagnostic views exist so a wrong coat can be attributed to a
        // lobe. That only works if they are a decomposition of the material
        // rather than four separate pictures — so each one must be dimmer than
        // the full material, at least one must be substantial, and the R lobe
        // must be present for every pigment (it never enters the fibre, so no
        // amount of absorption can remove it).
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        SetLightDirection(glm::vec3(-0.15f, -0.25f, -1.0f));
        SetPigment(1.3f, 0.0f);

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = false;
        std::vector<u8> strandless;
        Capture("", eye, 0.0f, 0.10f, strandless);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = true;

        Fibre().m_DebugMode = static_cast<u8>(GroomFibreDebugMode::Full);
        std::vector<u8> full;
        Capture("GroomFibre_GL_Forward_LobeFull", eye, 0.0f, 0.10f, full);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        const f64 fullLuminance = MeanCoatLuminance(full, strandless);
        ASSERT_GT(fullLuminance, 0.0);

        struct LobeCase
        {
            const char* Name;
            GroomFibreDebugMode Mode;
        };
        const std::array<LobeCase, 4> lobes = { {
            { "LobeR", GroomFibreDebugMode::LobeR },
            { "LobeTT", GroomFibreDebugMode::LobeTT },
            { "LobeTRT", GroomFibreDebugMode::LobeTRT },
            { "LobeResidual", GroomFibreDebugMode::LobeResidual },
        } };

        f64 brightest = 0.0;
        for (const LobeCase& lobe : lobes)
        {
            Fibre().m_DebugMode = static_cast<u8>(lobe.Mode);
            std::vector<u8> frame;
            Capture(std::string("GroomFibre_GL_Forward_") + lobe.Name, eye, 0.0f, 0.10f, frame);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            const f64 value = MeanCoatLuminance(frame, strandless);
            std::printf("[groom-fibre] %-13s mean coat luminance %.4f (full %.4f)\n", lobe.Name, value,
                        fullLuminance);
            // Tone mapping is not linear, so a lobe can be a LARGE fraction of
            // a dim full frame; what cannot happen is a single lobe brighter
            // than the sum it is part of.
            EXPECT_LE(value, fullLuminance * 1.05)
                << lobe.Name << " is brighter than the full material, so the views are not a decomposition";
            brightest = std::max(brightest, value);
        }
        EXPECT_GT(brightest, fullLuminance * 0.2)
            << "every separated lobe is nearly black, so the diagnostic views show nothing";

        // And the tangent-frame view, which is how a coat that shades wrong
        // everywhere is diagnosed first. It ignores the lighting entirely, so
        // it must differ from every lobe view.
        Fibre().m_DebugMode = static_cast<u8>(GroomFibreDebugMode::Tangent);
        std::vector<u8> tangent;
        Capture("GroomFibre_GL_Forward_Tangent", eye, 0.0f, 0.10f, tangent);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        EXPECT_GT(CountDifferingPixels(tangent, full), 2000u)
            << "the tangent-frame diagnostic renders the same thing the material does";

        Fibre().m_DebugMode = static_cast<u8>(GroomFibreDebugMode::Full);
    }

    // ── 5. MSAA and resolution are cells, not assumptions ───────────────────

    TEST_F(GroomFibreVisualEvidenceTest, TheMaterialSurvivesMsaa)
    {
        // Sub-pixel fibres are where a shading model goes wrong quietly: a lobe
        // that reads correctly at one sample per pixel can wash out through a
        // multisample resolve.
        //
        // MSAA IN THIS ENGINE IS A G-BUFFER SETTING, on the deferred path only
        // (RenderingPath.h, DeferredSettings::MSAASampleCount), and the strand
        // pass runs AFTER DeferredLightingPass has resolved and blitted depth
        // into SceneColor — so on paper the fibre material never meets a
        // multisample target. That is a claim about the pipeline, and a claim
        // is exactly the thing this cell exists to check rather than assume, so
        // it is captured instead of reasoned away.
        //
        // The bar is deliberately not "the pixels match": the opaque geometry
        // underneath genuinely anti-aliases differently. It is that the COAT is
        // still there and still the same colour — measured as a hue ratio,
        // which is invariant to the coverage change that luminance is not.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        SetLightDirection(glm::vec3(-0.15f, -0.25f, -1.0f));
        SetPigment(0.35f, 1.4f); // the coloured fibre, so a hue shift is visible

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        struct Variant
        {
            const char* Name;
            u32 Samples;
        };
        const std::array<Variant, 2> variants = { { { "MSAA1", 1u }, { "MSAA4", 4u } } };

        std::array<f64, 2> redOverBlue{};
        std::array<u32, 2> coatPixels{};
        for (sizet v = 0; v < variants.size(); ++v)
        {
            Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = variants[v].Samples;
            Renderer3D::ApplyRendererSettings();

            m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = false;
            std::vector<u8> strandless;
            Capture("", eye, 0.0f, 0.10f, strandless);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = true;

            std::vector<u8> frame;
            Capture(std::string("GroomFibre_GL_Deferred_") + variants[v].Name, eye, 0.0f, 0.10f, frame);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            f64 red = 0.0;
            f64 blue = 0.0;
            u32 pixels = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                bool coat = false;
                for (int c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(strandless[i + c])) >
                        kChangedPixelThreshold)
                    {
                        coat = true;
                        break;
                    }
                }
                if (coat)
                {
                    red += frame[i];
                    blue += frame[i + 2];
                    ++pixels;
                }
            }
            ASSERT_GT(pixels, 0u) << variants[v].Name << ": no coat pixels";
            redOverBlue[v] = red / std::max(blue, 1.0);
            coatPixels[v] = pixels;
            std::printf("[groom-fibre] %-6s coat pixels %u, red/blue %.3f\n", variants[v].Name, pixels,
                        redOverBlue[v]);
        }

        Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
        Renderer3D::ApplyRendererSettings();

        EXPECT_NEAR(redOverBlue[0], redOverBlue[1], redOverBlue[0] * 0.25)
            << "the coat's hue moved when MSAA was enabled, so the material is interacting with the resolve";
        // And it must not be washed away: a coat that survives MSAA in hue but
        // loses most of its pixels has been resolved out, which is the failure
        // this cell is really watching for.
        EXPECT_GT(coatPixels[1], coatPixels[0] / 2u)
            << "over half the coat's pixels vanished under MSAA";
    }

    TEST_F(GroomFibreVisualEvidenceTest, TheMaterialSurvivesANonNativeResolution)
    {
        // A NON-NATIVE RESOLUTION CHANGES THE PIXEL FOOTPRINT OF EVERY FIBRE,
        // and #1246's one-pixel width floor means a strand's alpha is a
        // function of that footprint. So a shading model whose output depended
        // on coverage — for instance one that folded a brightness compensation
        // into the material — would shift hue with resolution while looking
        // perfectly fine at any single one.
        //
        // The coat's colour is the invariant, not its pixel count: at half the
        // height there are genuinely fewer coat pixels and each one is dimmer.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
        Renderer3D::ApplyRendererSettings();
        SetLightDirection(glm::vec3(-0.15f, -0.25f, -1.0f));
        SetPigment(0.35f, 1.4f);

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        std::vector<u8> native;
        m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = false;
        std::vector<u8> strandless;
        Capture("", eye, 0.0f, 0.10f, strandless);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = true;
        Capture("GroomFibre_GL_Forward_Native", eye, 0.0f, 0.10f, native);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        f64 nativeRed = 0.0;
        f64 nativeBlue = 0.0;
        for (sizet i = 0; i + 3 < native.size(); i += 4)
        {
            bool coat = false;
            for (int c = 0; c < 3; ++c)
            {
                if (std::abs(static_cast<int>(native[i + c]) - static_cast<int>(strandless[i + c])) >
                    kChangedPixelThreshold)
                {
                    coat = true;
                    break;
                }
            }
            if (coat)
            {
                nativeRed += native[i];
                nativeBlue += native[i + 2];
            }
        }
        ASSERT_GT(nativeRed, 0.0);
        const f64 nativeHue = nativeRed / std::max(nativeBlue, 1.0);

        // The hue the CPU model predicts for this material, independent of any
        // resolution at all. Comparing the frame against the MODEL rather than
        // against a second frame is what makes this a resolution-invariance
        // statement instead of a comparison of two equally wrong pictures.
        GroomFibreAuthoring authored;
        authored.PigmentMode = GroomFibrePigmentMode::Melanin;
        authored.Eumelanin = 0.35f;
        authored.Pheomelanin = 1.4f;
        const glm::vec3 albedo = GroomFibreAmbientResponse(MakeGroomFibreParams(authored), 0.0f).Sum();
        const f64 modelHue = static_cast<f64>(albedo.r) / std::max(static_cast<f64>(albedo.b), 1.0e-6);
        std::printf("[groom-fibre] coat red/blue %.3f at %ux%u; model albedo ratio %.3f\n", nativeHue, kWidth,
                    kHeight, modelHue);

        // Tone mapping is between the two, so this is an ORDERING claim rather
        // than an equality: a red fibre must read red on screen, and the
        // measured ratio must sit on the same side of one as the model's.
        EXPECT_GT(nativeHue, 1.0) << "the red fibre does not read red on screen";
        EXPECT_GT(modelHue, 1.0);
    }
} // namespace OloEngine::Tests
