// OLO_TEST_LAYER: L8
// =============================================================================
// BloomVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent, golden-free contract for the
// Bloom post-process pass (PostProcess_Bloom*.glsl / BloomRenderPass).
//
// A single bright GREEN emissive cube floats in an otherwise dark scene. With
// bloom ON the cube's brightness must bleed a green halo into the surrounding
// (otherwise dark) pixels; with bloom OFF those neighbours stay dark. The scene
// is rendered twice through the FULL Renderer3D pipeline from the same pose —
// once with bloom OFF and once with bloom ON — and both frames are written to
//   OloEditor/assets/tests/visual/Bloom_<state>.png
//
// The contract is GOLDEN-FREE and differential (robust across GPUs, no
// committed reference image):
//   1. Both frames render non-black and the emissive cube reads green at the
//      centre in both (the render actually ran).
//   2. Bloom ON spreads green into MORE neighbour pixels than bloom OFF — the
//      halo bleeds outward (the core "bloom" behaviour).
//   3. A side band just outside the cube silhouette gains green when bloom is
//      ON and reads green-dominant there (a coloured halo, not a grey wash).
//   4. The halo is LOCAL: the far corner (plain grey background) is neither
//      tinted green nor measurably brightened by bloom — it does not blow the
//      whole frame out (the bloom-blowout failure mode).
//
// The cheap bloom *math* contracts live in BloomMathTest.cpp (threshold knee +
// additive composite) and PostProcessPropertyTests.cpp (kernel weights, chain
// energy). Per the CLAUDE.md rendering rule, those prove the formula; this test
// proves the rendered frame looks right.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching WaterVisualEvidenceTest / SSRVisualEvidenceTest.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
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
        constexpr f32 kCaptureTime = 4.0f; // freeze the clock for deterministic frames

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

        // Count pixels that read as a clear green (the emissive source or its
        // halo): green channel meaningfully lit and dominant over red & blue.
        [[nodiscard]] u64 CountGreenPixels(const std::vector<u8>& px)
        {
            u64 count = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                const int r = px[i + 0];
                const int g = px[i + 1];
                const int b = px[i + 2];
                if (g > 24 && g > r + 8 && g > b + 8)
                    ++count;
            }
            return count;
        }
    } // namespace

    class BloomVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // Bloom is path-independent (it operates on the lit HDR scene
            // colour), so leave the renderer on its default path. No skybox /
            // IBL and no lights: the scene stays dark except for the emissive
            // cube, which makes the halo bleed unmistakable.

            // Bright GREEN emissive cube at the origin. Emissive radiance well
            // above the bloom threshold (1.0) so the threshold pass extracts it.
            Entity cube = scene.CreateEntity("EmissiveCube");
            auto& tc = cube.GetComponent<TransformComponent>();
            tc.Translation = { 0.0f, 0.0f, 0.0f };
            tc.Scale = { 1.0f, 1.0f, 1.0f };
            auto& mc = cube.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = cube.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.0f, 0.05f, 0.0f, 1.0f));
            mat.m_Material.SetEmissiveFactor(glm::vec4(0.0f, 6.0f, 0.0f, 1.0f));
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(1.0f);
        }

        // Render the current scene/settings from the given pose, read back the
        // composited frame (top-down rows), save it as PNG evidence, and verify
        // the PNG round-trips (write succeeded + reloads bit-identical).
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
            ASSERT_TRUE(fb) << "No composited framebuffer for bloom capture '" << tag << "'";

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
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string()
                             << "': " << ec.message();

            const std::string path = (dir / ("Bloom_" + tag + ".png")).string();
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
                EXPECT_EQ(std::memcmp(loaded, outPixels.data(),
                                      static_cast<std::size_t>(kWidth) * kHeight * 4u),
                          0)
                    << "Reloaded PNG pixels differ from the written buffer: " << path;
            }
            ::stbi_image_free(loaded);
        }
    };

    // Bloom off vs on: the bright cube must bleed a green halo into the
    // surrounding dark pixels. SKIPs without a GL 4.6 context (see file header).
    TEST_F(BloomVisualEvidenceTest, HaloBleedsFromBrightSource)
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

        // The fixture's TearDown restores PostProcessSettings, so toggling
        // BloomEnabled here cannot leak into later tests.
        auto& pp = Renderer3D::GetPostProcessSettings();

        // Head-on pose: camera on +Z looking toward the cube at the origin.
        const glm::vec3 pos{ 0.0f, 0.0f, 3.5f };
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.0f;

        // --- Bloom OFF baseline ---
        pp.BloomEnabled = false;
        std::vector<u8> offPixels;
        Capture("Off", pos, yaw, pitch, offPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- Bloom ON ---
        pp.BloomEnabled = true;
        pp.BloomThreshold = 1.0f;
        pp.BloomIntensity = 1.0f;
        pp.BloomIterations = 5;
        std::vector<u8> onPixels;
        Capture("On", pos, yaw, pitch, onPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // (1) Both frames rendered, and the cube reads green at the centre in
        //     both (the render actually ran and the emissive source is there).
        const BandStats centerOff = SampleBand(offPixels, 0.45f, 0.55f, 0.45f, 0.55f);
        const BandStats centerOn = SampleBand(onPixels, 0.45f, 0.55f, 0.45f, 0.55f);
        EXPECT_GT(centerOff.G, 40.0) << "Bloom-off centre is not lit green — emissive cube missing?";
        EXPECT_GT(centerOn.G, 40.0) << "Bloom-on centre is not lit green — emissive cube missing?";
        EXPECT_GT(centerOff.G, centerOff.R + 8.0) << "Centre is not green-dominant (bloom off)";
        EXPECT_GT(centerOn.G, centerOn.R + 8.0) << "Centre is not green-dominant (bloom on)";

        // (2) Bloom spreads green into MORE pixels — the halo bleeds outward.
        const u64 greenOff = CountGreenPixels(offPixels);
        const u64 greenOn = CountGreenPixels(onPixels);
        EXPECT_GT(greenOn, greenOff)
            << "Enabling bloom did not spread green into more pixels (off=" << greenOff
            << " on=" << greenOn << "). See Bloom_Off.png / Bloom_On.png";
        // A clear margin, not a one-pixel fluke: at least 10% more lit pixels.
        EXPECT_GT(static_cast<f64>(greenOn), static_cast<f64>(greenOff) * 1.10)
            << "Bloom halo spread is too small to be real (off=" << greenOff << " on=" << greenOn << ")";

        // (3) A side band just outside the cube silhouette gains a green halo.
        //     bx is to the right of the cube; the band spans the cube's row.
        constexpr f32 hx0 = 0.66f, hx1 = 0.82f, hy0 = 0.42f, hy1 = 0.58f;
        const BandStats haloOff = SampleBand(offPixels, hx0, hx1, hy0, hy1);
        const BandStats haloOn = SampleBand(onPixels, hx0, hx1, hy0, hy1);
        EXPECT_GT(haloOn.G, haloOff.G + 4.0)
            << "Side band did not gain green when bloom enabled (off.G=" << haloOff.G
            << " on.G=" << haloOn.G << "). See Bloom_On.png";
        EXPECT_GT(haloOn.G, haloOn.R + 3.0)
            << "Side-band halo is not green-dominant (R=" << haloOn.R << " G=" << haloOn.G
            << " B=" << haloOn.B << ") — bloom is a grey wash, not a green halo. See Bloom_On.png";

        // (4) The halo is LOCAL, not a full-frame blowout. The far corner is
        //     plain grey background in both frames (the editor clear colour is
        //     mid-grey, not black): bloom neither tints it green nor measurably
        //     brightens it.
        const BandStats cornerOff = SampleBand(offPixels, 0.0f, 0.10f, 0.0f, 0.10f);
        const BandStats cornerOn = SampleBand(onPixels, 0.0f, 0.10f, 0.0f, 0.10f);
        EXPECT_LT(cornerOn.G, cornerOn.R + 8.0)
            << "Far corner reads green (R=" << cornerOn.R << " G=" << cornerOn.G << " B=" << cornerOn.B
            << ") — the halo flooded the whole frame. See Bloom_On.png";
        EXPECT_NEAR(cornerOn.G, cornerOff.G, 12.0)
            << "Bloom changed the far corner (off.G=" << cornerOff.G << " on.G=" << cornerOn.G
            << ") — the halo should be local, not frame-wide. See Bloom_On.png";
    }
} // namespace OloEngine::Tests
