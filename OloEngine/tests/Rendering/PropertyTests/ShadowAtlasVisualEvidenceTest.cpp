// OLO_TEST_LAYER: L8
// =============================================================================
// ShadowAtlasVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for issue #435's budgeted shadow atlas: SIX
// shadow-casting spot lights plus TWO shadow-casting point lights rendered
// simultaneously — beyond the old fixed 4-spot / 4-point caps, which could
// shadow at most four of the six spots. Golden-free differential contract
// (the SphereAreaLightShadowEvidenceTest pattern, robust across GPUs):
//
//   1. Capture with every light's CastShadows OFF, then ON.
//   2. Under EVERY one of the six spot occluders the floor must darken by a
//      clear margin (MaxDarkening grid scan). With the old caps at most four
//      could darken — pools five and six are the atlas acceptance proof.
//   3. Repeated on the clustered Forward+ path: tile-culled lights were
//      shadowless before #435, so the same six darkenings there pin the new
//      cluster-path atlas sampling.
//
// PNGs land in OloEditor/assets/tests/visual/ShadowAtlas_*.png.
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

        constexpr u32 kW = 1280;
        constexpr u32 kH = 720;

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

        // Greatest luma darkening (off -> on) over a UV region scanned as a
        // grid — locates the shadow pool without hard-coding a pixel band.
        f64 MaxDarkening(const std::vector<u8>& off, const std::vector<u8>& on,
                         f32 x0, f32 x1, f32 y0, f32 y1, u32 cells = 12)
        {
            f64 best = 0.0;
            const f32 dx = (x1 - x0) / static_cast<f32>(cells);
            const f32 dy = (y1 - y0) / static_cast<f32>(cells);
            for (u32 cy = 0; cy < cells; ++cy)
            {
                for (u32 cx = 0; cx < cells; ++cx)
                {
                    const f32 cellX0 = x0 + dx * static_cast<f32>(cx);
                    const f32 cellY0 = y0 + dy * static_cast<f32>(cy);
                    const Band o = SampleBand(off, cellX0, cellX0 + dx, cellY0, cellY0 + dy);
                    const Band n = SampleBand(on, cellX0, cellX0 + dx, cellY0, cellY0 + dy);
                    best = std::max(best, o.Luma() - n.Luma());
                }
            }
            return best;
        }
    } // namespace

    class ShadowAtlasVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kSpotCount = 6;
        static constexpr f32 kSpotSpacing = 7.0f;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kW, kH);

            // Pin the classic Forward path for the primary differential (its
            // shadow branch is the direct successor of the old fixed-slot
            // path). The clustered-path test switches paths per capture. Kill
            // the editor overlays so the evidence frames show only shadows.
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::Forward;
            settings.ForwardPlusAutoSwitch = false;
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

            addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 90.0f, 1.0f, 90.0f },
                    { 0.7f, 0.7f, 0.7f });

            // Six downward spot lights in a row along X, each with a floating
            // cube occluder halfway to the floor. Every (light, occluder) pair
            // is far enough from its neighbours that its shadow pool is
            // isolated on screen.
            for (u32 i = 0; i < kSpotCount; ++i)
            {
                const f32 x = SpotX(i);

                addMesh("Occluder", MeshPrimitive::Cube, { x, 3.0f, 0.0f }, { 1.6f, 0.4f, 1.6f },
                        { 0.7f, 0.7f, 0.7f });

                Entity light = scene.CreateEntity("SpotCaster");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { x, 8.0f, 0.0f };
                auto& sl = light.AddComponent<SpotLightComponent>();
                sl.m_Direction = { 0.0f, -1.0f, 0.0f };
                sl.m_Color = glm::vec3(1.0f);
                sl.m_Intensity = 60.0f;
                sl.m_Range = 16.0f;
                sl.m_InnerCutoff = 25.0f;
                sl.m_OuterCutoff = 34.0f;
                sl.m_CastShadows = false; // toggled per capture
                m_SpotLights[i] = light;
            }

            // Two point-light casters behind the spot row (their own occluders)
            // — the atlas budgets spots AND points simultaneously.
            for (u32 i = 0; i < 2; ++i)
            {
                const f32 x = (i == 0) ? -10.0f : 10.0f;
                addMesh("PointOccluder", MeshPrimitive::Cube, { x, 2.0f, -10.0f }, { 1.2f, 1.2f, 1.2f },
                        { 0.7f, 0.7f, 0.7f });

                Entity light = scene.CreateEntity("PointCaster");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { x - 3.0f, 5.0f, -13.0f };
                auto& pl = light.AddComponent<PointLightComponent>();
                pl.m_Color = glm::vec3(1.0f);
                pl.m_Intensity = 50.0f;
                pl.m_Range = 20.0f;
                pl.m_CastShadows = false; // toggled per capture
                m_PointLights[i] = light;
            }
        }

        [[nodiscard]] static f32 SpotX(u32 index)
        {
            return (static_cast<f32>(index) - (kSpotCount - 1) * 0.5f) * kSpotSpacing;
        }

        void SetAllCastShadows(bool cast)
        {
            for (auto& light : m_SpotLights)
                light.GetComponent<SpotLightComponent>().m_CastShadows = cast;
            for (auto& light : m_PointLights)
                light.GetComponent<PointLightComponent>().m_CastShadows = cast;
        }

        void Capture(const std::string& tag, std::vector<u8>& outPixels)
        {
            // Overhead-ish pose looking straight up the spot row so all six
            // pools land in one predictable horizontal strip. SetPose angles
            // are RADIANS; positive pitch tilts DOWN.
            EditorCamera camera(60.0f, static_cast<f32>(kW) / static_cast<f32>(kH), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kW), static_cast<f32>(kH));
            camera.SetPose({ 0.0f, 22.0f, 18.0f }, 0.0f, 0.84f);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for shadow-atlas capture '" << tag << "'";

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
            const std::string path = (dir / ("ShadowAtlas_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kW), static_cast<int>(kH),
                                               4, outPixels.data(), static_cast<int>(kW) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        // The horizontal UV strip each spot's shadow pool falls into for the
        // fixed overhead pose: the six lights split the middle of the frame
        // into six equal bands.
        void ExpectSixShadowPools(const std::vector<u8>& off, const std::vector<u8>& on,
                                  const char* pathName)
        {
            for (u32 i = 0; i < kSpotCount; ++i)
            {
                const f32 bandX0 = 0.08f + 0.14f * static_cast<f32>(i);
                const f32 bandX1 = bandX0 + 0.14f;
                const f64 darkening = MaxDarkening(off, on, bandX0, bandX1, 0.35f, 0.95f);
                EXPECT_GT(darkening, 8.0)
                    << pathName << ": spot " << i << " (of " << kSpotCount
                    << " — beyond the old 4-spot cap) produced no shadow pool";
            }
        }

        std::array<Entity, kSpotCount> m_SpotLights{};
        std::array<Entity, 2> m_PointLights{};
    };

    TEST_F(ShadowAtlasVisualEvidenceTest, SixSpotAndTwoPointCastersAllShadowSimultaneously)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> off;
        Capture("Forward_Off", off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        SetAllCastShadows(true);

        std::vector<u8> on;
        Capture("Forward_On", on);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Frame sanity: shadow-off frame must be lit where the pools will be.
        const Band litStrip = SampleBand(off, 0.1f, 0.9f, 0.4f, 0.9f);
        EXPECT_GT(litStrip.Luma(), 15.0) << "spot-lit floor near-black even without shadows";

        ExpectSixShadowPools(off, on, "Forward");

        // The two point casters must also darken their occluders' floor
        // regions (they sit in the upper half of the frame, behind the row).
        const f64 pointDarkeningLeft = MaxDarkening(off, on, 0.02f, 0.40f, 0.02f, 0.45f);
        const f64 pointDarkeningRight = MaxDarkening(off, on, 0.60f, 0.98f, 0.02f, 0.45f);
        EXPECT_GT(pointDarkeningLeft, 4.0) << "left point caster produced no shadow";
        EXPECT_GT(pointDarkeningRight, 4.0) << "right point caster produced no shadow";
    }

    TEST_F(ShadowAtlasVisualEvidenceTest, ClusteredPathSamplesAtlasShadowsToo)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Clustered Forward+ (mode Always): before issue #435 tile-culled
        // lights received NO shadow attenuation at all, so all six pools
        // darkening on this path pins the new cluster-path atlas sampling.
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::ForwardPlus;
        Renderer3D::ApplyRendererSettings();

        std::vector<u8> off;
        Capture("Clustered_Off", off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        SetAllCastShadows(true);

        std::vector<u8> on;
        Capture("Clustered_On", on);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        const Band litStrip = SampleBand(off, 0.1f, 0.9f, 0.4f, 0.9f);
        EXPECT_GT(litStrip.Luma(), 15.0) << "clustered spot-lit floor near-black even without shadows";

        ExpectSixShadowPools(off, on, "Clustered");
    }
} // namespace OloEngine::Tests
