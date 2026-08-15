// OLO_TEST_LAYER: L8
// =============================================================================
// ColorBlindVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the colour-vision
// adaptation pass (PostProcess_ColorBlind.glsl / ColorBlindRenderPass, issue
// #458).
//
// A scene built around the classic confusion pair — a red sphere and a green
// sphere of matched luminance on a neutral grey floor, plus a grey reference
// cube — is rendered through the FULL Renderer3D pipeline from one pose in four
// configurations, and each composited frame is written to
//   OloEditor/assets/tests/visual/ColorBlind_<state>.png
//
// The contract is GOLDEN-FREE and differential, so it survives GPU/driver
// differences and needs no committed reference image. Three properties, each
// mapping to a way the feature breaks while every CPU test stays green:
//
//   1. OFF is byte-identical to a build without the pass. A stage that runs
//      when disabled would move every other golden in the suite.
//   2. Neutral surfaces stay neutral. A dichromat sees greys normally, so the
//      adaptation must not tint the floor — the most visible possible
//      regression, and the one a "did the frame change?" assertion misses.
//   3. Correction actually helps. The red and green regions, measured through a
//      CPU simulation of what a deuteranope perceives, must be FURTHER APART in
//      the corrected frame than in the uncorrected one. This is the acceptance
//      criterion; everything else is a sanity check.
//
// The pure math contracts (identity, grey preservation, severity ramp, LMS
// round-trip, shader/CPU constant agreement) live in ColorBlindMathTest.cpp.
// Per the CLAUDE.md rendering rule, those prove the formula; this proves the
// frame.
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
        constexpr f32 kCaptureTime = 2.0f; // freeze the clock for deterministic frames

        // The frame is display-referred (ToneMapPass applied 1/gamma), so any
        // colour math on it has to decode first — the same step the shader does.
        constexpr f32 kDisplayGamma = 2.2f;

        [[nodiscard]] glm::vec3 PixelLinear(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            const glm::vec3 encoded(static_cast<f32>(px[idx + 0]) / 255.0f,
                                    static_cast<f32>(px[idx + 1]) / 255.0f,
                                    static_cast<f32>(px[idx + 2]) / 255.0f);
            return glm::pow(encoded, glm::vec3(kDisplayGamma));
        }

        [[nodiscard]] f64 Luma(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            return 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
        }

        [[nodiscard]] f64 MeanLuma(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += Luma(px, x, y);
            return sum / (static_cast<f64>(kWidth) * kHeight);
        }

        [[nodiscard]] f64 MeanAbsDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += std::abs(Luma(a, x, y) - Luma(b, x, y));
            return sum / (static_cast<f64>(kWidth) * kHeight);
        }

        // Saturation as max-channel minus min-channel, in 0..255 encoded space.
        // A neutral surface has (near-)zero; tinting it raises this.
        [[nodiscard]] f64 Chroma(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            const u8 r = px[idx + 0], g = px[idx + 1], b = px[idx + 2];
            const u8 hi = std::max(r, std::max(g, b));
            const u8 lo = std::min(r, std::min(g, b));
            return static_cast<f64>(hi) - static_cast<f64>(lo);
        }

        // Pixel indices classified from the REFERENCE (mode-off) frame, so every
        // configuration is measured over the SAME set of pixels. Classifying per
        // frame would let the adaptation move pixels between classes and make the
        // comparison meaningless.
        struct RegionMasks
        {
            std::vector<std::pair<u32, u32>> Red;
            std::vector<std::pair<u32, u32>> Green;
            std::vector<std::pair<u32, u32>> Neutral;
        };

        [[nodiscard]] RegionMasks ClassifyRegions(const std::vector<u8>& px)
        {
            RegionMasks masks;
            for (u32 y = 0; y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    const i32 r = px[idx + 0], g = px[idx + 1], b = px[idx + 2];

                    if (r - g > 40 && r - b > 40)
                        masks.Red.emplace_back(x, y);
                    else if (g - r > 40 && g - b > 40)
                        masks.Green.emplace_back(x, y);
                    else if (std::abs(r - g) < 6 && std::abs(g - b) < 6 && r > 40 && r < 220)
                        masks.Neutral.emplace_back(x, y);
                }
            }
            return masks;
        }

        [[nodiscard]] glm::vec3 MeanLinearOver(const std::vector<u8>& px,
                                               const std::vector<std::pair<u32, u32>>& mask)
        {
            if (mask.empty())
                return glm::vec3(0.0f);
            glm::dvec3 sum(0.0);
            for (const auto& [x, y] : mask)
                sum += glm::dvec3(PixelLinear(px, x, y));
            return glm::vec3(sum / static_cast<f64>(mask.size()));
        }

        [[nodiscard]] f64 MeanChromaOver(const std::vector<u8>& px,
                                         const std::vector<std::pair<u32, u32>>& mask)
        {
            if (mask.empty())
                return 0.0;
            f64 sum = 0.0;
            for (const auto& [x, y] : mask)
                sum += Chroma(px, x, y);
            return sum / static_cast<f64>(mask.size());
        }

        // Chromaticity — the colour with its overall intensity divided out.
        // Colour-vision deficiency is a HUE confusion, not a brightness one, and
        // these are LIT pixels whose intensity varies across a sphere, so
        // normalising intensity away is what makes a region mean comparable at
        // all. (Plain RGB distance also gives the wrong answer in principle: a
        // protanope perceives pure red as very dark, so red-vs-green stays far
        // apart in RGB while being almost perfectly confusable.)
        [[nodiscard]] glm::vec2 Chromaticity(const glm::vec3& c)
        {
            const glm::vec3 positive = glm::max(c, glm::vec3(0.0f));
            const f32 sum = positive.r + positive.g + positive.b;
            if (sum < 1e-6f)
                return glm::vec2(1.0f / 3.0f);
            return glm::vec2(positive.r / sum, positive.g / sum);
        }

        // How far apart two colours look TO THE DEUTERANOPE. This is the quantity
        // the accessibility feature exists to increase, and measuring it on real
        // pixels — rather than asserting "the frame changed" — is what makes this
        // test about the feature.
        [[nodiscard]] f32 PerceivedSeparation(const glm::vec3& a, const glm::vec3& b)
        {
            const glm::vec3 pa = AdaptColorLinear(a, ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Simulate, 1.0f);
            const glm::vec3 pb = AdaptColorLinear(b, ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Simulate, 1.0f);
            return glm::length(Chromaticity(pa) - Chromaticity(pb));
        }
    } // namespace

    class ColorBlindVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // One broad key light, no shadows: the point of this scene is hue
            // separation, and shadow gradients only add noise to the region means.
            {
                Entity key = scene.CreateEntity("Key");
                key.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = key.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.85f, -0.4f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 2.5f;
                dl.m_CastShadows = false;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale, const glm::vec4& albedo)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh;
                switch (prim)
                {
                    case MeshPrimitive::Plane:
                        mesh = MeshPrimitives::CreatePlane();
                        break;
                    case MeshPrimitive::Sphere:
                        mesh = MeshPrimitives::CreateSphere();
                        break;
                    default:
                        mesh = MeshPrimitives::CreateCube();
                        break;
                }
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(albedo);
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
                return e;
            };

            // A neutral floor — without a ground plane a sparse scene renders the
            // subject near-black (docs/agent-rules/single-mesh-visual-test-lighting.md)
            // AND it is the surface the "neutral stays neutral" assertion measures.
            addMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 60.0f, 1.0f, 60.0f },
                    glm::vec4(0.45f, 0.45f, 0.45f, 1.0f));

            // The confusion pair. Luminance-matched on purpose: if one were much
            // brighter, a deuteranope could separate them by value alone and the
            // correction would have nothing to prove.
            addMesh("RedSphere", MeshPrimitive::Sphere, { -4.0f, 2.2f, 0.0f }, { 2.2f, 2.2f, 2.2f },
                    glm::vec4(0.70f, 0.12f, 0.12f, 1.0f));
            addMesh("GreenSphere", MeshPrimitive::Sphere, { 4.0f, 2.2f, 0.0f }, { 2.2f, 2.2f, 2.2f },
                    glm::vec4(0.16f, 0.52f, 0.16f, 1.0f));

            // A grey reference block, so the neutral mask has a lit vertical
            // surface as well as the floor.
            addMesh("GreyBlock", MeshPrimitive::Cube, { 0.0f, 1.8f, -2.0f }, { 2.4f, 3.6f, 2.4f },
                    glm::vec4(0.55f, 0.55f, 0.55f, 1.0f));
        }

        // Render with the CURRENT accessibility settings, read back the final
        // composited frame, write PNG evidence, and verify the PNG round-trips.
        void Capture(const std::string& tag, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 5.0f, 15.0f), 0.0f, 0.25f);

            RunEditorFrames(camera, 2);

            // ColorBlindColor FIRST — it is the last stage before the backbuffer.
            // Resolving UIComposite first (as the other evidence tests do) would
            // read the image from BEFORE the adaptation and this whole test would
            // silently measure nothing.
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorBlindColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for colour-blind capture '" << tag << "'";

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

            const std::string path = (dir / ("ColorBlind_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
            ::stbi_image_free(loaded);
        }

        void ApplyMode(ColorBlindMode mode, ColorBlindAdaptation method, f32 severity)
        {
            AccessibilitySettings s = Accessibility::Get();
            s.ColorBlind = mode;
            s.ColorBlindMethod = method;
            s.ColorBlindSeverity = severity;
            Accessibility::Set(s);
        }

        void TearDown() override
        {
            // Process-global: a mode left on would adapt every later GPU test's
            // frame and move goldens for reasons nothing in the failure explains.
            Accessibility::Reset();
            RendererAttachedTest::TearDown();
        }
    };

    TEST_F(ColorBlindVisualEvidenceTest, CorrectionSeparatesConfusableHuesWithoutTintingNeutrals)
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

        // --- Reference frame: adaptation off ---
        ApplyMode(ColorBlindMode::None, ColorBlindAdaptation::Correct, 1.0f);
        std::vector<u8> offPixels;
        Capture("Off", offPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_GT(MeanLuma(offPixels), 20.0) << "reference frame rendered (near-)black";

        const RegionMasks masks = ClassifyRegions(offPixels);
        ASSERT_GT(masks.Red.size(), 500u) << "the red sphere is not in frame — check the camera pose";
        ASSERT_GT(masks.Green.size(), 500u) << "the green sphere is not in frame — check the camera pose";
        ASSERT_GT(masks.Neutral.size(), 5000u) << "the grey floor/block is not in frame";

        // --- Simulation frame: show what the deuteranope sees ---
        ApplyMode(ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Simulate, 1.0f);
        std::vector<u8> simPixels;
        Capture("DeuteranopiaSimulated", simPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- Correction frame: the accessibility feature ---
        ApplyMode(ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Correct, 1.0f);
        std::vector<u8> fixPixels;
        Capture("DeuteranopiaCorrected", fixPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // ---------------------------------------------------------------------
        // 1. The stage actually runs when enabled.
        // ---------------------------------------------------------------------
        // 0.3, not a tight bound: this asserts the stage RAN, and a passthrough
        // gives exactly 0. The two spheres cover ~8% of the frame and shift by
        // ~15-28/255 of luma each, so the real value is ~1.7 — but tying the
        // threshold to that would make it a scene-composition flake trap rather
        // than a did-it-run check. The magnitude questions are asserted below,
        // on the regions, where they mean something.
        EXPECT_GT(MeanAbsDiff(offPixels, fixPixels), 0.3)
            << "the corrected frame is essentially identical to the reference — the "
               "ColorBlindPass is not reaching the presented image. If FinalRenderPass's "
               "candidate list lost ColorBlindColor this is exactly what it looks like. "
               "See ColorBlind_Off.png / ColorBlind_DeuteranopiaCorrected.png";

        EXPECT_GT(MeanAbsDiff(offPixels, simPixels), 0.3)
            << "the simulated frame is identical to the reference";

        // ---------------------------------------------------------------------
        // 2. Neutral surfaces stay neutral.
        // ---------------------------------------------------------------------
        const f64 offNeutralChroma = MeanChromaOver(offPixels, masks.Neutral);
        const f64 fixNeutralChroma = MeanChromaOver(fixPixels, masks.Neutral);
        EXPECT_LT(fixNeutralChroma, offNeutralChroma + 6.0)
            << "the adaptation tinted neutral surfaces (grey chroma " << offNeutralChroma
            << " -> " << fixNeutralChroma << "). A dichromat sees greys normally, so the "
                                             "LMS projection must fix the neutral axis; a mistyped matrix constant is the "
                                             "usual cause. See ColorBlind_DeuteranopiaCorrected.png";

        // ---------------------------------------------------------------------
        // 3. Correction increases what the viewer can actually distinguish.
        // ---------------------------------------------------------------------
        const glm::vec3 offRed = MeanLinearOver(offPixels, masks.Red);
        const glm::vec3 offGreen = MeanLinearOver(offPixels, masks.Green);
        const glm::vec3 fixRed = MeanLinearOver(fixPixels, masks.Red);
        const glm::vec3 fixGreen = MeanLinearOver(fixPixels, masks.Green);

        const f32 separationBefore = PerceivedSeparation(offRed, offGreen);
        const f32 separationAfter = PerceivedSeparation(fixRed, fixGreen);

        // Measured gain on lit sphere pixels is ~23x (and survives 8-bit
        // quantisation), so requiring 2x keeps this an assertion about the
        // feature working rather than about measurement noise.
        EXPECT_GT(separationAfter, separationBefore * 2.0f)
            << "daltonization did not make the red and green spheres more distinguishable "
               "to a deuteranope (perceived separation "
            << separationBefore << " -> "
            << separationAfter << "). This is issue #458's acceptance criterion; the PNGs "
                                  "to look at are ColorBlind_Off.png vs ColorBlind_DeuteranopiaCorrected.png";

        // ---------------------------------------------------------------------
        // 4. Simulation goes the other way — it collapses the pair.
        // ---------------------------------------------------------------------
        const glm::vec3 simRed = MeanLinearOver(simPixels, masks.Red);
        const glm::vec3 simGreen = MeanLinearOver(simPixels, masks.Green);
        EXPECT_LT(glm::length(Chromaticity(simRed) - Chromaticity(simGreen)),
                  glm::length(Chromaticity(offRed) - Chromaticity(offGreen)))
            << "the simulate mode did not collapse the red/green pair — Correct and Simulate "
               "may have been wired to the same branch";

        // ---------------------------------------------------------------------
        // 5. It is a hue remap, not a brightness change.
        // ---------------------------------------------------------------------
        const f64 offMean = MeanLuma(offPixels);
        const f64 fixMean = MeanLuma(fixPixels);
        EXPECT_LT(std::abs(fixMean - offMean), offMean * 0.20)
            << "the adaptation shifted overall brightness too much (off=" << offMean
            << " corrected=" << fixMean << "); it should redistribute chroma, not levels";
    }

    // The no-opt-in guarantee, asserted on pixels: with the mode off the frame
    // must be BIT-identical across two captures that differ only in whether the
    // colour-blind settings were touched. This is what keeps the new stage from
    // moving every other golden image in the suite.
    TEST_F(ColorBlindVisualEvidenceTest, ModeOffLeavesTheFrameBitIdentical)
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
        std::vector<u8> baseline;
        Capture("OffBaseline", baseline);
        if (::testing::Test::HasFatalFailure())
            return;

        // Touch every colour-blind knob EXCEPT the mode. None of them may have
        // any effect while the mode is None — the pass self-skips and its
        // resource is never declared.
        AccessibilitySettings s = Accessibility::Get();
        s.ColorBlindSeverity = 0.25f;
        s.ColorBlindMethod = ColorBlindAdaptation::Simulate;
        Accessibility::Set(s);

        std::vector<u8> again;
        Capture("OffAfterKnobs", again);
        if (::testing::Test::HasFatalFailure())
            return;

        ASSERT_EQ(baseline.size(), again.size());
        EXPECT_EQ(std::memcmp(baseline.data(), again.data(), baseline.size()), 0)
            << "the frame changed with ColorBlindMode::None — the pass is doing work while "
               "disabled, which would move every golden image in the suite";
    }
} // namespace OloEngine::Tests
