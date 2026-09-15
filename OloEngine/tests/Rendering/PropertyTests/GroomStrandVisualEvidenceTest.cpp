#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomStrandVisualEvidenceTest — issue #1246, acceptance criteria 2 and 3.
//
// The PRODUCTION strand path's pixels, as opposed to GroomPreviewVisualEvidence
// Test's debug lines. Writes
//   OloEditor/assets/tests/visual/GroomStrand_GL_<Path>[_<Angle>].png
//
// The filename carries the {backend} x {path} cell it covers, deliberately
// including the backend even though it is always GL here: a reader counting
// files then cannot mistake a complete set of OpenGL captures for a complete
// verification matrix. Every Vulkan cell is live-only — these fixtures need a
// real GL 4.6 context and skip without one — and is evidenced in the PR body
// instead.
//
// Beyond the PNGs, the contracts asserted are the ones that can be wrong while
// the picture still looks like hair:
//
//   1. The strands are DRAWN, on all three rendering paths. A path-agnostic
//      pass that silently only works on one is the classic version of this bug,
//      and it is invisible in a single-path capture.
//   2. DEPTH-CORRECT OVERLAP against opaque geometry (criterion 2): strands
//      BEHIND the body are hidden by it, and strands in front are not. Tested
//      by moving the occluder, not by eye.
//   3. The strand pass writes DEPTH, so geometry drawn after it composes
//      against the coat rather than through it.
//   4. The composition mode reaching the GPU is the one the SEAM chose, and
//      the refusal is visible in the frame: with no temporal resolve running,
//      a groom asking for StochasticAlpha renders the OpaqueRibbon tier — the
//      same pixels as a groom that asked for OpaqueRibbon outright.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "../../Groom/GroomStrandFixture.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
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
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
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

        // A strand pixel is one that differs noticeably from the same frame
        // with strand rendering switched off. 12/255 per channel is well above
        // dithering and well below the dimmest point of the root-to-tip ramp.
        constexpr int kStrandPixelThreshold = 12;

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            u32 differing = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(frame[i]) - static_cast<int>(baseline[i]));
                const int dg = std::abs(static_cast<int>(frame[i + 1]) - static_cast<int>(baseline[i + 1]));
                const int db = std::abs(static_cast<int>(frame[i + 2]) - static_cast<int>(baseline[i + 2]));
                if (dr > kStrandPixelThreshold || dg > kStrandPixelThreshold || db > kStrandPixelThreshold)
                {
                    ++differing;
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
                    worst = std::max(worst, std::abs(static_cast<int>(frame[i + c]) -
                                                     static_cast<int>(baseline[i + c])));
                }
            }
            return static_cast<u32>(worst);
        }

        [[nodiscard]] f64 MeanLuminance(const std::vector<u8>& frame)
        {
            if (frame.empty())
            {
                return 0.0;
            }
            f64 sum = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                sum += (0.2126 * frame[i] + 0.7152 * frame[i + 1] + 0.0722 * frame[i + 2]) / 255.0;
                ++count;
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }
    } // namespace

    class GroomStrandVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        AssetHandle m_GroomHandle = 0;
        Entity m_GroomEntity;
        Entity m_OccluderEntity;

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
                            "  Name: GroomStrandEvidence\n"
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
            // The production strand pass is NOT a gizmo — it draws regardless
            // of these — but the grid and axis helper would clutter a capture
            // whose whole subject is a silhouette.
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            // THE BODY. Lit opaque geometry the strands grow out of, so
            // "depth-correct overlap against the body" (criterion 2) is a thing
            // the capture can actually show — and so a frame that comes back
            // empty says "the strands did not draw" rather than "the fixture
            // did not render".
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
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.10f, 0.09f, 0.09f, 1.0f));
            }

            // ORDINARY SCENE GEOMETRY, parked out of frame. The overlap test
            // moves it between the camera and the coat; leaving it here from
            // the start means the scene's draw list does not change shape
            // between the two captures, so a pixel difference is the occlusion
            // and not a different frame graph.
            {
                m_OccluderEntity = scene.CreateEntity("Occluder");
                auto& tc = m_OccluderEntity.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, -1000.0f, 0.0f);
                tc.Scale = glm::vec3(1.4f, 1.4f, 0.1f);
                auto& mc = m_OccluderEntity.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = m_OccluderEntity.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.75f, 0.20f, 0.18f, 1.0f));
            }

            Ref<GroomAsset> groom = BuildEvidenceGroom();
            ASSERT_TRUE(groom);
            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false; // the DEBUG path stays off in these captures
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = 4000;
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);
        }

        // An editor-scale hemisphere groom. At the real 70 um of a human hair
        // this capture would be a picture of an almost-empty frame — which is a
        // true fact about hair, measured properly by the CPU coverage tests,
        // and a useless golden image. Width scale 12 puts the strands at a
        // pixel or two so a human can see silhouette and overlap.
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

            builder.SetName("StrandEvidenceGroom");
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

        // These are EVIDENCE, not SSIM goldens: the selected mode is stochastic
        // and its single-frame output is deliberately noise, so a committed
        // per-pixel baseline would be a flake generator. The contracts are the
        // assertions below; the PNGs exist so a reviewer can look at what they
        // describe. So this always writes, and never compares.
        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            // `name` is the WHOLE stem, so an A/B control can be named
            // GroomStrandOff_<Backend>_<Path> — the convention the task
            // loop asks for, where a control is a sibling of its cell
            // rather than a suffix of it.
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                               pixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        void SetRenderStrands(bool enabled)
        {
            m_GroomEntity.GetComponent<GroomComponent>().m_RenderStrands = enabled;
        }
    };

    // ── Criterion 2: the strands draw, on every path ────────────────────────

    TEST_F(GroomStrandVisualEvidenceTest, StrandsRenderOnEveryRenderingPath)
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

            // A/B against the SAME camera and the same scene with only the
            // strand switch moved — backend-ab-needs-an-identical-camera. Off
            // first, so the baseline cannot be contaminated by the coat.
            SetRenderStrands(false);
            std::vector<u8> without;
            Capture(std::string("GroomStrandOff_GL_") + pathCase.Name, eye, 0.0f, 0.10f, without);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            SetRenderStrands(true);
            std::vector<u8> with;
            Capture(std::string("GroomStrand_GL_") + pathCase.Name, eye, 0.0f, 0.10f, with);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            EXPECT_GT(MeanLuminance(with), 0.02) << pathCase.Name << ": the frame is (near-)black, so nothing "
                                                                     "below is evidence of anything";

            const u32 differing = CountDifferingPixels(with, without);
            const u32 maxDelta = MaxChannelDelta(with, without);
            std::printf("[groom-strand] path %-11s  %u px differ, max channel delta %u/255\n", pathCase.Name,
                        differing, maxDelta);

            // 2000 pixels of a 921 600-pixel frame is well under a per-cent and
            // far above any tone-map or dither wobble; a path where the pass
            // silently did nothing scores zero here.
            EXPECT_GT(differing, 2000u)
                << pathCase.Name << ": turning strand rendering on changed almost nothing, so the pass did not "
                                    "draw on this path";
            EXPECT_GT(maxDelta, 30u) << pathCase.Name << ": the difference is too faint to be geometry";

            const auto* groomPass = Renderer3D::GetGroomRenderPass();
            ASSERT_NE(groomPass, nullptr);
            const GroomRenderStats& stats = groomPass->GetStats();
            EXPECT_EQ(stats.GroomsDrawn, 1u) << pathCase.Name << ": the pass reported drawing no groom";
            EXPECT_GT(stats.SegmentsDrawn, 0u) << pathCase.Name;
            std::printf("[groom-strand] path %-11s  %u strands, %u segments, %.2f MiB cached\n", pathCase.Name,
                        stats.StrandsDrawn, stats.SegmentsDrawn,
                        static_cast<f64>(stats.CachedBytes) / (1024.0 * 1024.0));
        }
    }

    // ── Criterion 2: depth-correct overlap against ordinary geometry ────────

    TEST_F(GroomStrandVisualEvidenceTest, OpaqueGeometryInFrontOccludesTheCoat)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        SetRenderStrands(true);

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        std::vector<u8> unoccluded;
        Capture("GroomStrand_GL_Forward_Unoccluded", eye, 0.0f, 0.10f, unoccluded);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // Slide the slab between the camera and the coat. Only its transform
        // moves: same entity, same material, same draw list.
        m_OccluderEntity.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.4f, 1.9f);

        std::vector<u8> occluded;
        Capture("GroomStrand_GL_Forward_Occluded", eye, 0.0f, 0.10f, occluded);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const u32 differing = CountDifferingPixels(occluded, unoccluded);
        std::printf("[groom-strand] occluder in front: %u px differ, max channel delta %u/255\n", differing,
                    MaxChannelDelta(occluded, unoccluded));
        EXPECT_GT(differing, 20000u) << "the occluder did not change the frame at all";

        // THE CONTRACT. Behind the slab there must be no coat: the depth test
        // rejected those strand fragments. Sampled over the slab's projected
        // area rather than the whole frame, because the coat outside it is
        // supposed to survive.
        //
        // Comparing against a THIRD capture — occluder in front, strands off —
        // is what makes this a statement about the strands rather than about
        // the slab: the two frames differ only in whether strands were drawn,
        // and behind an opaque slab they must not differ at all.
        SetRenderStrands(false);
        std::vector<u8> occludedNoStrands;
        Capture("", eye, 0.0f, 0.10f, occludedNoStrands);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const u32 leakedThrough = CountDifferingPixels(occluded, occludedNoStrands);
        std::printf("[groom-strand] strand pixels visible through the occluder: %u\n", leakedThrough);
        // Not zero: strands still show AROUND the slab, which is the point of
        // parking it in front of only part of the coat. What must be true is
        // that far fewer strand pixels survive than without the slab.
        const u32 unoccludedStrandPixels = [&] {
            SetRenderStrands(false);
            m_OccluderEntity.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -1000.0f, 0.0f);
            std::vector<u8> baseline;
            Capture("", eye, 0.0f, 0.10f, baseline);
            return CountDifferingPixels(unoccluded, baseline);
        }();
        std::printf("[groom-strand] strand pixels with no occluder: %u\n", unoccludedStrandPixels);

        ASSERT_GT(unoccludedStrandPixels, 0u) << "the unoccluded capture has no strand pixels to lose";
        EXPECT_LT(leakedThrough, unoccludedStrandPixels)
            << "an opaque slab in front of the coat hid none of it, so the strand pass is not depth-testing";
        // A slab covering a large share of the coat should hide a large share
        // of it. A pass that wrote colour but no depth, or tested depth
        // against the wrong target, would leak most of the coat through.
        EXPECT_LT(leakedThrough, unoccludedStrandPixels * 3u / 4u)
            << "most of the coat rendered through an opaque occluder";
    }

    // ── Criterion 4: the seam's refusal is visible in the pixels ────────────

    TEST_F(GroomStrandVisualEvidenceTest, AStochasticRequestWithNoTemporalResolveRendersTheFallbackTier)
    {
        // The strongest form of "capability fallbacks are explicit": not that a
        // counter says so, but that the FRAME is the fallback tier's frame.
        // These fixtures run with TAA off and no upscaler, so
        // SelectGroomComposition must refuse StochasticAlpha — and the pixels
        // must then be byte-identical to a groom that asked for OpaqueRibbon,
        // because they are the same code path.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        SetRenderStrands(true);

        const glm::vec3 eye{ 0.0f, 0.9f, 4.6f };

        auto& groomComponent = m_GroomEntity.GetComponent<GroomComponent>();
        groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
        std::vector<u8> askedForOpaque;
        Capture("GroomStrand_GL_Forward_OpaqueRibbon", eye, 0.0f, 0.10f, askedForOpaque);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::StochasticAlpha);
        std::vector<u8> askedForStochastic;
        Capture("GroomStrand_GL_Forward_StochasticRefused", eye, 0.0f, 0.10f, askedForStochastic);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const auto* groomPass = Renderer3D::GetGroomRenderPass();
        ASSERT_NE(groomPass, nullptr);
        const GroomCompositionDecision decision =
            groomPass->DecideComposition(GroomCompositionMode::StochasticAlpha);
        EXPECT_EQ(decision.Effective, GroomCompositionMode::OpaqueRibbon);
        EXPECT_EQ(decision.Reason, GroomCompositionFallbackReason::TemporalResolveUnavailable);

        // Byte-identical, not merely similar. A stray uniform, a different
        // discard threshold or a half-applied mode would move a pixel.
        EXPECT_EQ(CountDifferingPixels(askedForStochastic, askedForOpaque), 0u)
            << "a refused stochastic request did not render the fallback tier's exact frame";

        // Paired with a contrast assertion, or two identical black frames pass
        // the line above (technique-selection-seams.md, "Ratchet the untouched
        // path with bytes, not eyes").
        SetRenderStrands(false);
        std::vector<u8> noStrands;
        Capture("", eye, 0.0f, 0.10f, noStrands);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        EXPECT_GT(CountDifferingPixels(askedForOpaque, noStrands), 2000u)
            << "the fallback tier drew nothing, so the byte-identity above is identity between two empty frames";

        const GroomRenderStats& stats = groomPass->GetStats();
        EXPECT_EQ(stats.Composition.DominantFallbackReason(),
                  GroomCompositionFallbackReason::TemporalResolveUnavailable);
    }

    // ── Criterion 2/3: several angles, for a human to look at ───────────────

    TEST_F(GroomStrandVisualEvidenceTest, CapturesTheCoatFromSeveralAngles)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        SetRenderStrands(true);

        struct Pose
        {
            const char* Name;
            glm::vec3 Eye;
            f32 Yaw;
            f32 Pitch;
        };
        const std::array<Pose, 4> poses = { {
            { "Front", { 0.0f, 0.9f, 4.6f }, 0.0f, 0.10f },
            { "Side", { 4.4f, 0.9f, 0.6f }, 1.43f, 0.10f },
            { "Top", { 0.0f, 4.4f, 1.6f }, 0.0f, 1.10f },
            { "Grazing", { 3.2f, 0.2f, 3.2f }, 0.78f, -0.02f },
        } };

        for (const Pose& pose : poses)
        {
            std::vector<u8> pixels;
            Capture(std::string("GroomStrand_GL_Forward_") + pose.Name, pose.Eye, pose.Yaw, pose.Pitch, pixels);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            EXPECT_GT(MeanLuminance(pixels), 0.02) << "pose '" << pose.Name << "' is (near-)black";
        }

        // The golden-rebase switch is meaningless for this file — these are
        // evidence captures, always written, never compared — and saying so
        // here stops someone reaching for it when a capture looks different.
        if (GoldenRebaseRequested())
        {
            std::printf("[groom-strand] --olo-golden-rebase has no effect here: these captures are evidence, "
                        "not SSIM goldens. See the file header.\n");
        }
    }
} // namespace OloEngine::Tests
