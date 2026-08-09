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

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
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

    class ReflectionProbeParallaxVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Probe;

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

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
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

            const std::string path = (dir / ("ReflectionProbeParallax_" + tag + ".png")).string();
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

    TEST_F(ReflectionProbeParallaxVisualEvidenceTest, MirrorFloorReflectsTheCorridorWithRealParallax)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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

        Ref<Scene> sceneRef = GetSceneRef();
        auto& probe = m_Probe.GetComponent<ReflectionProbeComponent>();
        const glm::vec3 probePos = m_Probe.GetComponent<TransformComponent>().Translation;

        bool baked = false;
        {
            GLStateGuard bakeGuard("ReflectionProbeParallax::Bake", GLStateGuard::Policy::Restore);
            baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
        }
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
} // namespace OloEngine::Tests
