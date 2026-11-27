// OLO_TEST_LAYER: L8
// =============================================================================
// ReflectionProbeParallaxVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + golden-free differential contracts for the
// distance-impostor reflection probes (issue #705): a mirror floor inside a
// corridor whose four walls carry DISTINCT emissive colours, so the
// raymarched parallax correction is verifiable directionally —
//
//        -X wall RED | +X wall GREEN | far (-Z) BLUE | near (+Z) YELLOW
//
// A probe is baked at the corridor centre (the real editor bake path, which
// now also captures the radial-distance field). The scene renders through
// the full pipeline from several poses, toggling only the probe state:
//
//   * Off          — probe inactive: the floor has no reflection source
//                    (no global environment, no lights beyond the emissive
//                    walls) — the clean fallback baseline.
//   * Parallax     — probe active with its distance field: per-pixel
//                    raymarched reflections.
//   * Legacy       — probe active but the distance field stripped from its
//                    EnvironmentMap: the pre-#705 behaviour (dominant-probe
//                    global override only, direction-only lookup).
//
// Contracts:
//   1. BEHIND-THE-CAMERA / OFF-SCREEN (the SSR-failure case the issue names):
//      looking steeply DOWN the corridor toward the yellow end — the yellow
//      wall itself off-screen — the floor band must read yellow-dominant
//      with Parallax on, and must NOT with the probe Off.
//   2. POSITIONAL PARALLAX: floor strips hugging the red wall vs the green
//      wall must reflect THEIR wall (left strip red-dominant, right strip
//      green-dominant). A direction-only environment lookup cannot produce
//      this split — equal view geometry samples equal env directions
//      regardless of surface position — which makes contract 3 the
//      "measurable improvement" number.
//   3. IMPROVEMENT METRIC: the left/right split magnitude under Parallax
//      must exceed the Legacy split by a wide margin (printed to the log).
//   4. Every frame rendered non-trivially; every PNG round-trips.
//
// SSR stays OFF throughout (the default Forward path has no SSR pass), so
// these captures are exactly the "SSR-off screenshots" the acceptance
// criteria compare against.
//
// Evidence: OloEditor/assets/tests/visual/ReflectionProbeParallax_<state>.png
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;
        constexpr f32 kCaptureTime = 4.0f;

        struct ProbePath
        {
            RenderingPath Path;
            u32 DeferredSamples;
            const char* Name;
        };

        struct BandStats
        {
            f64 R = 0.0;
            f64 G = 0.0;
            f64 B = 0.0;
        };

        // Mean RGB over a rectangular band (UV fractions), rows top-down.
        [[nodiscard]] BandStats SampleBand(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    sumR += px[idx + 0];
                    sumG += px[idx + 1];
                    sumB += px[idx + 2];
                    ++count;
                }
            }
            if (count == 0)
                return {};
            return { static_cast<f64>(sumR) / count, static_cast<f64>(sumG) / count,
                     static_cast<f64>(sumB) / count };
        }

        [[nodiscard]] f64 MeanLuma(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            u64 count = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                sum += 0.2126 * px[i + 0] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2];
                ++count;
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }
    } // namespace

    class ReflectionProbeParallaxVisualEvidenceTest : public RendererAttachedTest,
                                                      public ::testing::WithParamInterface<ProbePath>
    {
      protected:
        Entity m_Probe;

        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        // Corridor interior: x in [-3,3], y in [0,4], z in [-12,12].
        static constexpr f32 kHalfWidth = 3.0f;
        static constexpr f32 kHeightY = 4.0f;
        static constexpr f32 kHalfLength = 12.0f;

        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // The editor render path draws wireframe gizmos for every active
            // probe (marker + influence sphere — with the camera INSIDE the
            // 40m influence sphere its wire circles cross the frame as thin
            // line fragments), plus the infinite grid at y=0 (coplanar with
            // the mirror floor's top face) and the world-axis helper at the
            // origin. Evidence frames should show the reflections, not the
            // authoring chrome.
            scene.SetLightGizmosVisible(false);
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);

            // No lights, no global environment: the emissive walls are the
            // only radiance, the baked probe the only reflection source.
            const auto addSlab = [&scene](const char* name, const glm::vec3& pos, const glm::vec3& scale,
                                          const glm::vec3& emissive)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.02f, 0.02f, 0.02f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            };

            constexpr f32 kThick = 0.25f;
            const f32 spanX = 2.0f * kHalfWidth + 1.0f;
            const f32 spanZ = 2.0f * kHalfLength + 1.0f;

            // Colour-coded walls (see file header).
            addSlab("Wall-X (red)", { -kHalfWidth - kThick, kHeightY * 0.5f, 0.0f },
                    { kThick, kHeightY + 1.0f, spanZ }, { 0.9f, 0.04f, 0.04f });
            addSlab("Wall+X (green)", { kHalfWidth + kThick, kHeightY * 0.5f, 0.0f },
                    { kThick, kHeightY + 1.0f, spanZ }, { 0.04f, 0.9f, 0.04f });
            addSlab("Wall-Z (blue)", { 0.0f, kHeightY * 0.5f, -kHalfLength - kThick },
                    { spanX, kHeightY + 1.0f, kThick }, { 0.04f, 0.04f, 0.9f });
            addSlab("Wall+Z (yellow)", { 0.0f, kHeightY * 0.5f, kHalfLength + kThick },
                    { spanX, kHeightY + 1.0f, kThick }, { 0.9f, 0.9f, 0.04f });
            addSlab("Ceiling", { 0.0f, kHeightY + kThick, 0.0f },
                    { spanX, kThick, spanZ }, { 0.15f, 0.15f, 0.15f });

            // The reflective floor — the acceptance criteria's subject.
            {
                Entity floor = scene.CreateEntity("MirrorFloor");
                auto& tc = floor.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -kThick, 0.0f };
                tc.Scale = { spanX, kThick, spanZ };
                auto& mc = floor.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(1.0f);
                mat.m_Material.SetRoughnessFactor(0.08f);
            }

            // Probe at the corridor centre, head height.
            m_Probe = scene.CreateEntity("CorridorProbe");
            m_Probe.GetComponent<TransformComponent>().Translation = { 0.0f, 2.0f, 0.0f };
            {
                auto& probe = m_Probe.AddComponent<ReflectionProbeComponent>();
                probe.m_InfluenceRadius = 40.0f;
                probe.m_Resolution = 256;
                probe.m_Intensity = 1.0f;
                probe.m_Active = false; // toggled per capture
            }
        }

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the top of the frame.
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir: " << ec.message();

            const std::string path = (dir / (TestRenderBackend::EvidenceName("ReflectionProbeParallax") +
                                             GetParam().Name + "_" + tag + ".png"))
                                         .string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
            if (w == static_cast<int>(kWidth) && h == static_cast<int>(kHeight))
            {
                // PNG is lossless — the evidence on disk must be the exact
                // pixels the contracts below measured (the sibling
                // ReflectionProbeVisualEvidenceTest's check).
                EXPECT_EQ(std::memcmp(loaded, outPixels.data(),
                                      static_cast<std::size_t>(kWidth) * kHeight * 4u),
                          0)
                    << "Reloaded PNG pixels differ from the written buffer: " << path;
            }
            ::stbi_image_free(loaded);
        }
    };

    TEST_P(ReflectionProbeParallaxVisualEvidenceTest, MirrorFloorReflectsTheCorridorWithRealParallax)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (GetParam().DeferredSamples > 1u && GetParam().DeferredSamples > Renderer3D::GetMaxMSAASamples())
            GTEST_SKIP() << "The device cannot render the requested " << GetParam().DeferredSamples << " samples";

        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = GetParam().Path;
        settings.Deferred.MSAASampleCount = GetParam().DeferredSamples;
        settings.Deferred.PerSampleLighting = true;
        Renderer3D::ApplyRendererSettings();

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } scopedMockTime(kCaptureTime);

        EditorCamera warmupCamera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        RunEditorFrames(warmupCamera, 2u);
        if (GetParam().Path == RenderingPath::Deferred)
        {
            const auto* scenePass = static_cast<const SceneRenderPass*>(
                Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry));
            ASSERT_TRUE(scenePass && scenePass->GetGBuffer());
            EXPECT_EQ(scenePass->GetGBuffer()->GetSampleCount(), GetParam().DeferredSamples);
        }

        Ref<Scene> sceneRef = GetSceneRef();
        auto& probe = m_Probe.GetComponent<ReflectionProbeComponent>();
        const glm::vec3 probePos = m_Probe.GetComponent<TransformComponent>().Translation;

        bool baked = false;
        RunInFrame([&]
                   {
                       if (TestRenderBackend::IsVulkanProcess())
                       {
                           baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
                       }
                       else
                       {
                           GLStateGuard bakeGuard("ReflectionProbeParallax::Bake", GLStateGuard::Policy::Restore);
                           baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
                       } });
        ASSERT_TRUE(baked) << "ReflectionProbeBaker::BakeProbe failed";
        ASSERT_TRUE(probe.m_BakedEnvironment && probe.m_BakedEnvironment->HasIBL());
        ASSERT_TRUE(probe.m_BakedEnvironment->HasProbeDistanceField())
            << "The bake produced no distance field — the #705 capture half is broken";

        // Bake-correctness spot check straight off the field: the probe at
        // (0,2,0) must see the red wall ~3.25 away along -X, the floor ~2
        // below, the blue wall ~12.25 along -Z — and everything finite.
        {
            const auto& field = probe.m_BakedEnvironment->GetProbeDistanceField();
            EXPECT_NEAR(field->SampleNearest({ -1.0f, 0.0f, 0.0f }, 0), kHalfWidth + 0.25f, 0.6f)
                << "-X distance should be the red wall";
            EXPECT_NEAR(field->SampleNearest({ 0.0f, -1.0f, 0.0f }, 0), 2.0f, 0.5f)
                << "-Y distance should be the mirror floor";
            EXPECT_NEAR(field->SampleNearest({ 0.0f, 0.0f, -1.0f }, 0), kHalfLength + 0.25f, 1.0f)
                << "-Z distance should be the blue wall";
            EXPECT_LT(field->GetMaxFiniteDistance(), 30.0f)
                << "A closed corridor must have a finite dMax well under the far plane";
        }

        Renderer3D::OnWindowResize(kWidth, kHeight);
        SetViewport(kWidth, kHeight);

        // --- Pose 1: "FloorBehind" — the SSR-failure case. Camera high in the
        // middle, pitched steeply down, FACING the yellow (+Z) end: the wall
        // itself is off-screen above the frame, only its floor reflection can
        // be yellow. EditorCamera yaw 0 looks toward -Z, so yaw pi turns it
        // toward +Z.
        const glm::vec3 behindPos{ 0.0f, 2.6f, 2.0f };
        const f32 behindYaw = glm::pi<f32>();
        const f32 behindPitch = 0.9f; // steeply down (EditorCamera pitch is +down)

        // --- Pose 2: "FloorAhead" — down the corridor toward the blue end,
        // gently pitched down so the floor fills the lower half.
        const glm::vec3 aheadPos{ 0.0f, 1.8f, 9.0f };
        const f32 aheadYaw = 0.0f;
        const f32 aheadPitch = 0.35f;

        // Floor strips for the left/right parallax split, in UV space of the
        // FloorAhead frame: lower third of the image, left and right sixths.
        constexpr f32 sy0 = 0.70f, sy1 = 0.95f;
        constexpr f32 lx0 = 0.06f, lx1 = 0.28f;
        constexpr f32 rx0 = 0.72f, rx1 = 0.94f;
        // Yellow-reflection band for FloorBehind. Verified against the
        // rendered frame: with the camera pitched ~52° down toward the +Z
        // end, distant floor rows land at the TOP of the image, so the
        // off-screen yellow wall's reflection occupies the upper-centre
        // (roughly x 0.30-0.72, y 0.00-0.22); below it the floor reflects
        // the grey ceiling.
        constexpr f32 by0 = 0.03f, by1 = 0.18f, bx0 = 0.38f, bx1 = 0.62f;

        // --- Off ---
        probe.m_Active = false;
        std::vector<u8> offBehind, offAhead;
        Capture("Off_FloorBehind", behindPos, behindYaw, behindPitch, offBehind);
        if (::testing::Test::HasFatalFailure())
            return;
        Capture("Off_FloorAhead", aheadPos, aheadYaw, aheadPitch, offAhead);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- Legacy (distance field stripped: pre-#705 global override only) ---
        probe.m_Active = true;
        Ref<ReflectionProbeDistanceField> savedField = probe.m_BakedEnvironment->GetProbeDistanceField();
        probe.m_BakedEnvironment->SetProbeDistanceField(nullptr);
        std::vector<u8> legacyAhead, legacyBehind;
        Capture("Legacy_FloorAhead", aheadPos, aheadYaw, aheadPitch, legacyAhead);
        if (::testing::Test::HasFatalFailure())
            return;
        Capture("Legacy_FloorBehind", behindPos, behindYaw, behindPitch, legacyBehind);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- Parallax (the #705 path) ---
        probe.m_BakedEnvironment->SetProbeDistanceField(savedField);
        std::vector<u8> parBehind, parAhead, parLeft, parOblique;
        Capture("Parallax_FloorBehind", behindPos, behindYaw, behindPitch, parBehind);
        if (::testing::Test::HasFatalFailure())
            return;
        Capture("Parallax_FloorAhead", aheadPos, aheadYaw, aheadPitch, parAhead);
        if (::testing::Test::HasFatalFailure())
            return;
        // Two more angles for the >= 4-angle acceptance requirement.
        Capture("Parallax_NearRedWall", { -2.0f, 1.6f, 6.0f }, 0.0f, 0.4f, parLeft);
        if (::testing::Test::HasFatalFailure())
            return;
        Capture("Parallax_ObliqueFromBlueEnd", { 1.5f, 2.4f, -8.0f }, glm::pi<f32>(), 0.35f, parOblique);
        if (::testing::Test::HasFatalFailure())
            return;

        // ---- Contracts ----
        for (const auto* frame : { &offBehind, &offAhead, &legacyAhead, &parBehind, &parAhead,
                                   &parLeft, &parOblique })
        {
            EXPECT_GT(MeanLuma(*frame), 4.0) << "a capture rendered (near-)black";
        }

        // The two extra acceptance angles carry their own positional
        // contracts (band placement verified against the rendered frames):
        //
        // NearRedWall (camera hugging the red wall, facing the blue end):
        // the red wall's reflection stripe runs down the left-centre floor,
        // and the blue end wall reflects in the mid-distance floor.
        {
            const BandStats redStripe = SampleBand(parLeft, 0.22f, 0.36f, 0.62f, 0.90f);
            EXPECT_GT(redStripe.R, redStripe.G + 10.0)
                << "NearRedWall: the floor stripe under the red wall is not red-dominant (R="
                << redStripe.R << " G=" << redStripe.G
                << "). See ReflectionProbeParallax_Parallax_NearRedWall.png";
            const BandStats blueFloor = SampleBand(parLeft, 0.50f, 0.63f, 0.26f, 0.38f);
            EXPECT_GT(blueFloor.B, blueFloor.G + 10.0)
                << "NearRedWall: the mid-floor is not reflecting the blue end wall (B="
                << blueFloor.B << " G=" << blueFloor.G
                << "). See ReflectionProbeParallax_Parallax_NearRedWall.png";
        }
        // ObliqueFromBlueEnd (facing the yellow end): the yellow wall
        // reflects in the floor directly beneath it, and the side-wall
        // reflections swap sides (green left, red right) with the view.
        {
            const BandStats yellowFloor = SampleBand(parOblique, 0.46f, 0.62f, 0.36f, 0.48f);
            EXPECT_GT(yellowFloor.R, yellowFloor.B + 15.0)
                << "ObliqueFromBlueEnd: the floor below the yellow wall is not yellow-dominant (R="
                << yellowFloor.R << " B=" << yellowFloor.B
                << "). See ReflectionProbeParallax_Parallax_ObliqueFromBlueEnd.png";
            EXPECT_GT(yellowFloor.G, yellowFloor.B + 15.0)
                << "ObliqueFromBlueEnd: yellow floor reflection missing its G half (G="
                << yellowFloor.G << " B=" << yellowFloor.B << ")";
            const BandStats leftStrip = SampleBand(parOblique, 0.08f, 0.30f, 0.68f, 0.92f);
            const BandStats rightStrip = SampleBand(parOblique, 0.70f, 0.92f, 0.68f, 0.92f);
            EXPECT_GT(leftStrip.G, leftStrip.R + 10.0)
                << "ObliqueFromBlueEnd: left floor strip should reflect the green wall (R="
                << leftStrip.R << " G=" << leftStrip.G << ")";
            EXPECT_GT(rightStrip.R, rightStrip.G + 10.0)
                << "ObliqueFromBlueEnd: right floor strip should reflect the red wall (R="
                << rightStrip.R << " G=" << rightStrip.G << ")";
        }

        // (1) Behind-the-camera reflection: the floor band reads yellow
        // (red+green high, blue low) only with the parallax probe on.
        const BandStats behindPar = SampleBand(parBehind, bx0, bx1, by0, by1);
        const BandStats behindOff = SampleBand(offBehind, bx0, bx1, by0, by1);
        EXPECT_GT(behindPar.R, behindPar.B + 15.0)
            << "FloorBehind band is not yellow-dominant (R=" << behindPar.R << " G=" << behindPar.G
            << " B=" << behindPar.B << ") — the floor is not reflecting the off-screen yellow wall. "
            << "See ReflectionProbeParallax_Parallax_FloorBehind.png";
        EXPECT_GT(behindPar.G, behindPar.B + 15.0)
            << "FloorBehind band is not yellow-dominant in G (G=" << behindPar.G
            << " B=" << behindPar.B << "). See ReflectionProbeParallax_Parallax_FloorBehind.png";
        EXPECT_GT(behindPar.R, behindOff.R + 15.0)
            << "The probe did not measurably add the behind-camera reflection (off.R="
            << behindOff.R << " parallax.R=" << behindPar.R << ")";

        // (2) Positional parallax: the strip by the red wall reflects red,
        // the strip by the green wall reflects green.
        const BandStats leftPar = SampleBand(parAhead, lx0, lx1, sy0, sy1);
        const BandStats rightPar = SampleBand(parAhead, rx0, rx1, sy0, sy1);
        EXPECT_GT(leftPar.R, leftPar.G + 10.0)
            << "Left floor strip is not red-dominant (R=" << leftPar.R << " G=" << leftPar.G
            << ") — no positional parallax. See ReflectionProbeParallax_Parallax_FloorAhead.png";
        EXPECT_GT(rightPar.G, rightPar.R + 10.0)
            << "Right floor strip is not green-dominant (R=" << rightPar.R << " G=" << rightPar.G
            << "). See ReflectionProbeParallax_Parallax_FloorAhead.png";

        // (3) Measurable improvement over the legacy probe path: the
        // left/right red-green split collapses without the distance field
        // (direction-only lookups cannot vary with surface position).
        const BandStats leftLeg = SampleBand(legacyAhead, lx0, lx1, sy0, sy1);
        const BandStats rightLeg = SampleBand(legacyAhead, rx0, rx1, sy0, sy1);
        // The improvement claim needs a WORKING baseline: a black legacy
        // floor would make splitLeg ~0 and pass the comparison vacuously
        // (the whole-frame luma check cannot see it — the emissive walls
        // dominate the mean). The legacy floor renders the direction-only
        // probe reflection, ~(104,100,100) grey in practice.
        EXPECT_GT(leftLeg.R + leftLeg.G + leftLeg.B, 60.0)
            << "Legacy left floor strip is (near-)black — the legacy baseline did not render. "
            << "See ReflectionProbeParallax_Legacy_FloorAhead.png";
        EXPECT_GT(rightLeg.R + rightLeg.G + rightLeg.B, 60.0)
            << "Legacy right floor strip is (near-)black — the legacy baseline did not render. "
            << "See ReflectionProbeParallax_Legacy_FloorAhead.png";
        const f64 splitPar = (leftPar.R - leftPar.G) + (rightPar.G - rightPar.R);
        const f64 splitLeg = (leftLeg.R - leftLeg.G) + (rightLeg.G - rightLeg.R);
        ::testing::Test::RecordProperty("ParallaxSplit", static_cast<int>(splitPar));
        ::testing::Test::RecordProperty("LegacySplit", static_cast<int>(splitLeg));
        ::testing::Test::RecordProperty("OffscreenRedGain", static_cast<int>(behindPar.R - behindOff.R));
        EXPECT_GT(splitPar, splitLeg + 20.0)
            << "The parallax path's left/right wall split (" << splitPar
            << ") is not measurably better than the legacy direction-only path (" << splitLeg
            << "). See ReflectionProbeParallax_{Parallax,Legacy}_FloorAhead.png";

        // (4) Clean fallback: with the probe off, the floor strips show no
        // wall-colour split at all.
        const BandStats leftOff = SampleBand(offAhead, lx0, lx1, sy0, sy1);
        const BandStats rightOff = SampleBand(offAhead, rx0, rx1, sy0, sy1);
        EXPECT_LT(std::abs((leftOff.R - leftOff.G) - (rightOff.R - rightOff.G)), 15.0)
            << "Probe-Off floor strips differ left/right — the baseline is contaminated. "
            << "See ReflectionProbeParallax_Off_FloorAhead.png";
    }

    // =========================================================================
    // Occluder silhouettes (issue #1804).
    //
    // The corridor again, plus the orange pillar of ReflectionProbeCorridor.olo
    // at (1.4, 1, -5), 0.6 x 2 x 0.6, standing 0.125 m above the mirror floor
    // (the authored gap is kept as the control: the reference below has it
    // too). The camera is the issue's: (0, 1.8, 9) looking at (0, -1.6, 0),
    // 60 degree vertical field of view, 941 x 628.
    //
    // A single radial distance per direction treats everything behind the
    // pillar, as seen from the probe, as solid: floor rays that pass beside
    // the pillar and behind it reported a pillar hit, which stretched its
    // reflection 31 px to the right. The reference is a geometric mirror:
    // every pixel's camera ray is traced to the floor top, reflected, and
    // tested against the pillar box on the CPU. The contracts compare the
    // rendered orange mask on the floor with that reference, with a 2 px
    // tolerance band for filtering at the edges.
    // =========================================================================
    namespace
    {
        constexpr u32 kPillarWidth = 941;
        constexpr u32 kPillarHeight = 628;
        constexpr f32 kFloorTop = -0.125f;
        const glm::vec3 kPillarMin{ 1.1f, 0.0f, -5.3f };
        const glm::vec3 kPillarMax{ 1.7f, 2.0f, -4.7f };

        // Slab test; returns the entry distance or a negative value on a miss.
        [[nodiscard]] f32 RayBox(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& lo, const glm::vec3& hi)
        {
            f32 tNear = 0.0f;
            f32 tFar = 1.0e30f;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(dir[axis]) < 1.0e-8f)
                {
                    if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
                        return -1.0f;
                    continue;
                }
                f32 t0 = (lo[axis] - origin[axis]) / dir[axis];
                f32 t1 = (hi[axis] - origin[axis]) / dir[axis];
                if (t0 > t1)
                    std::swap(t0, t1);
                tNear = std::max(tNear, t0);
                tFar = std::min(tFar, t1);
                if (tNear > tFar)
                    return -1.0f;
            }
            return tNear;
        }

        enum class MirrorPixel : u8
        {
            Other,       // not the floor (walls, the pillar itself, ...)
            Floor,       // floor that does not reflect the pillar
            FloorPillar, // floor whose ideal mirror ray hits the pillar
        };

        [[nodiscard]] bool IsOrange(const u8* px)
        {
            const f32 r = px[0];
            const f32 g = px[1];
            const f32 b = px[2];
            // The emissive orange reads (179, 155, 86) after tone mapping. The
            // walls do not pass: red and green fail the blue test, yellow and
            // the grey ceiling fail R >= G + 10.
            return r > 110.0f && r >= g + 10.0f && b < 0.7f * g;
        }

        struct SilhouetteMetrics
        {
            u32 ReferencePixels = 0;
            u32 RenderedPixels = 0;
            u32 Extra = 0;   // rendered orange more than 2 px outside the reference
            u32 Missing = 0; // reference more than 2 px inside, not rendered orange
            i32 MaxRightOvershoot = 0;
            u32 WorstRow = 0;
        };
    } // namespace

    class ReflectionProbePillarSilhouetteTest : public RendererAttachedTest,
                                                public ::testing::WithParamInterface<ProbePath>
    {
      protected:
        Entity m_Probe;

        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kPillarWidth, kPillarHeight);
            scene.SetLightGizmosVisible(false);
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);

            const auto addBox = [&scene](const char* name, const glm::vec3& pos, const glm::vec3& scale,
                                         const glm::vec3& baseColor, const glm::vec3& emissive, f32 metallic,
                                         f32 roughness)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(baseColor, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
                mat.m_Material.SetMetallicFactor(metallic);
                mat.m_Material.SetRoughnessFactor(roughness);
            };
            const glm::vec3 dark(0.02f);
            // ReflectionProbeCorridor.olo's geometry, emissive so the
            // reflection colours do not depend on the lights.
            addBox("Wall-X (red)", { -3.25f, 2.0f, 0.0f }, { 0.25f, 5.0f, 25.0f }, dark, { 0.9f, 0.04f, 0.04f }, 0.0f, 1.0f);
            addBox("Wall+X (green)", { 3.25f, 2.0f, 0.0f }, { 0.25f, 5.0f, 25.0f }, dark, { 0.04f, 0.9f, 0.04f }, 0.0f, 1.0f);
            addBox("Wall-Z (blue)", { 0.0f, 2.0f, -12.25f }, { 7.0f, 5.0f, 0.25f }, dark, { 0.04f, 0.04f, 0.9f }, 0.0f, 1.0f);
            addBox("Wall+Z (yellow)", { 0.0f, 2.0f, 12.25f }, { 7.0f, 5.0f, 0.25f }, dark, { 0.9f, 0.9f, 0.04f }, 0.0f, 1.0f);
            addBox("Ceiling", { 0.0f, 4.5f, 0.0f }, { 7.0f, 0.25f, 25.0f }, dark, { 0.15f, 0.15f, 0.15f }, 0.0f, 1.0f);
            addBox("MirrorFloor", { 0.0f, -0.25f, 0.0f }, { 7.0f, 0.25f, 25.0f }, glm::vec3(1.0f), glm::vec3(0.0f), 1.0f, 0.08f);
            addBox("Pillar", { 1.4f, 1.0f, -5.0f }, { 0.6f, 2.0f, 0.6f }, dark, { 0.85f, 0.5f, 0.1f }, 0.0f, 1.0f);

            m_Probe = scene.CreateEntity("CorridorProbe");
            m_Probe.GetComponent<TransformComponent>().Translation = { 0.0f, 2.0f, 0.0f };
            auto& probe = m_Probe.AddComponent<ReflectionProbeComponent>();
            probe.m_InfluenceRadius = 40.0f;
            probe.m_Resolution = 256;
            probe.m_Intensity = 1.0f;
            probe.m_Active = true;
        }

        [[nodiscard]] static EditorCamera IssueCamera()
        {
            EditorCamera camera(60.0f, static_cast<f32>(kPillarWidth) / static_cast<f32>(kPillarHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kPillarWidth), static_cast<f32>(kPillarHeight));
            // Looking from (0, 1.8, 9) at (0, -1.6, 0): pitch = atan(3.4 / 9), +down.
            camera.SetPose({ 0.0f, 1.8f, 9.0f }, 0.0f, std::atan2(3.4f, 9.0f));
            return camera;
        }

        // The geometric mirror reference, rows top-down.
        [[nodiscard]] static std::vector<MirrorPixel> BuildReference(const EditorCamera& camera)
        {
            const glm::mat4 invViewProj = glm::inverse(camera.GetViewProjection());
            const glm::vec3 eye = camera.GetPosition();
            std::vector<MirrorPixel> out(static_cast<sizet>(kPillarWidth) * kPillarHeight, MirrorPixel::Other);
            for (u32 y = 0; y < kPillarHeight; ++y)
            {
                for (u32 x = 0; x < kPillarWidth; ++x)
                {
                    const f32 ndcX = ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(kPillarWidth)) * 2.0f - 1.0f;
                    const f32 ndcY = 1.0f - ((static_cast<f32>(y) + 0.5f) / static_cast<f32>(kPillarHeight)) * 2.0f;
                    const glm::vec4 farPoint = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                    const glm::vec3 dir = glm::normalize(glm::vec3(farPoint) / farPoint.w - eye);
                    if (dir.y >= 0.0f)
                        continue;
                    const f32 tFloor = (kFloorTop - eye.y) / dir.y;
                    const glm::vec3 hit = eye + dir * tFloor;
                    if (std::abs(hit.x) > 3.125f || std::abs(hit.z) > 12.125f)
                        continue;
                    const f32 tPillar = RayBox(eye, dir, kPillarMin, kPillarMax);
                    if (tPillar >= 0.0f && tPillar < tFloor)
                        continue; // the pillar itself, in front of the floor
                    const glm::vec3 reflected(dir.x, -dir.y, dir.z);
                    const bool seesPillar = RayBox(hit, reflected, kPillarMin, kPillarMax) >= 0.0f;
                    out[static_cast<sizet>(y) * kPillarWidth + x] = seesPillar ? MirrorPixel::FloorPillar : MirrorPixel::Floor;
                }
            }
            return out;
        }

        [[nodiscard]] static SilhouetteMetrics Measure(const std::vector<MirrorPixel>& reference, const std::vector<u8>& rgba)
        {
            const auto at = [&](i32 x, i32 y) -> MirrorPixel
            {
                if (x < 0 || y < 0 || x >= static_cast<i32>(kPillarWidth) || y >= static_cast<i32>(kPillarHeight))
                    return MirrorPixel::Other;
                return reference[static_cast<sizet>(y) * kPillarWidth + static_cast<sizet>(x)];
            };
            const auto withinTwoPixels = [&](i32 x, i32 y, MirrorPixel kind)
            {
                for (i32 dy = -2; dy <= 2; ++dy)
                    for (i32 dx = -2; dx <= 2; ++dx)
                        if (at(x + dx, y + dy) == kind)
                            return true;
                return false;
            };
            SilhouetteMetrics m;
            for (i32 y = 0; y < static_cast<i32>(kPillarHeight); ++y)
            {
                i32 refRight = -1;
                i32 gotRight = -1;
                for (i32 x = 0; x < static_cast<i32>(kPillarWidth); ++x)
                {
                    const MirrorPixel kind = at(x, y);
                    if (kind == MirrorPixel::Other)
                        continue;
                    const bool orange = IsOrange(&rgba[(static_cast<sizet>(y) * kPillarWidth + static_cast<sizet>(x)) * 4u]);
                    if (kind == MirrorPixel::FloorPillar)
                    {
                        ++m.ReferencePixels;
                        refRight = x;
                        if (!orange && !withinTwoPixels(x, y, MirrorPixel::Floor))
                            ++m.Missing;
                    }
                    if (orange)
                    {
                        ++m.RenderedPixels;
                        gotRight = x;
                        if (kind == MirrorPixel::Floor && !withinTwoPixels(x, y, MirrorPixel::FloorPillar))
                            ++m.Extra;
                    }
                }
                if (refRight >= 0 && gotRight >= 0 && gotRight - refRight > m.MaxRightOvershoot)
                {
                    m.MaxRightOvershoot = gotRight - refRight;
                    m.WorstRow = static_cast<u32>(y);
                }
            }
            return m;
        }
    };

    TEST_P(ReflectionProbePillarSilhouetteTest, PillarReflectionFollowsTheGeometricMirror)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = GetParam().Path;
        settings.Deferred.MSAASampleCount = GetParam().DeferredSamples;
        Renderer3D::ApplyRendererSettings();

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } scopedMockTime(kCaptureTime);

        EditorCamera camera = IssueCamera();
        RunEditorFrames(camera, 2u);

        Ref<Scene> sceneRef = GetSceneRef();
        auto& probe = m_Probe.GetComponent<ReflectionProbeComponent>();
        const glm::vec3 probePos = m_Probe.GetComponent<TransformComponent>().Translation;
        bool baked = false;
        RunInFrame([&]
                   {
                       if (TestRenderBackend::IsVulkanProcess())
                       {
                           baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
                       }
                       else
                       {
                           GLStateGuard bakeGuard("ReflectionProbePillar::Bake", GLStateGuard::Policy::Restore);
                           baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
                       } });
        ASSERT_TRUE(baked) << "ReflectionProbeBaker::BakeProbe failed";
        ASSERT_TRUE(probe.m_BakedEnvironment && probe.m_BakedEnvironment->HasProbeDistanceField());

        Renderer3D::OnWindowResize(kPillarWidth, kPillarHeight);
        SetViewport(kPillarWidth, kPillarHeight);
        const std::vector<MirrorPixel> reference = BuildReference(camera);
        const fs::path dir = fs::path("assets") / "tests" / "visual";
        std::error_code ec;
        fs::create_directories(dir, ec);

        // Renders the current probe state, writes the frame and the reference
        // overlaid on it (magenta: the reference has the pillar and the frame
        // does not; cyan: orange outside the reference), and measures it.
        const auto captureArm = [&](const std::string& arm, SilhouetteMetrics& out)
        {
            RunEditorFrames(camera, 2u);
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            ASSERT_TRUE(fb) << "No composited framebuffer";
            std::vector<u8> rgba;
            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kPillarWidth, kPillarHeight, rgba);
            ASSERT_EQ(rgba.size(), static_cast<sizet>(kPillarWidth) * kPillarHeight * 4u);
            const sizet rowBytes = static_cast<sizet>(kPillarWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kPillarHeight / 2u; ++y)
            {
                u8* top = rgba.data() + static_cast<sizet>(y) * rowBytes;
                u8* bot = rgba.data() + static_cast<sizet>(kPillarHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
            out = Measure(reference, rgba);

            std::vector<u8> overlay = rgba;
            for (sizet i = 0; i < reference.size(); ++i)
            {
                const bool orange = IsOrange(&rgba[i * 4u]);
                if (reference[i] == MirrorPixel::FloorPillar && !orange)
                {
                    overlay[i * 4u + 0] = 255;
                    overlay[i * 4u + 1] = 0;
                    overlay[i * 4u + 2] = 255;
                }
                else if (reference[i] == MirrorPixel::Floor && orange)
                {
                    overlay[i * 4u + 0] = 0;
                    overlay[i * 4u + 1] = 255;
                    overlay[i * 4u + 2] = 255;
                }
            }
            const std::string base = TestRenderBackend::EvidenceName(arm) + GetParam().Name;
            EXPECT_NE(::stbi_write_png((dir / (base + "_Front.png")).string().c_str(), static_cast<int>(kPillarWidth),
                                       static_cast<int>(kPillarHeight), 4, rgba.data(), static_cast<int>(rowBytes)),
                      0);
            EXPECT_NE(::stbi_write_png((dir / (base + "_MirrorDiff.png")).string().c_str(),
                                       static_cast<int>(kPillarWidth), static_cast<int>(kPillarHeight), 4,
                                       overlay.data(), static_cast<int>(rowBytes)),
                      0);
            std::cout << "[PillarSilhouette] " << base << ": reference " << out.ReferencePixels
                      << " px, rendered orange " << out.RenderedPixels << " px, extra " << out.Extra << " px, missing "
                      << out.Missing << " px, max right overshoot " << out.MaxRightOvershoot << " px (row "
                      << out.WorstRow << ")\n";
        };

        // Control: the same bake collapsed to its front layer (d1 = d2 = d0,
        // no gap anywhere), which is exactly the single-distance march this
        // issue replaced. It must still show the stretch, or the contract
        // below could not see it.
        const Ref<EnvironmentMap> layeredEnvironment = probe.m_BakedEnvironment;
        const Ref<ReflectionProbeDistanceField> layered = layeredEnvironment->GetProbeDistanceField();
        Ref<ReflectionProbeDistanceField> frontOnly;
        {
            const std::span<const f32> mip0 = layered->GetMip(0);
            TArray<f32> front(static_cast<i32>(mip0.size() / kProbeDistanceChannels), 0.0f);
            for (sizet i = 0; i < static_cast<sizet>(front.Num()); ++i)
            {
                front[i] = mip0[i * kProbeDistanceChannels];
            }
            frontOnly = ReflectionProbeDistanceField::Create(std::move(front), layered->GetResolution());
        }
        // ReflectionProbeArray keys its layers by EnvironmentMap, so the
        // control gets an EnvironmentMap of its own (a second bake of the same
        // scene) carrying the collapsed field; switching arms swaps the map.
        RunInFrame([&]
                   {
                       if (TestRenderBackend::IsVulkanProcess())
                       {
                           baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
                       }
                       else
                       {
                           GLStateGuard bakeGuard("ReflectionProbePillar::BakeControl", GLStateGuard::Policy::Restore);
                           baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
                       } });
        ASSERT_TRUE(baked && probe.m_BakedEnvironment && probe.m_BakedEnvironment != layeredEnvironment);
        Ref<EnvironmentMap> singleEnvironment = probe.m_BakedEnvironment;
        singleEnvironment->SetProbeDistanceField(frontOnly);
        const auto useEnvironment = [&](const Ref<EnvironmentMap>& environment)
        {
            probe.m_BakedEnvironment = environment;
        };
        useEnvironment(singleEnvironment);
        SilhouetteMetrics single;
        captureArm("ReflectionProbePillarSingleLayer", single);
        if (HasFatalFailure())
            return;

        useEnvironment(layeredEnvironment);
        SilhouetteMetrics m;
        captureArm("ReflectionProbePillar", m);
        if (HasFatalFailure())
            return;

        // Shading cost, reported, not asserted (a shared GPU box): the median
        // frame GPU time of each arm over interleaved rounds, the probe off as
        // the floor. Both probe arms run the same shader on the same RGBA16F
        // array; their difference is the gap searches the layers trigger.
        {
            auto& pool = GPUPassTimerPool::GetInstance();
            // Only frames rendered in the arm's own state, each once: the pool
            // publishes the newest frame whose queries resolved, which can be a
            // frame rendered before the arm switched the probe or its field.
            const auto medianFrameMs = [&](u32 frames)
            {
                u64 lastFrame = pool.GetCurrentFrameNumber();
                std::vector<f64> samples;
                for (u32 i = 0; i < frames * 2u && samples.size() < frames; ++i)
                {
                    RunEditorFrames(camera, 1u);
                    const auto timings = pool.GetLastFrameTimings();
                    if (timings.Frame.IsValid() && timings.FrameNumber > lastFrame)
                    {
                        lastFrame = timings.FrameNumber;
                        samples.push_back(timings.Frame.GpuMs);
                    }
                }
                if (samples.empty())
                    return std::nan("");
                std::sort(samples.begin(), samples.end());
                return samples[samples.size() / 2u];
            };
            std::array<std::vector<f64>, 3> arms;
            for (u32 round = 0; round < 3u; ++round)
            {
                probe.m_Active = false;
                arms[0].push_back(medianFrameMs(12u));
                probe.m_Active = true;
                useEnvironment(singleEnvironment);
                arms[1].push_back(medianFrameMs(12u));
                useEnvironment(layeredEnvironment);
                arms[2].push_back(medianFrameMs(12u));
            }
            const auto median = [](std::vector<f64> v)
            {
                std::sort(v.begin(), v.end());
                return v[v.size() / 2u];
            };
            std::cout << "[PillarSilhouette] " << TestRenderBackend::EvidenceName("ReflectionProbePillar")
                      << GetParam().Name << " frame GPU ms (median of 3 interleaved rounds x 12 frames): probe off "
                      << median(arms[0]) << ", single layer " << median(arms[1]) << ", layered " << median(arms[2])
                      << "\n";
        }

        const std::string base = TestRenderBackend::EvidenceName("ReflectionProbePillar") + GetParam().Name;
        ::testing::Test::RecordProperty("ReferencePixels", static_cast<int>(m.ReferencePixels));
        ::testing::Test::RecordProperty("ExtraPixels", static_cast<int>(m.Extra));
        ::testing::Test::RecordProperty("MissingPixels", static_cast<int>(m.Missing));
        ::testing::Test::RecordProperty("MaxRightOvershoot", m.MaxRightOvershoot);
        ::testing::Test::RecordProperty("SingleLayerMaxRightOvershoot", single.MaxRightOvershoot);

        ASSERT_GT(m.ReferencePixels, 1000u) << "the reference has no pillar reflection: the camera or geometry moved";
        EXPECT_GT(single.MaxRightOvershoot, 15) << "control: the single-layer probe no longer stretches the pillar, so "
                                                   "this fixture cannot see the #1804 artefact";
        EXPECT_LE(m.MaxRightOvershoot, 4) << "the reflection stretches past the mirrored pillar's right edge (row "
                                          << m.WorstRow << "). See " << base << "_MirrorDiff.png";
        EXPECT_LE(m.Extra, m.ReferencePixels / 50u) << "orange beyond the mirrored outline. See " << base << "_MirrorDiff.png";
        EXPECT_LE(m.Missing, m.ReferencePixels / 10u) << "the mirrored pillar has holes. See " << base << "_MirrorDiff.png";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, ReflectionProbePillarSilhouetteTest,
                             ::testing::Values(ProbePath{ RenderingPath::Forward, 1u, "Forward" },
                                               ProbePath{ RenderingPath::ForwardPlus, 1u, "ForwardPlus" },
                                               ProbePath{ RenderingPath::Deferred, 1u, "Deferred" }),
                             [](const auto& info)
                             { return info.param.Name; });

    INSTANTIATE_TEST_SUITE_P(AllPaths, ReflectionProbeParallaxVisualEvidenceTest,
                             ::testing::Values(ProbePath{ RenderingPath::Forward, 1u, "Forward" },
                                               ProbePath{ RenderingPath::ForwardPlus, 1u, "ForwardPlus" },
                                               ProbePath{ RenderingPath::Deferred, 1u, "Deferred" },
                                               ProbePath{ RenderingPath::Deferred, 4u, "Deferred4x" }),
                             [](const auto& info)
                             { return info.param.Name; });
} // namespace OloEngine::Tests
