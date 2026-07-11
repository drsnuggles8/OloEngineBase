// OLO_TEST_LAYER: L8
// =============================================================================
// VolumetricFogVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for issue #435's froxel volumetric fog: a foggy
// scene with three coloured point lights rendered through the FULL pipeline
// (VolumetricFogPass compute chain → FogPass composite), from two camera
// angles, with driver-independent contracts:
//
//   1. Fog-off vs fog-on differential: distant floor bands lose contrast /
//      gain fog colour when the froxel fog is enabled (the volume actually
//      composites).
//   2. LIGHT SCATTERING (the acceptance criterion the old raymarch never
//      had): the fog around each coloured light glows with THAT light's
//      colour — channel dominance in a band around the light that is NOT
//      floor geometry. A broken cluster lookup or scatter pass kills this.
//   3. Near-vs-far gradient: farther bands are foggier (transmittance falls
//      with depth), pinning the front-to-back integration direction.
//
// PNGs land in OloEditor/assets/tests/visual/VolumetricFog_*.png and are
// written before any assertion. The fixture snapshots + restores the
// renderer FogSettings (RendererAttachedTest's settings snapshot does not
// cover them).
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

        // Channel excess (channel − max(other channels)) of the mean RGB over
        // a UV rect.
        f64 ChannelExcessIn(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1, int channel)
        {
            const Band b = SampleBand(px, x0, x1, y0, y1);
            const std::array<f64, 3> rgb = { b.R, b.G, b.B };
            f64 others = -255.0;
            for (int other = 0; other < 3; ++other)
            {
                if (other != channel)
                    others = std::max(others, rgb[static_cast<sizet>(other)]);
            }
            return rgb[static_cast<sizet>(channel)] - others;
        }

        struct PeakCell
        {
            f64 Excess = -255.0;
            f32 X0 = 0.0f, X1 = 0.0f, Y0 = 0.0f, Y1 = 0.0f;
        };

        // Peak channel excess over a UV region scanned as a grid, returning
        // the winning cell. A compact coloured glow inside a large
        // neutral-fog band is invisible to the band MEAN (the grey fog
        // dominates) but lights up the cell it occupies.
        PeakCell MaxChannelExcess(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1,
                                  int channel, u32 cells = 10)
        {
            PeakCell best;
            const f32 dx = (x1 - x0) / static_cast<f32>(cells);
            const f32 dy = (y1 - y0) / static_cast<f32>(cells);
            for (u32 cy = 0; cy < cells; ++cy)
            {
                for (u32 cx = 0; cx < cells; ++cx)
                {
                    const f32 cellX0 = x0 + dx * static_cast<f32>(cx);
                    const f32 cellY0 = y0 + dy * static_cast<f32>(cy);
                    const f64 excess = ChannelExcessIn(px, cellX0, cellX0 + dx, cellY0, cellY0 + dy, channel);
                    if (excess > best.Excess)
                        best = { excess, cellX0, cellX0 + dx, cellY0, cellY0 + dy };
                }
            }
            return best;
        }
    } // namespace

    class VolumetricFogVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void SetUp() override
        {
            RendererAttachedTest::SetUp();
            if (::testing::Test::IsSkipped())
                return;
            // The fixture's settings snapshot does not cover FogSettings —
            // save/restore them manually so a failing run can't leak dense
            // fog into later tests.
            m_SavedFog = Renderer3D::GetFogSettings();
        }

        void TearDown() override
        {
            if (!::testing::Test::IsSkipped())
                Renderer3D::GetFogSettings() = m_SavedFog;
            RendererAttachedTest::TearDown();
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kW, kH);

            // Clustered path (mode Always) so the fog's per-froxel local-light
            // scattering consumes real cluster lists. Kill the editor overlays
            // so the evidence frames show only the fog under test.
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::ForwardPlus;
            settings.ShowGrid = false;
            settings.ShowLightGizmos = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowCameraFrustums = false;
            Renderer3D::ApplyRendererSettings();

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

            // Dark floor + a few pillars so the coloured fog glow is the
            // dominant signal, not surface lighting.
            addMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 90.0f, 1.0f, 90.0f },
                    { 0.15f, 0.15f, 0.15f });
            addMesh("PillarA", MeshPrimitive::Cube, { -10.0f, 3.0f, -6.0f }, { 1.5f, 6.0f, 1.5f },
                    { 0.3f, 0.3f, 0.3f });
            addMesh("PillarB", MeshPrimitive::Cube, { 10.0f, 3.0f, -6.0f }, { 1.5f, 6.0f, 1.5f },
                    { 0.3f, 0.3f, 0.3f });

            // Three coloured point lights floating in the fog, spread left /
            // centre / right so each owns a screen third. 9 extra dim filler
            // lights push the total over the Forward+ threshold... not needed:
            // path is pinned to Always, so 3 lights suffice.
            const std::array<std::pair<glm::vec3, glm::vec3>, 3> lights = { {
                { { -12.0f, 4.0f, -2.0f }, { 1.0f, 0.1f, 0.1f } },
                { { 0.0f, 4.0f, -2.0f }, { 0.1f, 1.0f, 0.1f } },
                { { 12.0f, 4.0f, -2.0f }, { 0.15f, 0.15f, 1.0f } },
            } };
            for (const auto& [pos, color] : lights)
            {
                Entity light = scene.CreateEntity("FogLight");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = pos;
                auto& pl = light.AddComponent<PointLightComponent>();
                pl.m_Color = color;
                pl.m_Intensity = 40.0f;
                pl.m_Range = 12.0f;
            }

            // Dense volumetric fog. Noise off for determinism. Atmospheric
            // sun scattering (EnableScattering) off — it only gates the
            // Rayleigh/Mie sun term and there is no sun in this scene; the
            // cluster-light in-scatter that produces the coloured glow (the
            // evidence this test pins) is unconditional in the scatter pass.
            // Light shafts off for the same no-sun reason.
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = false; // the "off" capture; toggled on per test
            fog.EnableVolumetric = true;
            fog.Density = 0.05f;
            fog.Start = 0.0f;
            fog.End = 120.0f;
            fog.HeightFalloff = 0.0f;
            fog.HeightOffset = 0.0f;
            fog.MaxOpacity = 0.95f;
            fog.Color = { 0.35f, 0.38f, 0.42f };
            fog.EnableScattering = false;
            fog.EnableNoise = false;
            fog.EnableLightShafts = false;
        }

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kW) / static_cast<f32>(kH), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kW), static_cast<f32>(kH));
            camera.SetPose(position, yaw, pitch);

            // A few frames so the froxel scatter volume's temporal history
            // settles (first frame runs history-less).
            RunEditorFrames(camera, 4);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for fog capture '" << tag << "'";

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
            const std::string path = (dir / ("VolumetricFog_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kW), static_cast<int>(kH),
                                               4, outPixels.data(), static_cast<int>(kW) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        FogSettings m_SavedFog{};
    };

    TEST_F(VolumetricFogVisualEvidenceTest, FroxelFogCompositesAndScattersLocalLights)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Fog OFF baseline (camera slightly above the floor looking across
        // it). SetPose angles are RADIANS; positive pitch tilts DOWN.
        std::vector<u8> off;
        Capture("Off", { 0.0f, 3.0f, 18.0f }, 0.0f, 0.1f, off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Fog ON (froxel volumetric)
        Renderer3D::GetFogSettings().Enabled = true;
        std::vector<u8> on;
        Capture("On", { 0.0f, 3.0f, 18.0f }, 0.0f, 0.1f, on);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Second angle for the evidence set (looking along the light row).
        std::vector<u8> side;
        Capture("On_Side", { -22.0f, 5.0f, 10.0f }, -0.96f, 0.17f, side);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // 1. The fog composites: the UPPER half (background, no geometry —
        // near-black without fog) must brighten toward the fog/glow colours.
        const Band skyOff = SampleBand(off, 0.1f, 0.9f, 0.05f, 0.30f);
        const Band skyOn = SampleBand(on, 0.1f, 0.9f, 0.05f, 0.30f);
        EXPECT_GT(skyOn.Luma(), skyOff.Luma() + 3.0)
            << "background gained no fog in-scatter — froxel volume not compositing";

        // 2. Light scattering: the air AROUND each light (upper half, over a
        // dark background) must glow with that light's colour. The glows are
        // compact, so scan for the PEAK channel-dominant cell in each third —
        // the band mean would be swamped by the neutral (slightly blue) fog.
        const PeakCell redGlow = MaxChannelExcess(on, 0.03f, 0.30f, 0.15f, 0.45f, 0);
        const PeakCell greenGlow = MaxChannelExcess(on, 0.37f, 0.63f, 0.15f, 0.45f, 1);
        const PeakCell blueGlow = MaxChannelExcess(on, 0.70f, 0.97f, 0.15f, 0.45f, 2);

        EXPECT_GT(redGlow.Excess, 5.0) << "no red glow in fog around the red light";
        EXPECT_GT(greenGlow.Excess, 5.0) << "no green glow in fog around the green light";
        EXPECT_GT(blueGlow.Excess, 5.0) << "no blue glow in fog around the blue light";

        // The glow is FOG in-scatter, not surface lighting: at the exact cell
        // where the ON frame peaks red, the OFF frame must be LESS
        // red-dominant (the winning cell is glowing air; without fog that air
        // is empty background, whereas the red-lit PILLAR keeps its dominance
        // in both frames and would not satisfy this at the glow cell).
        const f64 offAtRedCell = ChannelExcessIn(off, redGlow.X0, redGlow.X1, redGlow.Y0, redGlow.Y1, 0);
        EXPECT_GT(redGlow.Excess, offAtRedCell + 3.0)
            << "red glow cell no more red-dominant than the same cell without fog — scattering suspect";

        // 3. Depth gradient: on the floor, the FAR band (upper floor rows)
        // must be foggier — closer to the fog luminance — than the NEAR band.
        // With a dark floor, fog raises luma with distance.
        const Band nearFloor = SampleBand(on, 0.30f, 0.70f, 0.86f, 0.97f);
        const Band farFloor = SampleBand(on, 0.30f, 0.70f, 0.52f, 0.62f);
        EXPECT_GT(farFloor.Luma(), nearFloor.Luma())
            << "far floor not foggier than near floor — integration direction suspect";
    }
} // namespace OloEngine::Tests
