// OLO_TEST_LAYER: L8
// =============================================================================
// ClusteredLightingVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for issue #435's clustered (froxel) light grid:
// a many-light scene rendered through the FULL pipeline on the clustered
// Forward+ path, from two camera angles, with driver-independent contracts:
//
//   1. Every coloured light pool is visible and channel-dominant in the
//      clustered image — a broken cluster index (wrong slice/tile mapping)
//      blanks or mis-places pools, which kills the dominance check.
//   2. The same pools are lit on the brute-force path (ForwardPlus mode
//      Never → the classic 256-light UBO loop) — clustered ≈ brute-force
//      agreement, both STRUCTURAL (per-pool visibility + colour dominance)
//      and PHOTOMETRIC (per-band channel means within a few grey levels).
//      Both paths evaluate the SAME falloff (PBRCommon calculateAttenuation /
//      calculateSpotIntensity — the clustered evaluator carries the
//      component's quadratic coefficient in ShadowAndAttenuation.y), so a
//      formula drift between them is a bug, not a tolerance. This exact
//      drift shipped once: the fp path originally used a UE4-style windowed
//      inverse-square that ignored m_Attenuation, rendering ~1.9x brighter.
//
// PNGs land in OloEditor/assets/tests/visual/ClusteredLighting_*.png and are
// written before any assertion so a reviewer always has the frame.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kW = 1024;
        constexpr u32 kH = 768;

        struct Band
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        Band SampleBand(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const auto ix0 = static_cast<u32>(x0 * kW);
            const auto ix1 = static_cast<u32>(x1 * kW);
            const auto iy0 = static_cast<u32>(y0 * kH);
            const auto iy1 = static_cast<u32>(y1 * kH);
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kW + x) * 4u;
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
    } // namespace

    class ClusteredLightingVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // A 4x3 grid of coloured point lights hovering over a neutral floor —
        // 12 local lights crosses the Forward+ auto threshold (8) with room to
        // spare, so the clustered path genuinely engages.
        static constexpr u32 kLightColumns = 4;
        static constexpr u32 kLightRows = 3;
        static constexpr f32 kLightSpacing = 8.0f;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kW, kH);

            // Pin the clustered path explicitly (mode Always). Kill the editor
            // overlays (grid z-fights the y=0 floor, gizmos pollute bands) so
            // the evidence frames show only the lighting under test.
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::ForwardPlus;
            settings.ShowGrid = false;
            settings.ShowLightGizmos = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowCameraFrustums = false;
            Renderer3D::ApplyRendererSettings();

            // Neutral floor + a few cubes so light pools land on real geometry
            // (per docs/agent-rules/single-mesh-visual-test-lighting.md a
            // sparse scene can render near-black).
            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale, const glm::vec3& color)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh = (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(color, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
                return e;
            };

            addMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 80.0f, 1.0f, 80.0f },
                    { 0.5f, 0.5f, 0.5f });
            addMesh("CubeA", MeshPrimitive::Cube, { -6.0f, 1.0f, -4.0f }, { 2.0f, 2.0f, 2.0f },
                    { 0.6f, 0.6f, 0.6f });
            addMesh("CubeB", MeshPrimitive::Cube, { 6.0f, 1.0f, 4.0f }, { 2.0f, 2.0f, 2.0f },
                    { 0.6f, 0.6f, 0.6f });

            // Coloured point lights: column index selects R / G / B / warm-white
            // so each pool's dominant channel is knowable from its position.
            const std::array<glm::vec3, kLightColumns> colors = {
                glm::vec3(1.0f, 0.1f, 0.1f),
                glm::vec3(0.1f, 1.0f, 0.1f),
                glm::vec3(0.1f, 0.1f, 1.0f),
                glm::vec3(1.0f, 0.9f, 0.6f),
            };

            for (u32 row = 0; row < kLightRows; ++row)
            {
                for (u32 col = 0; col < kLightColumns; ++col)
                {
                    Entity light = scene.CreateEntity("PointLight");
                    auto& tc = light.GetComponent<TransformComponent>();
                    tc.Translation = LightPosition(col, row);
                    auto& pl = light.AddComponent<PointLightComponent>();
                    pl.m_Color = colors[col];
                    pl.m_Intensity = 30.0f;
                    pl.m_Range = 7.0f;
                }
            }
        }

        [[nodiscard]] static glm::vec3 LightPosition(u32 col, u32 row)
        {
            return {
                (static_cast<f32>(col) - (kLightColumns - 1) * 0.5f) * kLightSpacing,
                2.0f,
                (static_cast<f32>(row) - (kLightRows - 1) * 0.5f) * kLightSpacing,
            };
        }

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kW) / static_cast<f32>(kH), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kW), static_cast<f32>(kH));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kW, kH, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kW) * kH * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kW) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kH / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kH - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("ClusteredLighting_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kW), static_cast<int>(kH),
                                               4, outPixels.data(), static_cast<int>(kW) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }
    };

    TEST_F(ClusteredLightingVisualEvidenceTest, ManyLightPoolsRenderOnClusteredPathFromTwoAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Top-down-ish view over the light grid: every pool falls in a
        // predictable screen band. SetPose: yaw/pitch are RADIANS, yaw 0
        // looks toward -Z, positive pitch tilts DOWN.
        std::vector<u8> overhead;
        Capture("Clustered_Overhead", { 0.0f, 24.0f, 22.0f }, 0.0f, 0.73f, overhead);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        std::vector<u8> grazing;
        Capture("Clustered_Grazing", { -16.0f, 4.0f, 20.0f }, -0.67f, 0.14f, grazing);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Non-black overall (broken cluster lookup blanks local lighting).
        const Band whole = SampleBand(overhead, 0.05f, 0.95f, 0.05f, 0.95f);
        EXPECT_GT(whole.Luma(), 8.0) << "clustered many-light frame is near-black";

        // Colour-dominance per column band in the overhead capture: the red
        // column must be red-dominant, green green-dominant, blue
        // blue-dominant. Bands are quarters of the frame width (the grid is
        // camera-centred), inset to avoid the neighbouring pools.
        const Band redBand = SampleBand(overhead, 0.06f, 0.22f, 0.30f, 0.80f);
        const Band greenBand = SampleBand(overhead, 0.31f, 0.47f, 0.30f, 0.80f);
        const Band blueBand = SampleBand(overhead, 0.56f, 0.72f, 0.30f, 0.80f);

        EXPECT_GT(redBand.R, redBand.G * 1.1) << "red pool column not red-dominant";
        EXPECT_GT(redBand.R, redBand.B * 1.1) << "red pool column not red-dominant";
        EXPECT_GT(greenBand.G, greenBand.R * 1.1) << "green pool column not green-dominant";
        EXPECT_GT(greenBand.G, greenBand.B * 1.1) << "green pool column not green-dominant";
        EXPECT_GT(blueBand.B, blueBand.R * 1.1) << "blue pool column not blue-dominant";
        EXPECT_GT(blueBand.B, blueBand.G * 1.1) << "blue pool column not blue-dominant";

        // Grazing view also lit — depth-slice selection must work across a
        // long depth range, not just at one slice.
        const Band grazingBand = SampleBand(grazing, 0.1f, 0.9f, 0.35f, 0.9f);
        EXPECT_GT(grazingBand.Luma(), 8.0) << "grazing view near-black — depth slicing suspect";
    }

    TEST_F(ClusteredLightingVisualEvidenceTest, ClusteredAgreesStructurallyWithBruteForce)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Clustered capture (path pinned in BuildScene). Pose angles in radians.
        std::vector<u8> clustered;
        Capture("PathClustered", { 0.0f, 24.0f, 22.0f }, 0.0f, 0.73f, clustered);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Brute-force UBO loop: classic Forward with auto-switch OFF.
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Forward;
        settings.ForwardPlusAutoSwitch = false;
        Renderer3D::ApplyRendererSettings();

        std::vector<u8> bruteForce;
        Capture("PathBruteForce", { 0.0f, 24.0f, 22.0f }, 0.0f, 0.73f, bruteForce);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Agreement contracts: every colour band lit and channel-dominant in
        // BOTH captures, AND photometrically matching — both paths evaluate
        // the same PBRCommon falloff, so per-band channel means must agree
        // within a small driver/TAA tolerance. A formula drift between the
        // clustered evaluator and the UBO loop (e.g. the original fp
        // UE4-style falloff that ignored m_Attenuation and lit ~1.9x
        // brighter) fails the mean-agreement check outright.
        const struct
        {
            f32 X0, X1;
            int Channel; // 0 = R, 1 = G, 2 = B
            const char* Name;
        } bands[] = {
            { 0.06f, 0.22f, 0, "red" },
            { 0.31f, 0.47f, 1, "green" },
            { 0.56f, 0.72f, 2, "blue" },
        };

        // Grey levels (0-255) per channel. Calibration: full-suite cross-test
        // residue (TAA/exposure history) shifts the two captures by a uniform
        // ~2-3 grey levels even with identical light math, while the falloff
        // bugs this contract exists to catch measured 8-16 (fp UE4-style
        // attenuation ignoring m_Attenuation) and 4-8 (missing NdotL).
        constexpr f64 kMeanTolerance = 5.0;

        for (const auto& band : bands)
        {
            const Band c = SampleBand(clustered, band.X0, band.X1, 0.30f, 0.80f);
            const Band b = SampleBand(bruteForce, band.X0, band.X1, 0.30f, 0.80f);
            EXPECT_GT(c.Luma(), 5.0) << band.Name << " band unlit on the clustered path";
            EXPECT_GT(b.Luma(), 5.0) << band.Name << " band unlit on the brute-force path";

            const std::array<f64, 3> cRGB = { c.R, c.G, c.B };
            const std::array<f64, 3> bRGB = { b.R, b.G, b.B };
            for (int other = 0; other < 3; ++other)
            {
                if (other == band.Channel)
                    continue;
                EXPECT_GT(cRGB[static_cast<sizet>(band.Channel)], cRGB[static_cast<sizet>(other)] * 1.05)
                    << band.Name << " band lost channel dominance on the clustered path";
                EXPECT_GT(bRGB[static_cast<sizet>(band.Channel)], bRGB[static_cast<sizet>(other)] * 1.05)
                    << band.Name << " band lost channel dominance on the brute-force path";
            }

            // Photometric parity: same lights, same falloff formula, same
            // tone map — the band means must match closely on every channel.
            for (int ch = 0; ch < 3; ++ch)
            {
                EXPECT_NEAR(cRGB[static_cast<sizet>(ch)], bRGB[static_cast<sizet>(ch)], kMeanTolerance)
                    << band.Name << " band channel " << ch
                    << " diverges between clustered and brute-force — the two paths' "
                       "light falloff formulas have drifted apart";
            }
        }
    }
} // namespace OloEngine::Tests
