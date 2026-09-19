#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomCoatShadowVisualEvidenceTest — issue #1248, the pixels.
//
// Writes OloEditor/assets/tests/visual/GroomCoatShadow_GL_<Path>[_<Case>].png
// and the matching GroomCoatShadowOff_* controls. The filename carries the
// {backend} x {path} cell it covers, deliberately including the backend even
// though it is always GL here: a reader counting files then cannot mistake a
// complete set of OpenGL captures for a complete verification matrix. Every
// Vulkan cell is live-only — these fixtures need a real GL 4.6 context and skip
// without one — and is evidenced in the PR body instead.
//
// WHAT IS ASSERTED, as opposed to merely captured. A coat that is wrong still
// produces a picture of a coat, so "it looks shadowed" proves nothing. These
// are the claims that can be wrong while the picture stays plausible:
//
//   1. THE COAT SHADOW CHANGES THE FRAME, on all three rendering paths, and it
//      DARKENS. A representation that is bound but never sampled, a routing
//      lane left inactive, or a volume that baked empty all render exactly like
//      the control — and all three are invisible in a screenshot.
//   2. A DENSER COAT IS DARKER THAN A SPARSE ONE. The term has to respond to
//      the coat's actual density rather than being a constant tint, which is
//      what a mis-scaled march or a dropped density channel would look like.
//   3. PALE AND DARK COATS STAY DISTINCT under the new term. The failure this
//      guards is the coat shadow swamping the pigment — criterion 2 asks that
//      undercoat, guard hairs and transmission stay distinct, and a shadow term
//      strong enough to flatten them would satisfy criterion 1 and break this.
//   4. AN ANIMATED LIGHT CAUSES NO REBUILDS. This is criterion 4 measured as a
//      COUNTER rather than looked at: the selected representation is
//      light-independent, so a moving light must cost exactly zero rebuilds. A
//      per-light representation would fail this, which is the whole reason the
//      bake-off rejected one.
//   5. MSAA AND A NON-NATIVE RESOLUTION do not change what the coat IS.
//      Sub-pixel fibres are where a shading model goes wrong quietly, so both
//      are captured cells rather than assumptions.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// These are EVIDENCE, not SSIM goldens — the composition tier underneath can be
// stochastic by design (#1246) — so the contracts are the assertions and the
// PNGs exist so a reviewer can look at what they describe.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomCoatShadowTechnique.h"
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        constexpr int kChangedPixelThreshold = 8;

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            u32 count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(baseline[i + c])) >
                        kChangedPixelThreshold)
                    {
                        ++count;
                        break;
                    }
                }
            }
            return count;
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
                    worst = std::max(worst, std::abs(static_cast<int>(frame[i + c]) -
                                                     static_cast<int>(baseline[i + c])));
                }
            }
            return static_cast<u32>(worst);
        }

        // The total luminance the COAT contributes, measured as the signed sum
        // against a strandless frame over every pixel with no threshold.
        //
        // Summed rather than thresholded for the reason #1247's CoatHue records:
        // which pixels clear a fixed absolute threshold depends on how many
        // strands land in a pixel, so a thresholded mask silently selects for
        // the densest part of the coat and the selection effect alone moves the
        // number by tens of per cent between resolutions.
        [[nodiscard]] f64 CoatLuminance(const std::vector<u8>& frame, const std::vector<u8>& strandless)
        {
            if (frame.size() != strandless.size())
            {
                return 0.0;
            }
            f64 total = 0.0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                // Rec. 709 luma, on the tone-mapped frame. It is a relative
                // measure between two frames of the same scene, so the transfer
                // function cancels well enough for an ordering — which is all
                // that is asserted. A RATIO of two tone-mapped means would not
                // be safe (groom-fibre-scattering.md rule 13), and none is
                // taken here.
                const f64 lit = 0.2126 * frame[i] + 0.7152 * frame[i + 1] + 0.0722 * frame[i + 2];
                const f64 base =
                    0.2126 * strandless[i] + 0.7152 * strandless[i + 1] + 0.0722 * strandless[i + 2];
                total += lit - base;
            }
            return total;
        }
    } // namespace

    class GroomCoatShadowVisualEvidenceTest : public RendererAttachedTest
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
                            "  Name: GroomCoatShadowEvidence\n"
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

            // ONE directional light, repointed between captures, for the same
            // reason #1247's fixture uses one: the coat's appearance is then a
            // function of a single angle, which is the axis the criteria are
            // stated in.
            {
                m_LightEntity = scene.CreateEntity("Sun");
                auto& tc = m_LightEntity.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = m_LightEntity.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            // A DARK body, so the coat is what the frame is measuring.
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
            groomComponent.m_MaxRenderStrands = 6000;
            // The DETERMINISTIC tier, for the same reason #1247's evidence uses
            // it: the stochastic one is noise by design, so a pixel A/B would
            // be measuring the dither rather than the shadow.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);

            // A fibre material, because coat shadowing attenuates LIGHTING and
            // #1246's neutral ramp has no lighting to attenuate. Pale rather
            // than dark: a pale coat is where inter-fibre occlusion is most
            // visible, because a dark one has already absorbed the light the
            // shadow would have removed.
            auto& fibre = m_GroomEntity.AddComponent<GroomFibreComponent>();
            fibre.m_Enabled = true;
            fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::Melanin);
            fibre.m_Eumelanin = 0.1f;
            fibre.m_Pheomelanin = 0.05f;
            fibre.m_Intensity = 12.0f;

            auto& coat = m_GroomEntity.AddComponent<GroomCoatShadowComponent>();
            coat.m_Enabled = true;
            coat.m_Mode = static_cast<u8>(GroomCoatShadow::CoatShadowMode::AnisotropicDensityVolume);
            coat.m_Resolution = 64;
            coat.m_StepVoxels = 3.0f;
        }

        static Ref<GroomAsset> BuildEvidenceGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr u32 kStrands = 6000;
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

            builder.SetName("CoatShadowEvidenceGroom");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        void Capture(const std::string& saveAs, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels, u32 width = kWidth, u32 height = kHeight)
        {
            EditorCamera camera(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
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

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), width, height, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(width) * height * 4u);

            {
                const sizet rowBytes = static_cast<sizet>(width) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < height / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(height - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
            {
                WriteEvidenceAt(saveAs, width, height, outPixels);
            }
        }

        static void WriteEvidenceAt(const std::string& name, u32 width, u32 height, const std::vector<u8>& pixels)
        {
            // RELATIVE, and that is load-bearing: the renderer's initialisation
            // leaves the process cwd inside OloEditor/, so a path that looks
            // like it lands in the repo root actually lands in the editor's
            // asset tree — which is where these belong. See
            // GroomFibreVisualEvidenceTest, which does the same.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               pixels.data(), static_cast<int>(width) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        GroomCoatShadowComponent& Coat()
        {
            return m_GroomEntity.GetComponent<GroomCoatShadowComponent>();
        }

        GroomComponent& Groom()
        {
            return m_GroomEntity.GetComponent<GroomComponent>();
        }

        void SetLightDirection(const glm::vec3& direction)
        {
            m_LightEntity.GetComponent<DirectionalLightComponent>().m_Direction = glm::normalize(direction);
        }

        [[nodiscard]] const GroomCoatShadowStats& CoatStats() const
        {
            static const GroomCoatShadowStats kEmpty{};
            const auto* pass = Renderer3D::GetGroomRenderPass();
            return pass != nullptr ? pass->GetStats().CoatShadow : kEmpty;
        }
    };

    // ── 1. The coat shadow reaches the screen, on every rendering path ──────

    TEST_F(GroomCoatShadowVisualEvidenceTest, TheCoatShadowDarkensTheCoatOnEveryRenderingPath)
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

            // A/B against the SAME camera, scene and geometry, with only the
            // coat-shadow switch moved. Off first, so the baseline cannot be
            // contaminated by a volume built for the previous case.
            Coat().m_Enabled = false;
            std::vector<u8> unshadowed;
            Capture(std::string("GroomCoatShadowOff_GL_") + pathCase.Name, eye, 0.0f, 0.10f, unshadowed);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            Coat().m_Enabled = true;
            std::vector<u8> shadowed;
            Capture(std::string("GroomCoatShadow_GL_") + pathCase.Name, eye, 0.0f, 0.10f, shadowed);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            const u32 differing = CountDifferingPixels(shadowed, unshadowed);
            const u32 maxDelta = MaxChannelDelta(shadowed, unshadowed);
            const f64 shadowedLuma = CoatLuminance(shadowed, unshadowed);
            std::printf("[groom-coat] path %-11s  %u px differ, max delta %u/255, luma delta %.0f\n",
                        pathCase.Name, differing, maxDelta, shadowedLuma);

            EXPECT_GT(differing, 2000u)
                << pathCase.Name
                << ": turning coat shadowing on changed almost nothing, so the volume is not reaching the coat "
                   "on this path — which is what a bound-but-unsampled texture or an inactive routing lane "
                   "looks like";
            EXPECT_GT(maxDelta, 20u) << pathCase.Name << ": the difference is too faint to be occlusion";

            // AND IT DARKENS — on THIS coat, which is dense enough (6000
            // strands) for the measured occlusion to outweigh the geometric
            // ramp it replaces. A term that changed the frame by brightening a
            // dense coat would pass every assertion above while being the
            // opposite of a shadow: a sign error in the march, or a
            // transmittance used as an opacity.
            //
            // The direction is density-dependent by design, and
            // ADenserCoatIsDarkenedMoreThanASparseOne pins both ends of that —
            // a coat too sparse to self-shadow legitimately comes out brighter
            // once the ramp is bypassed.
            EXPECT_LT(shadowedLuma, 0.0)
                << pathCase.Name << ": coat shadowing made a DENSE coat brighter, which is a sign error, not a "
                                    "shadow";

            const GroomCoatShadowStats& stats = CoatStats();
            EXPECT_EQ(stats.ShadowedGrooms, 1u)
                << pathCase.Name << ": the pass did not report a shadowed groom (dominant reason: "
                << ToString(stats.DominantFallbackReason()) << ")";
            EXPECT_EQ(stats.FallbackGrooms, 0u) << pathCase.Name;
            EXPECT_EQ(stats.ResolutionInForce, 64u) << pathCase.Name;
            EXPECT_GT(stats.ResidentBytes, 0u) << pathCase.Name;
        }
    }

    // ── 2. The term follows the coat's density ─────────────────────────────

    TEST_F(GroomCoatShadowVisualEvidenceTest, ADenserCoatIsDarkenedMoreThanASparseOne)
    {
        // A constant tint would satisfy "the frame changed". This is what says
        // the number being marched is the coat's own density: the SAME groom at
        // two strand budgets, so the geometry and the camera are identical and
        // only how much hair is in the box moves.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        const auto measure = [&](u32 strands, const char* tag) -> f64 {
            Groom().m_MaxRenderStrands = strands;

            Coat().m_Enabled = false;
            std::vector<u8> unshadowed;
            Capture({}, eye, 0.0f, 0.10f, unshadowed);

            Coat().m_Enabled = true;
            std::vector<u8> shadowed;
            Capture(std::string("GroomCoatShadow_GL_Forward_") + tag, eye, 0.0f, 0.10f, shadowed);

            const f64 delta = CoatLuminance(shadowed, unshadowed);
            std::printf("[groom-coat] %-6s strands %5u  luma delta %.0f\n", tag, strands, delta);
            return delta;
        };

        const f64 sparse = measure(1500u, "Sparse");
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        const f64 dense = measure(6000u, "Dense");
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // Asserted as an ORDERING rather than a ratio, because the two frames
        // sit on different parts of the tone curve and a ratio of tone-mapped
        // sums is exactly the measurement groom-fibre-scattering.md rule 13
        // records getting backwards.
        EXPECT_LT(dense, sparse) << "dense " << dense << " sparse " << sparse
                                 << ": the coat shadow did not respond to the coat's density, so what is being "
                                    "marched is not the density field";

        // AND THE SPARSE COAT GETS BRIGHTER, which is not a bug and is worth
        // pinning so nobody "fixes" it.
        //
        // Turning coat shadowing on does two things at once: it adds the
        // measured occlusion, and it BYPASSES the geometric root-to-tip ramp
        // (GroomStrand.glsl), because that ramp was the crude stand-in for
        // exactly the depth-in-coat darkening the volume now measures — keeping
        // both would darken the roots twice. On a coat too sparse to actually
        // self-shadow, the fake darkening it replaced was the stronger of the
        // two, so the net result is a brighter coat. That is the correct
        // physical answer: a sparse coat should NOT have dark roots.
        //
        // Measured at 1500 strands: +338 020, against −510 380 at 6000.
        EXPECT_GT(sparse, 0.0)
            << "sparse " << sparse
            << ": a coat too sparse to self-shadow should come out BRIGHTER once the geometric ramp is bypassed. "
               "A negative value here means either the ramp is still being applied on top of the measured term "
               "(a double count) or the term is darkening a coat that has almost no density to darken with";
        EXPECT_LT(dense, 0.0) << "dense " << dense
                              << ": a coat dense enough to self-shadow must come out net DARKER, or the measured "
                                 "occlusion is not outweighing the ramp it replaced";
    }

    // ── 3. The pigment survives the shadow ─────────────────────────────────

    TEST_F(GroomCoatShadowVisualEvidenceTest, PaleAndDarkCoatsStayDistinctWithCoatShadowingOn)
    {
        // Criterion 2 asks that undercoat, guard hairs and transmission remain
        // distinct INCLUDING on pale coats. The failure this guards is a coat
        // shadow strong enough to flatten the pigment: it would pass every
        // assertion in case 1 while destroying the thing #1247 shipped.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };
        // BACKLIT: the regime where transmission carries the coat, and where a
        // pale and a dark coat are furthest apart.
        SetLightDirection(glm::vec3(0.15f, -0.25f, 0.85f));

        auto& fibre = m_GroomEntity.GetComponent<GroomFibreComponent>();
        Coat().m_Enabled = true;

        fibre.m_Eumelanin = 0.1f;
        fibre.m_Pheomelanin = 0.05f;
        std::vector<u8> pale;
        Capture("GroomCoatShadow_GL_Forward_PaleBacklit", eye, 0.0f, 0.10f, pale);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        fibre.m_Eumelanin = 4.0f;
        fibre.m_Pheomelanin = 0.0f;
        std::vector<u8> dark;
        Capture("GroomCoatShadow_GL_Forward_DarkBacklit", eye, 0.0f, 0.10f, dark);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const u32 differing = CountDifferingPixels(pale, dark);
        const u32 maxDelta = MaxChannelDelta(pale, dark);
        std::printf("[groom-coat] pale vs dark, coat shadow ON: %u px differ, max delta %u/255\n", differing,
                    maxDelta);

        EXPECT_GT(differing, 4000u)
            << "the coat shadow has flattened the pigment: a pale and a dark coat render nearly the same, which "
               "is criterion 2 broken by the term criterion 1 asked for";
        EXPECT_GT(maxDelta, 30u);
    }

    // ── 4. Criterion 4, as a counter ───────────────────────────────────────

    TEST_F(GroomCoatShadowVisualEvidenceTest, AnAnimatedLightCausesNoVolumeRebuilds)
    {
        // THE STRUCTURAL ANSWER TO "no uncontrolled flicker or stale density",
        // measured rather than looked at. The selected representation is
        // light-independent, so a moving light must cost EXACTLY ZERO rebuilds.
        // A deep opacity map — the candidate the bake-off rejected — would
        // rebuild on every one of these frames, which is precisely why it was
        // rejected.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };
        Coat().m_Enabled = true;

        // One capture to get the first bake out of the way. The first frame a
        // coat is visible SHOULD rebuild; that is not staleness, and counting
        // it would make this assertion impossible to satisfy honestly.
        std::vector<u8> warmup;
        Capture({}, eye, 0.0f, 0.10f, warmup);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        ASSERT_EQ(CoatStats().ShadowedGrooms, 1u) << "the coat never got a volume, so this measures nothing";

        u32 totalRebuilds = 0;
        for (int step = 0; step < 8; ++step)
        {
            const f32 angle = static_cast<f32>(step) * 0.7f;
            SetLightDirection(glm::vec3(std::sin(angle), -0.6f, std::cos(angle)));

            std::vector<u8> frame;
            Capture({}, eye, 0.0f, 0.10f, frame);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            totalRebuilds += CoatStats().Rebuilds;
            EXPECT_EQ(CoatStats().ShadowedGrooms, 1u) << "step " << step << ": the coat lost its volume mid-sweep";
        }

        std::printf("[groom-coat] eight light directions: %u volume rebuilds\n", totalRebuilds);
        EXPECT_EQ(totalRebuilds, 0u)
            << "the coat volume rebuilt " << totalRebuilds
            << " times while only the LIGHT moved — the representation is supposed to be light-independent, and "
               "a rebuild per light move is the staleness/flicker failure criterion 4 names";
    }

    TEST_F(GroomCoatShadowVisualEvidenceTest, AMovingCameraCausesNoVolumeRebuildsAtAFixedLod)
    {
        // The camera's twin of the case above. The volume is baked in GROOM
        // OBJECT space, so moving the eye cannot invalidate it either — as long
        // as the coat stays on one side of a shadow-LOD boundary, which is what
        // pinning the LOD to a single step here isolates.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        Coat().m_Enabled = true;
        // No halvings, so this case measures camera motion alone and not the
        // LOD policy — which has its own coverage in
        // GroomCoatShadowLod.AnOscillatingRequestCannotRebuildEveryFrame.
        Coat().m_MaxLodSteps = 0;

        std::vector<u8> warmup;
        Capture({}, glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f, warmup);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        ASSERT_EQ(CoatStats().ShadowedGrooms, 1u);

        u32 totalRebuilds = 0;
        for (int step = 0; step < 6; ++step)
        {
            const f32 angle = static_cast<f32>(step) * 0.5f;
            const glm::vec3 eye{ std::sin(angle) * 4.6f, 0.9f, std::cos(angle) * 4.6f };
            std::vector<u8> frame;
            Capture({}, eye, angle, 0.10f, frame);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            totalRebuilds += CoatStats().Rebuilds;
        }

        std::printf("[groom-coat] six camera poses: %u volume rebuilds\n", totalRebuilds);
        EXPECT_EQ(totalRebuilds, 0u) << "the coat volume rebuilt " << totalRebuilds
                                     << " times while only the CAMERA moved";
    }

    // ── 5. MSAA and a non-native resolution ────────────────────────────────

    TEST_F(GroomCoatShadowVisualEvidenceTest, TheCoatShadowSurvivesMsaa)
    {
        // MSAA IS A CELL, decided in the HANDOVER rather than here: hair is
        // sub-pixel geometry and coverage interacts with both the shadow and
        // the transmission term, which makes this the likeliest place for the
        // flicker failure to appear.
        //
        // MSAASampleCount lives on DeferredSettings, not RendererSettings —
        // MSAA is a G-Buffer setting on the deferred path only, and the strand
        // pass runs after the resolve. That is verified here rather than
        // assumed.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        auto& deferred = Renderer3D::GetRendererSettings().Deferred;
        const u32 restoreSamples = deferred.MSAASampleCount;

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };
        Coat().m_Enabled = true;

        deferred.MSAASampleCount = 1;
        Renderer3D::ApplyRendererSettings();
        std::vector<u8> noMsaa;
        Capture("GroomCoatShadow_GL_Deferred_NoMsaa", eye, 0.0f, 0.10f, noMsaa);
        if (::testing::Test::HasFatalFailure())
        {
            deferred.MSAASampleCount = restoreSamples;
            Renderer3D::ApplyRendererSettings();
            return;
        }

        deferred.MSAASampleCount = 4;
        Renderer3D::ApplyRendererSettings();
        std::vector<u8> msaa;
        Capture("GroomCoatShadow_GL_Deferred_Msaa4", eye, 0.0f, 0.10f, msaa);

        deferred.MSAASampleCount = restoreSamples;
        Renderer3D::ApplyRendererSettings();
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // The coat must still BE there. Not "identical" — MSAA legitimately
        // changes edge pixels — but the shadow must not vanish, and it must not
        // invert.
        const GroomCoatShadowStats& stats = CoatStats();
        EXPECT_EQ(stats.ShadowedGrooms, 1u) << "the coat lost its shadow under MSAA";
        EXPECT_EQ(stats.FallbackGrooms, 0u);

        const u32 differing = CountDifferingPixels(msaa, noMsaa);
        std::printf("[groom-coat] MSAA 1 vs 4: %u px differ\n", differing);
        // A hard equality would be wrong (MSAA changes edges) and a hard
        // inequality would be wrong too (it may legitimately change very
        // little), so what is asserted is that the frame is still a coat: the
        // pass reports one, and the picture is committed for a reviewer.
        EXPECT_GT(MaxChannelDelta(msaa, std::vector<u8>(msaa.size(), 0u)), 20u)
            << "the MSAA capture is black, so nothing was drawn at all";
    }

    TEST_F(GroomCoatShadowVisualEvidenceTest, TheCoatShadowSurvivesANonNativeResolution)
    {
        // The UPSCALE cell, decided in the HANDOVER for the same sub-pixel
        // reason as MSAA. The volume's resolution is a property of the COAT,
        // not of the framebuffer, so the representation in force must not move
        // when the render target does — a resolution-dependent bake would make
        // an upscaled frame a different coat.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        Coat().m_Enabled = true;

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        std::vector<u8> native;
        Capture("GroomCoatShadow_GL_Deferred_Native", eye, 0.0f, 0.10f, native);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        const u32 nativeResolution = CoatStats().ResolutionInForce;
        ASSERT_EQ(CoatStats().ShadowedGrooms, 1u);

        constexpr u32 kSmallWidth = 960;
        constexpr u32 kSmallHeight = 540;
        // ResizeRenderTarget, not a second EnableRendering: the fixture owns
        // the render target's lifetime, and re-enabling would build a new one
        // underneath the frame graph mid-test.
        ResizeRenderTarget(kSmallWidth, kSmallHeight);

        std::vector<u8> scaled;
        Capture("GroomCoatShadow_GL_Deferred_Scaled", eye, 0.0f, 0.10f, scaled, kSmallWidth, kSmallHeight);
        const u32 scaledResolution = CoatStats().ResolutionInForce;
        const u32 scaledShadowed = CoatStats().ShadowedGrooms;
        const u32 scaledFallback = CoatStats().FallbackGrooms;

        // Restored BEFORE any assertion can return early, so a failure here
        // cannot leave the fixture at the small size for whatever runs next.
        ResizeRenderTarget(kWidth, kHeight);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        std::printf("[groom-coat] resolution in force: native %u, scaled %u\n", nativeResolution,
                    scaledResolution);
        EXPECT_EQ(scaledShadowed, 1u) << "the coat lost its shadow at a non-native resolution";
        EXPECT_EQ(scaledFallback, 0u);
        // THE POINT OF THE CELL: the volume's resolution is a property of the
        // COAT, not of the framebuffer. A bake that moved with the render
        // target would make an upscaled frame a different coat.
        EXPECT_EQ(scaledResolution, nativeResolution)
            << "the coat volume's resolution changed with the render target size";
    }
}
