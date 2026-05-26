// =============================================================================
// SphereAreaLightVisualTest.cpp
//
// Visual evidence (PNG) for issue #231 sphere area lights:
//   - Compiles the production SphereAreaLightVisualProbe.glsl shader.
//   - Renders 8 cells (4 roughness columns x 2 light-type rows) into an
//     RGBA8 framebuffer via the standard PropertyTests harness.
//   - Writes the readback frame to
//       OloEditor/assets/tests/visual/SphereAreaLight_VisualEvidence.png
//     (the working directory is OloEditor/; output rooted under
//     `assets/tests/visual/` next to the golden-image baselines).
//   - Asserts two contracts the visual must satisfy regardless of the
//     specific GPU driver:
//
//       1. Both rows produced non-trivial output (mean luminance above a
//          floor). Catches the "shader silently outputs black" regression.
//       2. The top row (sphere area light, radius=1) spreads luminance
//          across MORE bright pixels than the bottom row (point light).
//          That's the visible Karis representative-point softening
//          contract — if a future shader edit collapses the area light
//          to a point response, the highlight-area ratio drops below 1.0
//          and this test fails.
//
// The PNG is always written, whether the assertions pass or fail, so a
// reviewer always has the frame to look at. Asset-relative paths assume
// the test binary runs from `OloEditor/` (the project convention; the
// VS Code task `run-tests-debug` runs the binary from repo root, where
// the `assets/` lookup falls back gracefully and the PNG ends up under
// `<cwd>/assets/tests/visual/`).
//
// Classification: L8 / integration (full GL pipeline, RGBA8 readback +
// PNG output — matches the GoldenImageTests layer but is keyed to a
// specific feature rather than a moving baseline).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <stb_image/stb_image_write.h>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Matches the layout constants in SphereAreaLightVisualProbe.glsl.
        // If the shader's grid changes, update both — the assertions below
        // index by these values to count pixels-per-cell.
        constexpr u32 kColumns = 4;
        constexpr u32 kCellSize = 256;
        constexpr u32 kWidth = kColumns * kCellSize; // 1024
        constexpr u32 kHeight = 2 * kCellSize;       // 512

        struct VisualHarness
        {
            Ref<Framebuffer> m_Fb;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            VisualHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                spec.Attachments = { FramebufferTextureFormat::RGBA8 };
                m_Fb = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/SphereAreaLightVisualProbe.glsl");
            }

            void Draw()
            {
                // Policy::Restore auto-rolls-back viewport / blend / depth /
                // cull / FBO / active program / VAO on scope exit so the
                // probe shader bind + fullscreen-tri VAO + viewport change
                // don't leak into whichever test runs next. Without this the
                // probe would dump 5+ ERROR lines through OloEngine.log on
                // every test invocation (see GLStateGuard::Policy::Log).
                GLStateGuard guard("SphereAreaLightVisualHarness::Draw", GLStateGuard::Policy::Restore);
                m_Fb->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_Fb->Unbind();
            }

            void Readback(std::vector<u8>& out) const
            {
                ReadbackRgba8(m_Fb->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            }
        };

        // Luminance (Rec. 709) of a pixel at index `idx` in an RGBA8 buffer.
        // Used for both the "non-trivial output" assertion and the "highlight
        // area" assertion below.
        f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        // Peak luminance inside a (col,row) cell. Used to derive an
        // adaptive highlight threshold so the test is robust against
        // tone-map / driver variation.
        f32 PeakLuminanceInCell(const std::vector<u8>& px, u32 col, u32 row)
        {
            f32 peak = 0.0f;
            const u32 xStart = col * kCellSize;
            const u32 yStart = row * kCellSize;
            for (u32 y = yStart; y < yStart + kCellSize; ++y)
            {
                for (u32 x = xStart; x < xStart + kCellSize; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                    peak = std::max(peak, LuminanceAt(px, idx));
                }
            }
            return peak;
        }

        // Counts pixels inside a cell whose luminance exceeds `threshold`.
        // Combined with PeakLuminanceInCell this gives a tone-map-stable
        // "highlight spread" metric: count pixels above (peak * fraction).
        // Sphere area light spreads the highlight across more area than a
        // point light, so the count above the same fraction-of-peak must be
        // strictly larger for the area-light row.
        u32 CountBrightPixelsInColumn(const std::vector<u8>& px, u32 col, u32 row,
                                      f32 threshold)
        {
            u32 count = 0;
            const u32 xStart = col * kCellSize;
            const u32 yStart = row * kCellSize;
            for (u32 y = yStart; y < yStart + kCellSize; ++y)
            {
                for (u32 x = xStart; x < xStart + kCellSize; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                    if (LuminanceAt(px, idx) > threshold)
                        ++count;
                }
            }
            return count;
        }

        f32 MeanLuminanceInRow(const std::vector<u8>& px, u32 row)
        {
            f64 sum = 0.0;
            const u32 yStart = row * kCellSize;
            const u32 yEnd = yStart + kCellSize;
            for (u32 y = yStart; y < yEnd; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                    sum += LuminanceAt(px, idx);
                }
            }
            return static_cast<f32>(sum / static_cast<f64>(kWidth * kCellSize));
        }

        fs::path VisualOutputPath()
        {
            // Mirrors GoldenBaselineDir() layout but under `visual/` so the
            // output is not confused with regression-tracked goldens.
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "SphereAreaLight_VisualEvidence.png";
        }
    } // namespace

    // ========================================================================
    // SphereAreaLightVisual.RendersAndProducesPng
    //
    // Drives the visual probe and writes the PNG. Asserts the highlight
    // spread contract — area light highlight reaches more pixels than the
    // point light reference at low roughness. The exact pixel count is
    // GPU-dependent, but the inequality is a hard property: the Karis
    // representative-point construction integrates radiance over the
    // sphere's projected solid angle, which is strictly larger than the
    // point-light case.
    // ========================================================================
    TEST(SphereAreaLightVisual, RendersAndProducesPng)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        VisualHarness harness;
        ASSERT_TRUE(harness.m_Fb != nullptr) << "framebuffer creation failed";
        ASSERT_TRUE(harness.m_Shader != nullptr) << "shader compile failed";

        harness.Draw();

        std::vector<u8> px;
        harness.Readback(px);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

        // Always write the PNG first, then assert — keeps the artifact
        // available even if a later assertion fails.
        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(kWidth), static_cast<int>(kHeight),
                                           4, px.data(), static_cast<int>(kWidth) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        // Non-trivial output contract. Both rows must show real shading,
        // not just the background tint (~0.04 luminance).
        const f32 meanRow0 = MeanLuminanceInRow(px, 0);
        const f32 meanRow1 = MeanLuminanceInRow(px, 1);
        EXPECT_GT(meanRow0, 0.05f) << "top row (area light) is suspiciously dark";
        EXPECT_GT(meanRow1, 0.05f) << "bottom row (point light) is suspiciously dark";

        // Use column 1 (roughness=0.25) for highlight analysis: at
        // roughness=0.05 the specular lobe collapses to sub-pixel size
        // and the peak measurement just samples the diffuse shoulder,
        // giving identical values between rows. r=0.25 is the smallest
        // probe roughness at which the highlight registers as a multi-
        // pixel blob the eye can resolve in the PNG.
        constexpr u32 kHighlightCol = 1;
        const f32 peakArea = PeakLuminanceInCell(px, kHighlightCol, 0 /*row*/);
        const f32 peakPoint = PeakLuminanceInCell(px, kHighlightCol, 1 /*row*/);

        // Both rows must register a visible highlight — peak luminance
        // comfortably above the diffuse shoulder (~0.15 in this scene).
        EXPECT_GT(peakArea, 0.25f)
            << "area-light column has no visible highlight — radius wiring may have regressed";
        EXPECT_GT(peakPoint, 0.25f)
            << "point-light reference column has no visible highlight — control regressed";

        // Karis representative-point + normalisation contract, the
        // visually-defining property of sphere area lights:
        //
        //   The energy-conservation rescale multiplies the GGX D term by
        //   (alpha / alpha_prime)^2 < 1, *spreading* the specular lobe
        //   across more solid angle. The result is a softer, *wider*
        //   highlight whose *peak* is LOWER than a point light's at the
        //   same intensity. The total integrated radiance is preserved
        //   (within the Karis approximation) — what shifts is the SHAPE.
        //
        //   So at the same colour/intensity/range, the point-light peak
        //   must be strictly brighter than the area-light peak; the
        //   8-bit + Reinhard pipeline keeps this delta well above
        //   quantisation noise (typically 30-50%).
        EXPECT_GT(peakPoint, peakArea * 1.05f)
            << "point-light peak is not measurably brighter than area-light peak — "
            << "Karis normalisation factor may have stopped reducing D "
            << "(peakArea=" << peakArea << ", peakPoint=" << peakPoint
            << "); written PNG: " << out.string();

        // Belt-and-braces: also check the spread. With the threshold set
        // to 80% of the AREA-LIGHT peak, the area light should land more
        // pixels above it than the point light does. The area light's
        // peak is lower but its shoulder is wider — at peakArea*0.8
        // we're looking at the inner half of the soft highlight, where
        // the point light's narrower lobe registers fewer pixels.
        const f32 areaSpreadThreshold = peakArea * 0.8f;
        const u32 spreadArea = CountBrightPixelsInColumn(px, kHighlightCol, 0, areaSpreadThreshold);
        const u32 spreadPoint = CountBrightPixelsInColumn(px, kHighlightCol, 1, areaSpreadThreshold);
        EXPECT_GT(spreadArea, spreadPoint)
            << "area-light highlight is not broader than point-light reference — "
            << "Karis representative-point spread has collapsed (areaPx=" << spreadArea
            << " above peakArea*0.8=" << areaSpreadThreshold
            << ", pointPx=" << spreadPoint << "); written PNG: " << out.string();
    }
} // namespace OloEngine::Tests
