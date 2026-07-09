// OLO_TEST_LAYER: L8
// =============================================================================
// Visual evidence for camera-relative rendering in the 2D sprite path
// (issue #429, 2D-sprites slice).
//
// Renderer2D bakes world vertices on the CPU (transform * QuadVertexPositions)
// at Draw* time, so far from the origin it loses fine detail at *bake* time —
// a different failure path from the 3D GPU-upload slice, and the reason 2D
// sprites were called out as a separate follow-up. The fix gives Renderer2D its
// own grid-snapped render origin: bake every vertex relative to it and upload a
// relative view-projection. No shader change (a pure coordinate shift).
//
// This test drives Renderer2D DIRECTLY into a framebuffer (the same renderer the
// editor's 2D overlay uses) and reads the pixels back, three ways:
//   * near_ref : a fine checkerboard of small sprites built at the world origin
//                — the ground-truth appearance (the feature is a no-op in the
//                first grid cell, so this is what the frame should look like).
//   * far_on   : the identical sprite grid far from origin, feature ON (the fix).
//   * far_off  : the same far grid with the feature forced OFF (the pre-slice
//                world-space bake — the "before").
// far_on renders the identical local geometry near 0, so it must match near_ref
// closely; far_off bakes tiny sprite corners in f32 at a large coordinate, where
// the ULP swamps the sub-unit corner offsets, so the checkerboard smears/merges
// and it diverges from the truth. The test asserts far_on is much closer to
// near_ref than far_off is, and writes all three PNGs to assets/tests/visual/.
//
// SKIPs cleanly (does not fail) when no GL 4.6 context exists — same gate as the
// other RendererAttachedTest visual-evidence tests.
//
// Classification: L8 / integration (real Renderer2D draw path + RGBA8 readback
// + PNG).
// =============================================================================
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/OrthographicCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer2D.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 512;

        // Half-extent (in world units) of the orthographic view — the sprite
        // grid spans most of it. Small so a large world offset lands the grid
        // in the deep-precision-loss regime while the on-screen view is normal.
        constexpr f32 kViewHalfExtent = 2.0f;

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        // Fraction of pixels distinctly brighter than the dark clear colour — a
        // "something actually drew" guard so a blank frame can't pass.
        [[nodiscard]] f64 NonClearFraction(const std::vector<u8>& px)
        {
            std::size_t nonClear = 0, total = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                if (px[i] > 40 || px[i + 1] > 40 || px[i + 2] > 40)
                    ++nonClear;
                ++total;
            }
            return total ? static_cast<f64>(nonClear) / static_cast<f64>(total) : 0.0;
        }
    } // namespace

    class Renderer2DCameraRelativeVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // We drive Renderer2D directly (not the scene render graph), so the
        // fixture only needs the renderer + GL context brought up — no scene
        // build, no EnableRendering.
        void BuildScene() override {}

        // Render a fine checkerboard of small sprites centred on `center`,
        // plus a ring and a framing rect, through the REAL Renderer2D path into
        // an offscreen framebuffer; read the pixels back top-row-first and save
        // assets/tests/visual/Renderer2DCameraRelative_<tag>.png.
        void Capture(const std::string& tag, const glm::vec3& center, std::vector<u8>& outPixels)
        {
            FramebufferSpecification spec;
            spec.Width = kSize;
            spec.Height = kSize;
            // Match the editor scene framebuffer (RGBA8 colour + entity-id +
            // depth). The 2D shaders also write o_ViewNormal at location 2;
            // GL discards writes to an absent attachment, exactly as in the
            // editor which uses this same two-colour-attachment layout.
            spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth };
            Ref<Framebuffer> fb = Framebuffer::Create(spec);

            // Renderer2D's line width is engine-side static state, NOT raw GL
            // state, so the GLStateGuard below does not restore it — save and
            // restore it by hand so the temporary 1.0f (set for the DrawRect,
            // see below) cannot leak into later tests as the wrong default.
            const f32 savedLineWidth = Renderer2D::GetLineWidth();

            {
                // Contain all GL state this render touches so it cannot leak
                // into the next GPU test in the shared process-wide context.
                GLStateGuard guard("Renderer2DCameraRelative", GLStateGuard::Policy::Restore);

                fb->Bind();
                fb->ClearAllAttachments(glm::vec4(0.05f, 0.05f, 0.08f, 1.0f), -1);

                // 2D overlay state: no depth test, straight alpha blending.
                RenderCommand::SetDepthTest(false);
                RenderCommand::EnableBlending();
                RenderCommand::DisableCulling();

                // Core-profile GL only guarantees a 1.0 line width; Renderer2D's
                // default 2.0 makes glLineWidth raise GL_INVALID_VALUE (which the
                // suite's GL-error listener treats as context pollution). The
                // DrawRect below only needs to exercise the bake path, so pin 1.0.
                Renderer2D::SetLineWidth(1.0f);

                OrthographicCamera camera(-kViewHalfExtent, kViewHalfExtent, -kViewHalfExtent, kViewHalfExtent);
                camera.SetPosition(center);

                Renderer2D::BeginScene(camera);

                // A fine checkerboard of small sprites. The sprite CENTRES are
                // placed at exactly-f32-representable coordinates (`center` is a
                // power of two and `kStep` a multiple of the far ULP, 0.03125 at
                // 2^18), so the sprite *positions* carry NO f32 quantization at
                // either distance — the stored-position ULP floor is not what
                // camera-relative fixes (see CameraRelativeTest). What DOES vary
                // is each sprite's internal geometry: OFF bakes corner offsets
                // (transform * QuadVertexPositions) in f32 at ~262 k, where the
                // 0.03125 ULP swamps the ~0.03-unit corners so the little squares
                // distort/collapse; ON bakes them near 0, crisp. Two alternating
                // colours make the smear obvious.
                constexpr int kGrid = 12;          // -12..12 -> 25x25 sprites
                constexpr f32 kStep = 0.25f;       // 8 * far-ULP (exact) spacing
                constexpr f32 kSpriteSize = 0.06f; // < step, so gaps stay visible
                for (int gy = -kGrid; gy <= kGrid; ++gy)
                {
                    for (int gx = -kGrid; gx <= kGrid; ++gx)
                    {
                        const bool even = ((gx + gy) & 1) == 0;
                        const glm::vec4 color = even ? glm::vec4(0.95f, 0.75f, 0.15f, 1.0f)
                                                     : glm::vec4(0.15f, 0.55f, 0.95f, 1.0f);
                        const glm::vec3 pos = center + glm::vec3(static_cast<f32>(gx) * kStep,
                                                                 static_cast<f32>(gy) * kStep, 0.0f);
                        Renderer2D::DrawQuad(pos, glm::vec2(kSpriteSize), color);
                    }
                }

                // Exercise the circle + rect + line bake paths too.
                Renderer2D::DrawCircle(glm::translate(glm::mat4(1.0f), center) *
                                           glm::scale(glm::mat4(1.0f), glm::vec3(1.2f)),
                                       glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), 0.08f, 0.005f, -1);
                Renderer2D::DrawRect(glm::translate(glm::mat4(1.0f), center) *
                                         glm::scale(glm::mat4(1.0f), glm::vec3(3.4f)),
                                     glm::vec4(1.0f, 0.2f, 0.2f, 1.0f), -1);

                Renderer2D::EndScene();

                fb->Unbind();
            }

            // Restore the line width before any early-returning assertion below
            // so the temporary 1.0f never escapes this Capture call.
            Renderer2D::SetLineWidth(savedLineWidth);

            // Read AFTER the framebuffer is unbound (the GLStateGuard also
            // restored the draw/read FBO to 0): NVIDIA raises GL_INVALID_VALUE
            // ("not valid from a preview context") when glGetTextureImage samples
            // a texture still attached to the currently-bound FBO. Matches the
            // sibling CameraRelativeVisualEvidenceTest, which reads post-render.
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kSize, kSize, outPixels);

            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kSize) * kSize * 4u);

            // GL readback is bottom-up; flip to top-row-first for the PNG.
            const std::size_t rowBytes = static_cast<std::size_t>(kSize) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kSize / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kSize - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("Renderer2DCameraRelative_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kSize), static_cast<int>(kSize),
                                               4, outPixels.data(), static_cast<int>(kSize) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
            OLO_CORE_INFO("Renderer2DCameraRelativeVisualEvidence: wrote {} (abs: {})", path,
                          fs::absolute(path).string());
        }
    };

    // A far-from-origin 2D sprite grid must render the same as at the origin
    // with the feature ON, and visibly worse with it OFF. SKIPs without a GPU.
    TEST_F(Renderer2DCameraRelativeVisualEvidenceTest, FarOriginMatchesNearOriginWithFeatureOnNotOff)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The 2D bake loses detail at *bake* time, so a static frame degrades
        // only where the f32 ULP rivals the sprite's sub-unit corner offsets.
        // The spec target is tens of km; the CPU contract test pins the 45 km
        // fine-detail case exactly. Here we push further (2^18 ≈ 260 km) so the
        // static-frame smear is unmistakable in the OFF image while the ON image
        // stays pixel-faithful to the origin (its relative coords are < 512).
        // A power-of-two centre keeps the exactly-representable sprite grid
        // (see Capture) truly exact, isolating bake precision from position ULP.
        const glm::vec3 farCenter(262144.0f, -262144.0f, 0.0f);

        // Ground truth at the origin (feature is a no-op there).
        Renderer2D::SetCameraRelativeEnabled(true);
        std::vector<u8> nearRef;
        Capture("near_ref", glm::vec3(0.0f), nearRef);

        // Same grid far away, feature ON — should reproduce the origin frame.
        Renderer2D::SetCameraRelativeEnabled(true);
        std::vector<u8> farOn;
        Capture("far_on", farCenter, farOn);

        // Feature OFF — the pre-slice world-space bake; large-coordinate smear.
        Renderer2D::SetCameraRelativeEnabled(false);
        std::vector<u8> farOff;
        Capture("far_off", farCenter, farOff);

        Renderer2D::SetCameraRelativeEnabled(true); // restore for later tests

        // Sanity: the reference and the on-frame actually drew the grid. The
        // sparse small-sprite checkerboard (with gaps + dark background) covers
        // ~8% of the frame, so the guard only needs to exclude a truly blank
        // frame, not assert dense coverage.
        EXPECT_GT(NonClearFraction(nearRef), 0.04) << "near-origin reference frame looks blank";
        EXPECT_GT(NonClearFraction(farOn), 0.04) << "far-origin (feature on) frame looks blank";

        const f64 rmseOnVsRef = Rgba8Rmse(farOn, nearRef);
        const f64 rmseOffVsRef = Rgba8Rmse(farOff, nearRef);

        OLO_CORE_INFO("Renderer2DCameraRelativeVisualEvidence: RMSE far_on-vs-ref={:.3f}, far_off-vs-ref={:.3f}",
                      rmseOnVsRef, rmseOffVsRef);

        // With the feature ON the far frame reproduces the origin frame almost
        // exactly (identical relative geometry + a relative VP that recovers the
        // origin projection), so this is a tight bound, not a loose one.
        EXPECT_LT(rmseOnVsRef, 3.0)
            << "camera-relative 2D rendering should reproduce the near-origin appearance far from origin (RMSE "
            << rmseOnVsRef << ")";
        // With it OFF the world-space bake visibly degrades the sprite geometry,
        // so it is markedly further from the truth — both by an absolute margin
        // (a near-zero ON would make a pure ratio test vacuous) and relative to
        // the ON frame.
        EXPECT_GT(rmseOffVsRef, 5.0)
            << "feature-off far frame should visibly degrade vs the near-origin truth (off="
            << rmseOffVsRef << ")";
        EXPECT_GT(rmseOffVsRef, rmseOnVsRef + 3.0)
            << "feature-off far frame should be markedly further from truth than feature-on (on="
            << rmseOnVsRef << ", off=" << rmseOffVsRef << ")";
    }
} // namespace OloEngine::Tests
