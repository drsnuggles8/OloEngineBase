// OLO_TEST_LAYER: L8
// =============================================================================
// UITextScaleVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the global UI
// text scale (issue #458, acceptance criterion: "a global text-scale setting
// visibly scales all UI text").
//
// A screen-space UI canvas carrying one UITextComponent is rendered through the
// FULL Renderer3D pipeline at three accessibility settings, and each composited
// frame is written to
//   OloEditor/assets/tests/visual/UITextScale_<state>.png
//
// WHY THIS EXISTS RATHER THAN A MANUAL SCREENSHOT
//
// The policy is unit-tested (ResolveUIFontSize) and a source scan proves all
// four UIRenderer draw sites route through it — but neither executes a single
// glyph. "Visibly scales" is a claim about PIXELS, and the only honest way to
// make it is to count them. Measuring ink coverage also makes the criterion a
// permanent CI gate instead of a one-off look at a running editor.
//
// The contract is GOLDEN-FREE and differential: the measure is the number of
// pixels the glyphs actually cover (text INK against a flat backdrop), so it
// needs no reference image and survives GPU/driver/font-raster differences.
//
//   1. Scale 1.0 must draw a non-trivial amount of ink — otherwise every
//      later comparison is between two empty frames and passes vacuously.
//   2. Scale 2.0 must draw MATERIALLY more ink. Glyph area grows with the
//      square of the linear scale, so 2x linear is ~4x area; requiring only
//      2x keeps this about the feature, not about font rasterisation.
//   3. Default settings must be pixel-identical to scale 1.0 — the
//      "byte-identical until opt-in" invariant, asserted on a real frame.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching the other *VisualEvidenceTest fixtures.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
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
        constexpr f32 kCaptureTime = 2.0f;

        // The authored size. Deliberately well above any legibility floor so
        // this test measures the SCALE and nothing else.
        constexpr f32 kAuthoredFontSize = 28.0f;

        // Count NEAR-WHITE pixels — i.e. glyph ink.
        //
        // The threshold is 200, and it is not arbitrary. The editor render path
        // draws a reference GRID whose lines measure ~150-180 grey, and there
        // are tens of thousands of them: at a threshold of 140 the grid
        // contributes ~56,000 px and completely swamps the few hundred px of
        // glyph, so the 1x and 2x frames differ by well under 1% and the test
        // reports "no change" while the text has visibly doubled. That is
        // exactly what the first version of this test did.
        //
        // The label is pure white (255), so 200 separates ink from grid
        // cleanly. Measured on this scene: 1x = 279 px, 2x = 1452 px.
        [[nodiscard]] u32 InkPixels(const std::vector<u8>& px, u8 threshold = 200u)
        {
            u32 count = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                const u8 r = px[i], g = px[i + 1], b = px[i + 2];
                if (r > threshold && g > threshold && b > threshold)
                    ++count;
            }
            return count;
        }
    } // namespace

    class UITextScaleVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // A screen-space overlay canvas covering the viewport.
            Entity canvas = scene.CreateEntity("ScaleCanvas");
            {
                auto& c = canvas.AddComponent<UICanvasComponent>();
                c.m_RenderMode = UICanvasRenderMode::ScreenSpaceOverlay;
                c.m_SortOrder = 10;

                auto& rect = canvas.AddComponent<UIRectTransformComponent>();
                rect.m_AnchorMin = { 0.0f, 0.0f };
                rect.m_AnchorMax = { 1.0f, 1.0f };
            }

            // One text element, generously sized so a 2x scale still fits inside
            // the viewport — the measure is ink COUNT, and glyphs clipped by the
            // viewport edge would undercount the scaled frame and mask a
            // regression rather than expose one.
            Entity label = scene.CreateEntity("ScaleLabel");
            {
                auto& rect = label.AddComponent<UIRectTransformComponent>();
                rect.m_AnchorMin = { 0.05f, 0.30f };
                rect.m_AnchorMax = { 0.95f, 0.70f };

                auto& text = label.AddComponent<UITextComponent>();
                text.m_Text = "Accessibility";
                text.m_FontSize = kAuthoredFontSize;
                text.m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
                text.m_Alignment = UITextAlignment::MiddleCenter;

                auto& rel = label.AddComponent<RelationshipComponent>();
                rel.m_ParentHandle = canvas.GetUUID();
                if (!canvas.HasComponent<RelationshipComponent>())
                    canvas.AddComponent<RelationshipComponent>();
                canvas.GetComponent<RelationshipComponent>().m_Children.push_back(label.GetUUID());

                m_LabelEntity = label.GetUUID();
            }
        }

        void Capture(const std::string& tag, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 2.0f, 8.0f), 0.0f, 0.0f);

            RunEditorFrames(camera, 3);

            // ColorBlindColor first for the same reason FinalRenderPass reads it
            // first — it is the last stage before present when a mode is active
            // (notes-renderer.md §17). It is None here, so UIComposite is what
            // resolves; listing it keeps this fixture correct if that changes.
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorBlindColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            ASSERT_TRUE(fb) << "No UI-composited framebuffer for capture '" << tag
                            << "' — the UI overlay did not reach the render graph";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

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

            const std::string path = (dir / ("UITextScale_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, outPixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        void ApplyScale(f32 scale)
        {
            // Read-modify-write, not default-construct: overwriting the whole
            // global would silently reset every other preference, so a later
            // case that sets one first would measure the wrong configuration.
            AccessibilitySettings s = Accessibility::Get();
            s.UITextScale = scale;
            Accessibility::Set(s);
        }

        void TearDown() override
        {
            // Process-global: a scale left set would enlarge every later GPU
            // test's UI text and move goldens for a reason nothing explains.
            Accessibility::Reset();
            RendererAttachedTest::TearDown();
        }

        UUID m_LabelEntity = 0;
    };

    TEST_F(UITextScaleVisualEvidenceTest, RaisingTheScaleVisiblyEnlargesRenderedGlyphs)
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

        Accessibility::Reset();
        std::vector<u8> defaultPixels;
        Capture("Default", defaultPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        ApplyScale(1.0f);
        std::vector<u8> scale1;
        Capture("Scale1x", scale1);
        if (::testing::Test::HasFatalFailure())
            return;

        ApplyScale(2.0f);
        std::vector<u8> scale2;
        Capture("Scale2x", scale2);
        if (::testing::Test::HasFatalFailure())
            return;

        const u32 inkDefault = InkPixels(defaultPixels);
        const u32 ink1 = InkPixels(scale1);
        const u32 ink2 = InkPixels(scale2);

        // 1. There is text on screen at all. Without this the rest is vacuous:
        //    two empty frames satisfy "identical" and would only fail the ratio.
        EXPECT_GT(ink1, 100u)
            << "no glyph ink at scale 1.0 (" << ink1 << " px) — the UI text never rendered, so "
                                                        "nothing below this line means anything. See UITextScale_Scale1x.png";

        // 2. THE acceptance criterion, measured in pixels.
        //
        // Glyph AREA grows with the square of the linear scale, so 2x linear is
        // ~4x area; the measured ratio is higher still (~5.2x) because a 1x
        // stroke is thin enough that antialiasing keeps most of its pixels
        // below full white, while a 2x stroke saturates. Requiring only 2x
        // keeps this an assertion about the feature rather than about the font
        // rasteriser.
        EXPECT_GT(ink2, static_cast<u32>(static_cast<f64>(ink1) * 2.0))
            << "doubling UITextScale did not visibly enlarge the glyphs (ink " << ink1 << " -> " << ink2
            << " px). Compare UITextScale_Scale1x.png and UITextScale_Scale2x.png — if the glyphs look "
               "the same size the feature regressed; if they look bigger, this metric is picking up "
               "background (the editor grid) instead of ink.";

        // 3. The no-opt-in invariant, on a real frame rather than in the policy
        //    function: default settings must render exactly as scale 1.0.
        ASSERT_EQ(defaultPixels.size(), scale1.size());
        EXPECT_EQ(std::memcmp(defaultPixels.data(), scale1.data(), defaultPixels.size()), 0)
            << "default accessibility settings did not render identically to an explicit 1.0 scale "
               "(ink "
            << inkDefault << " vs " << ink1 << " px) — something in the default path is "
                                               "already resizing text";
    }
} // namespace OloEngine::Tests
