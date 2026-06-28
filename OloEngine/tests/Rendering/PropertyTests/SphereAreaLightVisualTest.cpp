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
#include "RendererAttachedTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
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

    // =========================================================================
    // SphereAreaLightShadowEvidence — real-pipeline hard-shadow evidence
    //
    // The probe test above proves the SHADING math; it cannot prove shadows
    // because it has no geometry to occlude. This fixture renders a cube on a
    // floor lit by a single sphere area light through the FULL deferred
    // Renderer3D pipeline, twice from the same pose — once with the light's
    // m_CastShadows OFF and once ON — and writes both frames to
    //   OloEditor/assets/tests/visual/SphereAreaLightShadow_<state>.png
    //
    // Sphere area-light shadows reuse the point-light cubemap pool (the emitter
    // is treated as a point at its centre). The contract is GOLDEN-FREE and
    // differential, robust across GPUs: enabling shadows must DARKEN the floor in
    // the cube's lee (the side away from the light) while a control band on the
    // lit side stays essentially unchanged. With only one area light in the
    // scene Forward+ stays inactive, so the deferred main-loop shading path runs
    // (the path that applies the shadow). SKIPs (not fails) without a GL 4.6
    // context, matching the other *VisualEvidence tests.
    // =========================================================================
    class SphereAreaLightShadowEvidenceTest : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kW = 1024;
        static constexpr u32 kH = 768;

        struct Band
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        // Mean RGB over a UV rectangle (rows top-down).
        static Band SampleBand(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
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

        // Greatest luma darkening (off -> on) over a UV region scanned as a grid —
        // the shadow's exact screen position shifts with the pose, so locate the
        // strongest-darkening cell rather than hard-code a pixel band.
        static f64 MaxDarkening(const std::vector<u8>& off, const std::vector<u8>& on,
                                f32 x0, f32 x1, f32 y0, f32 y1, u32 cells = 16)
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

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kW, kH);

            // Force the classic Forward path — the editor's default for a small
            // light count, and the path verified live for this feature. Sphere
            // area-light shadows work identically on Deferred (the shadow cubemap
            // pool + sampling branch are shared), but pinning ONE deterministic
            // path keeps the differential stable across runs.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
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

            // Neutral grey floor at y=0 and a cube resting on it (unit cube spans
            // -0.5..0.5, so scale-3 centred at y=1.5 puts its base on y=0).
            addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 60.0f, 1.0f, 60.0f },
                    { 0.7f, 0.7f, 0.7f });
            addMesh("Occluder", MeshPrimitive::Cube, { 0.0f, 1.5f, 0.0f }, { 3.0f, 3.0f, 3.0f },
                    { 0.7f, 0.7f, 0.7f });

            // One sphere area light, up and to the screen-LEFT (-X) of the cube, so
            // the cube casts its shadow onto the floor on the screen-RIGHT (+X).
            // High intensity offsets the inverse-square area-light falloff so the
            // floor is clearly lit under Reinhard (the shadow then reads as a
            // strong darkening). Verified live in the editor at this geometry.
            m_Light = scene.CreateEntity("AreaLight");
            {
                auto& tc = m_Light.GetComponent<TransformComponent>();
                tc.Translation = { -5.0f, 6.0f, 0.0f };
                auto& al = m_Light.AddComponent<SphereAreaLightComponent>();
                al.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                al.m_Intensity = 450.0f;
                al.m_Radius = 0.5f;
                al.m_Range = 30.0f;
                al.m_CastShadows = false; // toggled per capture
            }
        }

        // Render from the pose, read back the composite (top-down rows), save PNG
        // evidence, and verify the PNG round-trips.
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
            ASSERT_TRUE(fb) << "No composited framebuffer for area-light-shadow capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kW, kH, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kW) * kH * 4u);

            // GL readback is bottom-up; flip so row 0 is the top of the frame.
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

            namespace fs = std::filesystem;
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);

            const std::string path = (dir / ("SphereAreaLightShadow_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kW), static_cast<int>(kH),
                                               4, outPixels.data(), static_cast<int>(kW) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kW));
            EXPECT_EQ(h, static_cast<int>(kH));
            ::stbi_image_free(loaded);
        }

        Entity m_Light;
    };

    // CastShadows off vs on: the floor in the cube's lee (screen-right, opposite
    // the light) must DARKEN when shadows are enabled, while the lit floor on the
    // light side (screen-left) stays essentially unchanged. Checked from two
    // poses. SKIPs without a GL 4.6 context.
    TEST_F(SphereAreaLightShadowEvidenceTest, CastShadowsDarkensFloorInOccluderLee)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };
        // Look steeply DOWN at the cube so the floor (and the +X lee shadow) fills
        // the frame — a grazing angle compresses the shadow off-screen. yaw 0 looks
        // down -Z so +X is screen-right (the shadow side), -X screen-left (lit).
        const std::array<Pose, 2> poses = { {
            { "TopDown", { 0.0f, 16.0f, 9.0f }, 0.0f, 1.02f },
            { "Angled", { 0.0f, 13.0f, 13.0f }, 0.0f, 0.78f },
        } };

        // Scan regions (UV). Lee = screen-right of the cube (+X, shadow side);
        // Lit = screen-left (-X, light side, control). Kept off-centre so neither
        // band straddles the cube itself, and spanning the lower-mid frame where
        // the floor (not the horizon) sits under a top-down view.
        constexpr f32 kLeeX0 = 0.58f, kLeeX1 = 0.92f;
        constexpr f32 kLitX0 = 0.08f, kLitX1 = 0.42f;
        constexpr f32 kScanY0 = 0.34f, kScanY1 = 0.86f;

        auto& light = m_Light.GetComponent<SphereAreaLightComponent>();

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);

            light.m_CastShadows = false;
            std::vector<u8> off;
            Capture(std::string("Off_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, off);
            if (::testing::Test::HasFatalFailure())
                return;

            light.m_CastShadows = true;
            std::vector<u8> on;
            Capture(std::string("On_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, on);
            if (::testing::Test::HasFatalFailure())
                return;

            // Both frames must be non-trivial (catch a black / failed render): the
            // open lit floor on the light side is a stable mid-tone in both.
            const Band offLit = SampleBand(off, kLitX0, kLitX1, kScanY0, kScanY1);
            const Band onLit = SampleBand(on, kLitX0, kLitX1, kScanY0, kScanY1);
            EXPECT_GT(offLit.Luma(), 20.0) << "shadows-off frame rendered (near-)black";
            EXPECT_GT(onLit.Luma(), 20.0) << "shadows-on frame rendered (near-)black";

            // Core contract: enabling the area light's shadow darkens the lee floor.
            const f64 leeDrop = MaxDarkening(off, on, kLeeX0, kLeeX1, kScanY0, kScanY1);
            EXPECT_GT(leeDrop, 8.0)
                << "enabling sphere-area-light shadows did not darken the lee-side floor (max drop="
                << leeDrop << "). See SphereAreaLightShadow_Off_" << pose.Name
                << ".png / SphereAreaLightShadow_On_" << pose.Name << ".png";

            // Localised, not a global dim: the lit (light-facing) floor must not
            // darken nearly as much as the lee floor.
            const f64 litDrop = MaxDarkening(off, on, kLitX0, kLitX1, kScanY0, kScanY1);
            EXPECT_LT(litDrop, leeDrop * 0.5)
                << "area-light shadow darkened the lit floor as much as the lee floor (lit drop="
                << litDrop << " lee drop=" << leeDrop << "). See SphereAreaLightShadow_On_"
                << pose.Name << ".png";
        }
    }
} // namespace OloEngine::Tests
