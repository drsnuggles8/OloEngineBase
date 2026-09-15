// =============================================================================
// GroomPreviewVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the groom debug preview (issue #1232, AC 3:
// "preview shows roots, groups and curve direction"). Renders a two-group
// groom with guides through the FULL editor render path from several camera
// poses and writes each frame to
//   OloEditor/assets/tests/visual/GroomPreview_<pose>.png
//
// WHY A VISUAL TEST FOR DEBUG LINES. The preview IS the acceptance surface of
// this issue — an imported groom has no shading yet, so the debug lines are the
// only way import correctness becomes visible. A CPU test can prove the
// subsampling rule and the colour assignment (GroomPreviewTest does); only a
// rendered frame can prove that the lines reach the screen at all, which is the
// failure a renderer change would actually cause.
//
// Beyond the PNGs, four driver-independent contracts are asserted, each aimed
// at a way the preview could be broken while still producing a plausible
// picture:
//
//   1. The strands are DRAWN — a meaningful number of pixels differ from the
//      same frame rendered with the preview switched off.
//   2. ROOTS are drawn independently of strands: a roots-only frame is not
//      empty, and differs from a strands-only frame.
//   3. The DIRECTION ramp darkens the root end — the ramp-on frame is dimmer
//      over the strand pixels than the ramp-off frame. If the ramp ever ran the
//      other way, a tip-first import would look correct.
//   4. COLOUR BY GROUP produces more than one hue; with it off, one.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomPreview.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Same slack as the other visual-evidence goldens: the render is
        // deterministic, so a same-machine re-run is ~0 and this only absorbs
        // cross-GPU float variance in the line rasterisation.
        constexpr f64 kGoldenRmseThreshold = 6.0;

        // A strand pixel is one that differs noticeably from the preview-off
        // frame. 12/255 per channel is well above dithering and well below the
        // dimmest point of the direction ramp (0.15 of a ~0.8 base colour).
        constexpr int kStrandPixelThreshold = 12;

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
            {
                return std::numeric_limits<f64>::max();
            }
            f64 sumSq = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        // Indices (in pixels, not bytes) where `frame` differs from `baseline`.
        [[nodiscard]] std::vector<sizet> DifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            std::vector<sizet> differing;
            if (frame.size() != baseline.size())
            {
                return differing;
            }
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(frame[i]) - static_cast<int>(baseline[i]));
                const int dg = std::abs(static_cast<int>(frame[i + 1]) - static_cast<int>(baseline[i + 1]));
                const int db = std::abs(static_cast<int>(frame[i + 2]) - static_cast<int>(baseline[i + 2]));
                if (dr > kStrandPixelThreshold || dg > kStrandPixelThreshold || db > kStrandPixelThreshold)
                {
                    differing.push_back(i);
                }
            }
            return differing;
        }

        [[nodiscard]] f64 MeanChannelOver(const std::vector<u8>& frame, const std::vector<sizet>& pixels)
        {
            if (pixels.empty())
            {
                return 0.0;
            }
            f64 sum = 0.0;
            for (const sizet i : pixels)
            {
                sum += static_cast<f64>(frame[i]) + static_cast<f64>(frame[i + 1]) + static_cast<f64>(frame[i + 2]);
            }
            return sum / (static_cast<f64>(pixels.size()) * 3.0);
        }

        // Coarse hue bucket of a pixel, or -1 when it is too dark or too
        // desaturated to have a meaningful hue. Used for the colour-by-group
        // contract — comparing exact RGB would be a driver-precision test.
        [[nodiscard]] int HueBucket(u8 r, u8 g, u8 b)
        {
            const int maxC = std::max({ static_cast<int>(r), static_cast<int>(g), static_cast<int>(b) });
            const int minC = std::min({ static_cast<int>(r), static_cast<int>(g), static_cast<int>(b) });
            if (maxC < 40 || (maxC - minC) < 25)
            {
                return -1; // near-black or near-grey: no usable hue
            }
            const f32 fr = static_cast<f32>(r) / 255.0f;
            const f32 fg = static_cast<f32>(g) / 255.0f;
            const f32 fb = static_cast<f32>(b) / 255.0f;
            const f32 fmax = static_cast<f32>(maxC) / 255.0f;
            const f32 delta = static_cast<f32>(maxC - minC) / 255.0f;

            f32 hue = 0.0f;
            if (fmax == fr)
            {
                hue = std::fmod(((fg - fb) / delta) + 6.0f, 6.0f);
            }
            else if (fmax == fg)
            {
                hue = ((fb - fr) / delta) + 2.0f;
            }
            else
            {
                hue = ((fr - fg) / delta) + 4.0f;
            }
            // 12 buckets of 30 degrees each.
            return static_cast<int>(hue * 2.0f) % 12;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }
    } // namespace

    class GroomPreviewVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        AssetHandle m_GroomHandle = 0;
        Entity m_GroomEntity;

        void BuildScene() override
        {
            // AddMemoryOnlyAsset needs an active project + asset manager —
            // mount a throwaway temp project, mirroring
            // VolumeVDBVisualEvidenceTest.
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                const fs::path projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: GroomPreviewEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            // The preview draws under the editor-debug gizmo switches. They are
            // off in some capture contexts (see the benchmark-capture fix that
            // stopped the grid bleeding into frames), so turn them on
            // explicitly rather than depending on a default.
            auto& rendererSettings = Renderer3D::GetRendererSettings();
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = true;
            rendererSettings.ShowGrid = false; // keep the frame about the groom
            rendererSettings.ShowWorldAxisHelper = false;
            // Deliberately NO Renderer3D::ApplyRendererSettings() here. These
            // four flags are read straight off the settings struct by
            // Scene::RenderScene3D every frame; ApplyRendererSettings exists to
            // RECONFIGURE the render graph after a path / AO / culling change,
            // and calling it right after EnableRendering re-runs that configure
            // against targets EnableRendering had only just sized.

            // A lit scalp sphere the strands grow out of. Two reasons, and the
            // second is the important one: it gives the captures a subject so
            // "are the roots ON the surface?" is answerable by eye, and it puts
            // ordinary lit geometry in the frame so a capture that comes back
            // empty says "the preview did not draw" rather than "the fixture
            // did not render".
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }
            {
                Entity scalp = scene.CreateEntity("Scalp");
                auto& tc = scalp.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f);
                // Slightly inside the 1.0 root radius so the roots sit just
                // proud of the surface rather than z-fighting it.
                tc.Scale = glm::vec3(0.98f);
                auto& mc = scalp.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = scalp.AddComponent<MaterialComponent>();
                // Dark, unsaturated: the strand hues have to read against it,
                // and the colour-by-group hue count must not pick it up.
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.10f, 0.09f, 0.09f, 1.0f));
            }

            Ref<GroomAsset> groom = BuildEvidenceGroom();
            ASSERT_TRUE(groom);
            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u) << "AddMemoryOnlyAsset returned a null handle";

            m_GroomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_MaxPreviewStrands = 600;
            groomComponent.m_RootMarkerSize = 0.0f; // bounds-relative
        }

        // A two-group hemisphere groom at editor scale (metres, not the 9 cm of
        // a real scalp) so the strands are several pixels long in the capture.
        // Real-scale grooms are covered by the CPU tests; a 9 cm subject would
        // make these PNGs a picture of a dot.
        static Ref<GroomAsset> BuildEvidenceGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 groupFront = 0;
            u16 groupBack = 0;
            EXPECT_TRUE(builder.AddGroup("front", groupFront, reason)) << reason;
            EXPECT_TRUE(builder.AddGroup("back", groupBack, reason)) << reason;

            constexpr u32 kStrands = 600;
            constexpr u32 kPoints = 8;
            constexpr f32 kRadius = 1.0f;
            constexpr f32 kLength = 1.2f;
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
                    point.y -= kLength * 0.7f * along * along;
                    points.push_back(point);
                    widths.push_back(0.01f * (1.0f - (0.8f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                // Front half vs back half, INTERLEAVED in source order so the
                // cook's grouping sort does real work.
                input.GroupId = (std::sin(phi) >= 0.0f) ? groupFront : groupBack;
                input.IsGuide = (s % 25u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("EvidenceGroom");
            GroomProvenance provenance;
            provenance.SourcePath = "grooms/evidence.abc";
            provenance.SourceFormat = "AlembicCurves";
            provenance.SourceContentHash = 0x1232123212321232ull;
            provenance.ImporterVersion = 1;
            builder.SetProvenance(provenance);

            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        // Renders one frame from `position` and reads back the composited image.
        // `saveAs` empty means "do not treat this frame as a golden" — the
        // ablation frames (preview off, ramp off, ...) exist to be compared
        // against in-process, not to be committed.
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

            // glGetTextureImage returns rows bottom-up; flip so the PNG is
            // right-side up.
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

            if (saveAs.empty())
            {
                return;
            }
            CompareOrRebaseGolden(saveAs, outPixels);
        }

        void CompareOrRebaseGolden(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("GroomPreview_" + poseName + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                                   4, pixels.data(), static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int gw = 0;
            int gh = 0;
            int gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "Missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
            {
                goldenPixels.assign(golden, golden + (static_cast<sizet>(kWidth) * kHeight * 4u));
            }
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected " << kWidth
                                     << "x" << kHeight << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > " << kGoldenRmseThreshold
                << "). If this is an intended visual change, rerun with --olo-golden-rebase to update " << path;
        }

        [[nodiscard]] GroomComponent& Component()
        {
            return m_GroomEntity.GetComponent<GroomComponent>();
        }
    };

    TEST_F(GroomPreviewVisualEvidenceTest, CaptureGroomPreviewFromMultipleAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // Four angles, chosen for what each one can show to be WRONG:
        //   Front      — the overall silhouette; strands growing inward instead
        //                of outward is obvious here and nowhere else.
        //   Side       — the droop profile, and whether roots sit ON the
        //                hemisphere rather than floating off it.
        //   Top        — group split across the hemisphere (the two groups are
        //                front/back halves, so this is where the colour split
        //                reads).
        //   RootCloseUp— close enough that individual root crosses and the
        //                root-end darkening are resolvable.
        const std::array<Pose, 4> poses = { {
            { "Front", { 0.0f, 0.9f, 4.2f }, 0.0f, 0.10f },
            // NEGATIVE yaw: forward is (sin(yaw), ., -cos(yaw)), so a camera on
            // +X needs yaw = -pi/2 to look back at the origin. +pi/2 points it
            // away, and the capture came back empty — which is exactly the
            // "looks fine, shows nothing" failure these captures exist to catch.
            { "Side", { 4.2f, 0.9f, 0.0f }, -1.5708f, 0.10f },
            { "Top", { 0.0f, 4.4f, 0.9f }, 0.0f, 1.30f },
            { "RootCloseUp", { 0.6f, 1.5f, 1.9f }, 0.28f, 0.42f },
        } };

        for (const auto& pose : poses)
        {
            std::vector<u8> pixels;
            Capture(pose.Name, pose.Position, pose.Yaw, pose.Pitch, pixels);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            u64 lumaSum = 0;
            for (sizet i = 0; i < pixels.size(); i += 4)
            {
                lumaSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            }
            const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
            EXPECT_GT(meanChannel, 1.0) << "Pose '" << pose.Name << "' rendered (near-)black";
        }
    }

    TEST_F(GroomPreviewVisualEvidenceTest, PreviewActuallyReachesTheScreen)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 position(0.0f, 0.9f, 4.2f);
        constexpr f32 kYaw = 0.0f;
        constexpr f32 kPitch = 0.10f;

        // Baseline: the same camera, the same scene, preview OFF. Everything
        // that differs between these two frames is the preview.
        Component().m_ShowPreview = false;
        std::vector<u8> off;
        Capture("", position, kYaw, kPitch, off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Component().m_ShowPreview = true;
        std::vector<u8> on;
        Capture("", position, kYaw, kPitch, on);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        const std::vector<sizet> strandPixels = DifferingPixels(on, off);
        // 600 strands of 7 segments each across a 1280x720 frame: thousands of
        // pixels even at one pixel of line width. A few hundred would mean the
        // lines are being clipped or dropped.
        EXPECT_GT(strandPixels.size(), 2000u)
            << "the groom preview changed only " << strandPixels.size()
            << " pixels — the debug lines are not reaching the composited frame";
    }

    TEST_F(GroomPreviewVisualEvidenceTest, RootsAndStrandsAreIndependentlyVisible)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 position(0.6f, 1.5f, 1.9f);
        constexpr f32 kYaw = 0.28f;
        constexpr f32 kPitch = 0.42f;

        Component().m_ShowPreview = false;
        std::vector<u8> off;
        Capture("", position, kYaw, kPitch, off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Component().m_ShowPreview = true;
        Component().m_ShowStrands = true;
        Component().m_ShowRoots = false;
        std::vector<u8> strandsOnly;
        Capture("", position, kYaw, kPitch, strandsOnly);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Component().m_ShowStrands = false;
        Component().m_ShowRoots = true;
        std::vector<u8> rootsOnly;
        Capture("", position, kYaw, kPitch, rootsOnly);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Restore for any later case in the fixture.
        Component().m_ShowStrands = true;

        const sizet strandCount = DifferingPixels(strandsOnly, off).size();
        const sizet rootCount = DifferingPixels(rootsOnly, off).size();

        EXPECT_GT(strandCount, 1000u) << "strands-only drew almost nothing";
        // Roots are three short crosses per strand: fewer pixels than the
        // strands, but far from zero. A root marker that collapsed to a
        // zero-length line would land here.
        EXPECT_GT(rootCount, 300u) << "roots-only drew almost nothing — the root markers are not visible";
        EXPECT_LT(rootCount, strandCount) << "the root markers are dominating the strands";
    }

    TEST_F(GroomPreviewVisualEvidenceTest, DirectionRampDarkensTheRootEnd)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Measured from ABOVE, where the fixture's hemisphere separates root
        // from tip radially: the roots are a disc in the middle of the frame
        // and the tips are the outer ring (see GroomPreview_Top.png).
        //
        // The measurement is a root-vs-tip brightness GAP, compared between the
        // ramp ON and the ramp OFF frames. Two earlier formulations failed for
        // reasons worth keeping:
        //   * a frame-wide ramped-vs-flat mean came back the WRONG WAY ROUND,
        //     because the debug line is emissive (colour x5) and a hemisphere
        //     of overlapping strands clips to white — what that comparison
        //     actually measured was which strand won the depth test;
        //   * a single-strand close-up put too few pixels on screen (41) to
        //     mean anything.
        // Comparing the GAP against the ramp-off control removes both the
        // clipping and the radial strand-density falloff, which is present in
        // both frames equally.
        const glm::vec3 position(0.0f, 4.4f, 0.9f);
        constexpr f32 kYaw = 0.0f;
        constexpr f32 kPitch = 1.30f;

        // The root crosses are NOT ramped and are drawn near-white, so leaving
        // them on would brighten the very region under test. Group colour off
        // so brightness is the only thing varying.
        Component().m_ShowRoots = false;
        Component().m_ColorByGroup = false;

        Component().m_ShowPreview = false;
        std::vector<u8> off;
        Capture("", position, kYaw, kPitch, off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Component().m_ShowPreview = true;
        Component().m_ShowDirection = false;
        std::vector<u8> flat;
        Capture("", position, kYaw, kPitch, flat);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Component().m_ShowDirection = true;
        std::vector<u8> ramped;
        Capture("", position, kYaw, kPitch, ramped);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Restore for any later case in the fixture.
        Component().m_ShowRoots = true;
        Component().m_ColorByGroup = true;

        // One shared pixel set and one shared geometry, taken from the flat
        // frame, so both measurements sample exactly the same strands.
        const std::vector<sizet> strandPixels = DifferingPixels(flat, off);
        ASSERT_GT(strandPixels.size(), 2000u) << "the top-down frame drew almost no strands";

        f64 sumX = 0.0;
        f64 sumY = 0.0;
        for (const sizet i : strandPixels)
        {
            const sizet pixel = i / 4u;
            sumX += static_cast<f64>(pixel % kWidth);
            sumY += static_cast<f64>(pixel / kWidth);
        }
        const f64 centreX = sumX / static_cast<f64>(strandPixels.size());
        const f64 centreY = sumY / static_cast<f64>(strandPixels.size());

        f64 maxRadius = 0.0;
        for (const sizet i : strandPixels)
        {
            const sizet pixel = i / 4u;
            const f64 dx = static_cast<f64>(pixel % kWidth) - centreX;
            const f64 dy = static_cast<f64>(pixel / kWidth) - centreY;
            maxRadius = std::max(maxRadius, std::sqrt((dx * dx) + (dy * dy)));
        }
        ASSERT_GT(maxRadius, 50.0) << "the groom is too small on screen to split radially";

        std::vector<sizet> rootPixels; // inner disc
        std::vector<sizet> tipPixels;  // outer ring
        for (const sizet i : strandPixels)
        {
            const sizet pixel = i / 4u;
            const f64 dx = static_cast<f64>(pixel % kWidth) - centreX;
            const f64 dy = static_cast<f64>(pixel / kWidth) - centreY;
            const f64 radius = std::sqrt((dx * dx) + (dy * dy)) / maxRadius;
            if (radius < 0.35)
            {
                rootPixels.push_back(i);
            }
            else if (radius > 0.75)
            {
                tipPixels.push_back(i);
            }
        }
        ASSERT_GT(rootPixels.size(), 200u) << "no strand pixels near the roots";
        ASSERT_GT(tipPixels.size(), 200u) << "no strand pixels near the tips";

        const f64 rampedGap = MeanChannelOver(ramped, tipPixels) - MeanChannelOver(ramped, rootPixels);
        const f64 flatGap = MeanChannelOver(flat, tipPixels) - MeanChannelOver(flat, rootPixels);

        // Sign: the ramp runs 0.15 at the root to 1.0 at the tip, so tips must
        // be brighter. Magnitude: clearly larger than the same measurement with
        // the ramp OFF, which is what rules out the radial falloff that exists
        // in both frames. 8/255 is roughly a third of the gap this actually
        // produces, so it fails long before the ramp merely weakens — and it
        // fails hard if the ramp is ever inverted, which is the bug that would
        // make a tip-first import look correct.
        EXPECT_GT(rampedGap, flatGap + 8.0)
            << "ramped tip-minus-root gap " << rampedGap << " vs the ramp-off control " << flatGap
            << " — the root-to-tip brightness ramp is not darkening the root end";
    }

    TEST_F(GroomPreviewVisualEvidenceTest, ColorByGroupProducesMoreThanOneHue)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // From above, the front and back halves are both in frame, so a
        // two-group split has to show as two hues.
        const glm::vec3 position(0.0f, 4.4f, 0.9f);
        constexpr f32 kYaw = 0.0f;
        constexpr f32 kPitch = 1.30f;

        Component().m_ShowRoots = false;
        Component().m_ShowDirection = false; // keep the hue unmodulated
        Component().m_ColorByGroup = true;
        std::vector<u8> colored;
        Capture("", position, kYaw, kPitch, colored);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Component().m_ColorByGroup = false;
        std::vector<u8> neutral;
        Capture("", position, kYaw, kPitch, neutral);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Component().m_ShowRoots = true;
        Component().m_ShowDirection = true;
        Component().m_ColorByGroup = true;

        auto countHues = [](const std::vector<u8>& frame)
        {
            std::set<int> buckets;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int bucket = HueBucket(frame[i], frame[i + 1], frame[i + 2]);
                if (bucket >= 0)
                {
                    buckets.insert(bucket);
                }
            }
            return buckets.size();
        };

        const sizet coloredHues = countHues(colored);
        const sizet neutralHues = countHues(neutral);

        EXPECT_GE(coloredHues, 2u) << "colour-by-group showed " << coloredHues
                                   << " hue(s); a two-group groom must show at least two";
        EXPECT_LT(neutralHues, coloredHues)
            << "turning colour-by-group OFF did not reduce the number of hues (" << neutralHues << " vs "
            << coloredHues << ") — the group colouring is not actually driving the strand colour";
    }
} // namespace OloEngine::Tests
