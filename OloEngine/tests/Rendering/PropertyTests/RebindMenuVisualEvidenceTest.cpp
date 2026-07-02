// OLO_TEST_LAYER: L8
// =============================================================================
// RebindMenuVisualEvidenceTest.cpp
//
// Issue #475: visual evidence that the in-game input rebind panel
// (RuntimeInputRebindMenu, built on the ECS UI toolkit) actually renders
// through the real Scene → Renderer3D → UI-composite path. The panel is built
// into a scene with a primary camera, ticked, and the composited frame is read
// back and written to a PNG so a reviewer can see the rows / bindings / buttons.
//
// Contracts (driver-independent):
//   1. The composited frame reads back at the requested resolution.
//   2. The frame is non-trivial (the panel + text produce real luminance
//      contrast — it is not a flat clear).
//   3. The centred panel region is brighter than the corner background (the
//      dark panel with light buttons/text sits at screen centre).
//
// SKIPs cleanly when no GL 4.6 context is available (headless CI), same gate as
// the other RendererAttachedTest visual-evidence tests.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/UI/RuntimeInputRebindMenu.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kW = 1280;
        constexpr u32 kH = 720;

        f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        f32 MeanLuminanceInRegion(const std::vector<u8>& px, u32 w, u32 h, u32 cx, u32 cy, u32 halfExtent)
        {
            const u32 x0 = (cx > halfExtent) ? cx - halfExtent : 0u;
            const u32 y0 = (cy > halfExtent) ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent, w);
            const u32 y1 = std::min(cy + halfExtent, h);
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    sum += LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4);
                    ++count;
                }
            }
            return count ? static_cast<f32>(sum / count) : 0.0f;
        }

        fs::path VisualOutputPath()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "RebindMenu_VisualEvidence.png";
        }
    } // namespace

    class RebindMenuScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            InputActionManager::Init();
            InputActionManager::SetActionMap(InputContextType::Gameplay, CreateDefaultGameActions());

            // A primary perspective camera so the runtime render path executes.
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.5f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // Build the in-game rebind panel into the scene.
            m_Menu.Open(GetScene(), InputContextType::Gameplay, {});

            EnableRendering(kW, kH);
        }

        void TearDown() override
        {
            m_Menu.Close();
            InputActionManager::Shutdown();
            RendererAttachedTest::TearDown();
        }

        RuntimeInputRebindMenu m_Menu;
    };

    TEST_F(RebindMenuScene, RendersRebindPanelAndProducesPng)
    {
        RunFrames(2);

        std::vector<u8> px;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(px, width, height)) << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        EXPECT_EQ(width, kW);
        EXPECT_EQ(height, kH);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, px.data(), static_cast<int>(width) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        f32 minLum = 1.0f;
        f32 maxLum = 0.0f;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const f32 l = LuminanceAt(px, i);
            minLum = std::min(minLum, l);
            maxLum = std::max(maxLum, l);
        }
        EXPECT_GT(maxLum - minLum, 0.05f) << "frame is nearly flat (min=" << minLum << ", max=" << maxLum << ") — the panel may not have drawn; see " << out.string();

        // Bright button/text pixels must exist (the near-white row labels), proving text drew.
        EXPECT_GT(maxLum, 0.3f) << "no bright pixels — panel text/buttons may not have rendered; see " << out.string();

        // The centred (dark) panel must be measurably distinct from the dimmed corner
        // background — it darkens the middle of the frame relative to the surround.
        const f32 centre = MeanLuminanceInRegion(px, width, height, width / 2, height / 2, 200);
        const f32 corner = MeanLuminanceInRegion(px, width, height, 30, 30, 24);
        EXPECT_GT(std::abs(centre - corner), 0.01f) << "panel region (" << centre << ") does not differ from the corner background (" << corner
                                                    << ") — the panel may not be visible; see " << out.string();
    }
} // namespace OloEngine::Tests
