// OLO_TEST_LAYER: L8
// =============================================================================
// OverdrawVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the overdraw heatmap
// debug view (issue #519 / PostProcessSettings::OverdrawDebugView /
// OverdrawRenderPass). A scene with a KNOWN overdraw gradient is rendered through
// the FULL Renderer3D pipeline with the overdraw debug view active and the
// composited frame written to
//   OloEditor/assets/tests/visual/Overdraw_Heatmap.png
//
// The scene puts two comparable objects side by side:
//   * LEFT  — a single cube (one shaded layer per covered pixel).
//   * RIGHT — a tight stack of many cubes at overlapping screen positions (deep
//             overdraw: the debug view counts occluded fragments too).
// The contract is GOLDEN-FREE and differential: the deeply-overlapped stack must
// read HOTTER (much more red-shifted) than the single cube, and the empty
// background must stay black — the pixel-level proof the additive fragment
// counting actually works. The count->colour ramp itself is pinned cheaply
// without a GL context by OverdrawHeatmapMathTest.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching the other *VisualEvidenceTest fixtures.
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

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

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

        [[nodiscard]] u8 RedAt(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            return px[idx + 0];
        }

        // Mean of the red channel over a pixel rectangle (heat "warmth" proxy —
        // the ramp's red channel rises monotonically with the overdraw count).
        [[nodiscard]] f64 MeanRed(const std::vector<u8>& px, u32 x0, u32 y0, u32 x1, u32 y1)
        {
            f64 sum = 0.0;
            u32 n = 0;
            for (u32 y = y0; y < y1; ++y)
                for (u32 x = x0; x < x1; ++x)
                {
                    sum += RedAt(px, x, y);
                    ++n;
                }
            return n > 0 ? sum / n : 0.0;
        }

        [[nodiscard]] u8 MaxRed(const std::vector<u8>& px, u32 x0, u32 y0, u32 x1, u32 y1)
        {
            u8 hi = 0;
            for (u32 y = y0; y < y1; ++y)
                for (u32 x = x0; x < x1; ++x)
                    hi = std::max(hi, RedAt(px, x, y));
            return hi;
        }
    } // namespace

    class OverdrawVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // One directional light — the overdraw view ignores shading entirely
            // (it counts fragments), but a scene still needs a light to render.
            {
                Entity key = scene.CreateEntity("Key");
                key.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = key.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.4f));
                dl.m_Intensity = 2.0f;
                dl.m_CastShadows = false;
            }

            auto addCube = [&scene](const char* name, const glm::vec3& pos, const glm::vec3& scale)
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
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
                return e;
            };

            // LEFT: a single cube — one shaded layer per covered pixel.
            addCube("Single", { -4.0f, 1.0f, 0.0f }, { 2.0f, 2.0f, 2.0f });

            // RIGHT: a stack of cubes at the SAME screen position but staggered in
            // depth, so from the front camera they overlap heavily — deep overdraw.
            // Depth testing is disabled during the count, so every one of these
            // occluded layers is tallied.
            for (int i = 0; i < 8; ++i)
            {
                const f32 z = -3.5f + static_cast<f32>(i) * 0.5f;
                addCube("StackCube", { 4.0f, 1.0f, z }, { 2.0f, 2.0f, 2.0f });
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
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::OverdrawColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for overdraw capture '" << tag << "'";

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
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string() << "': " << ec.message();

            const std::string path = (dir / ("Overdraw_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                               outPixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
            ::stbi_image_free(loaded);
        }
    };

    // Overdraw view: the deeply-overlapped stack (RIGHT) must read hotter (much
    // redder) than the single cube (LEFT), and the empty background must be black.
    // SKIPs without a GL 4.6 context (see header).
    TEST_F(OverdrawVisualEvidenceTest, StackedGeometryReadsHotter)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.OverdrawDebugView = true;

        const glm::vec3 pos = { 0.0f, 2.5f, 14.0f };
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.05f;

        std::vector<u8> px;
        Capture("Heatmap", pos, yaw, pitch, px);
        if (::testing::Test::HasFatalFailure())
            return;

        // Regions (top-left origin after the flip): left third holds the single
        // cube, right third holds the stack. Sample a vertical band through the
        // middle rows where both objects sit.
        const u32 y0 = kHeight / 3u;
        const u32 y1 = (kHeight * 2u) / 3u;
        const u32 leftX0 = kWidth / 12u;
        const u32 leftX1 = kWidth / 3u;
        const u32 rightX0 = (kWidth * 2u) / 3u;
        const u32 rightX1 = (kWidth * 11u) / 12u;

        const u8 stackMaxRed = MaxRed(px, rightX0, y0, rightX1, y1);
        const u8 singleMaxRed = MaxRed(px, leftX0, y0, leftX1, y1);
        const f64 stackMeanRed = MeanRed(px, rightX0, y0, rightX1, y1);
        const f64 singleMeanRed = MeanRed(px, leftX0, y0, leftX1, y1);

        // The frame must actually contain the heatmap (not a black frame / missed
        // pass): the deep stack has to reach a hot (red-dominant) pixel.
        EXPECT_GT(stackMaxRed, 120)
            << "The overlapping stack never reached a hot (red) colour — overdraw "
               "accumulation likely did not run. See Overdraw_Heatmap.png";

        // Core contract: deeper overlap reads redder than a single layer.
        EXPECT_GT(static_cast<int>(stackMaxRed), static_cast<int>(singleMaxRed) + 40)
            << "The stacked region (max red=" << static_cast<int>(stackMaxRed)
            << ") is not meaningfully hotter than the single cube (max red="
            << static_cast<int>(singleMaxRed) << "). See Overdraw_Heatmap.png";
        EXPECT_GT(stackMeanRed, singleMeanRed)
            << "Stacked region mean red (" << stackMeanRed << ") <= single-layer mean red ("
            << singleMeanRed << ")";

        // The empty background (top corners, well above the geometry) must be
        // black — nothing drew there, so the count is 0 -> black.
        const f64 cornerRed = MeanRed(px, 0u, 0u, kWidth / 12u, kHeight / 12u);
        EXPECT_LT(cornerRed, 12.0)
            << "Background is not black in the overdraw view (corner mean red=" << cornerRed << ")";
    }
} // namespace OloEngine::Tests
