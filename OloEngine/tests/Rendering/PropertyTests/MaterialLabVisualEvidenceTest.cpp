// OLO_TEST_LAYER: L8
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

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Matches the layout constants in MaterialLabProbe.glsl. If the
        // shader's grid changes, update both — the cell samplers below index
        // by these values.
        constexpr u32 kGridSize = 7;
        constexpr u32 kImageSize = 1024;

        // Cell edge in pixels. 1024/7 is deliberately non-integral — all cell
        // geometry below is done in f64 and never assumes integer cells.
        constexpr f64 kCellSizePx = static_cast<f64>(kImageSize) / static_cast<f64>(kGridSize);

        // Sphere disk radius in cell-local NDC ([-1,1] per cell) — must match
        // kDiskRadius in the shader. In pixels that is
        // kDiskRadiusNdc * kCellSizePx / 2 (the cell NDC range [-1,1] spans
        // one cell edge).
        constexpr f64 kDiskRadiusNdc = 0.85;

        // Cell samplers only look at the inner 60% of the disk RADIUS: the
        // outer 40% ring is excluded so no sampled pixel can straddle the
        // disk edge and pick up backdrop or the near-zero-NdotV limb, which
        // would dilute every differential below with constant pixels.
        constexpr f64 kInnerSampleFraction = 0.6;

        // Params UBO slot — must match MaterialLabProbe.glsl. Binding 18 is
        // the established test-probe params slot (ShaderUnit_ShadowSelfShadow
        // / ShaderUnit_ShadowVisualize's ShadowProbeUBO). Test shaders are
        // outside the production binding scan, and the production occupant of
        // 18 (UBO_PRECIPITATION) never runs during a probe draw.
        constexpr u32 kProbeUboBinding = 18;

        // Grey levels the task's contracts are expressed in (0..255 luma).
        constexpr f64 kMultiAngleFloor = 2.0; // contract (d)

        struct MaterialLabParamsUbo
        {
            f32 ViewDir[4]; // xyz = view direction, w selects the single-scatter control
        };
        static_assert(sizeof(MaterialLabParamsUbo) == 16, "std140 layout mismatch");

        struct VisualHarness
        {
            Ref<Framebuffer> m_Fb;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;
            GLuint m_Ubo = 0;

            VisualHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kImageSize;
                spec.Height = kImageSize;
                spec.Attachments = { FramebufferTextureFormat::RGBA8 };
                m_Fb = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/MaterialLabProbe.glsl");
                ::glCreateBuffers(1, &m_Ubo);
                ::glNamedBufferData(m_Ubo, sizeof(MaterialLabParamsUbo), nullptr,
                                    GL_DYNAMIC_DRAW);
            }

            ~VisualHarness()
            {
                if (m_Ubo != 0)
                    ::glDeleteBuffers(1, &m_Ubo);
            }

            VisualHarness(const VisualHarness&) = delete;
            VisualHarness& operator=(const VisualHarness&) = delete;

            void Draw(const glm::vec3& viewDir, bool omitMultiScatter = false)
            {
                MaterialLabParamsUbo data{};
                const glm::vec3 v = glm::normalize(viewDir);
                data.ViewDir[0] = v.x;
                data.ViewDir[1] = v.y;
                data.ViewDir[2] = v.z;
                data.ViewDir[3] = omitMultiScatter ? 1.0f : 0.0f;
                ::glNamedBufferSubData(m_Ubo, 0, sizeof(data), &data);

                // Policy::Restore rolls back viewport / blend / depth / cull /
                // FBO / program / VAO on scope exit so the probe draw cannot
                // leak GL state into whichever test runs next.
                GLStateGuard guard("MaterialLabVisualHarness::Draw",
                                   GLStateGuard::Policy::Restore);
                m_Fb->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(kImageSize),
                             static_cast<GLsizei>(kImageSize));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                ::glBindBufferBase(GL_UNIFORM_BUFFER, kProbeUboBinding, m_Ubo);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_Fb->Unbind();
            }

            void Readback(std::vector<u8>& out) const
            {
                ReadbackRgba8(m_Fb->GetColorAttachmentHandle(0), kImageSize, kImageSize,
                              out);
            }
        };

        // Rec. 709 luma of an RGBA8 pixel, in GREY LEVELS (0..255) — the unit
        // every threshold in this file is expressed in.
        f32 LumaAt(const std::vector<u8>& px, std::size_t idx)
        {
            return 0.2126f * static_cast<f32>(px[idx + 0]) +
                   0.7152f * static_cast<f32>(px[idx + 1]) +
                   0.0722f * static_cast<f32>(px[idx + 2]);
        }

        // Visits every pixel whose CENTRE lies inside the inner
        // kInnerSampleFraction of grid cell (col,row)'s sphere disk.
        //
        // Geometry: cell (col,row) spans pixels [col*cell,(col+1)*cell) x
        // [row*cell,(row+1)*cell) (buffer rows in GL order — row 0 is
        // metallic 0, see the header note). The disk is centred on the cell
        // centre with radius kDiskRadiusNdc * cell/2; sampling stops at 60%
        // of that radius so backdrop / disk-edge pixels are never included.
        // Pixel centres sit at (x+0.5, y+0.5), matching the interpolated
        // v_TexCoord the fragment shader saw.
        template<typename Fn>
        void ForEachInnerDiskPixel(u32 col, u32 row, Fn&& fn)
        {
            const f64 cx = (static_cast<f64>(col) + 0.5) * kCellSizePx;
            const f64 cy = (static_cast<f64>(row) + 0.5) * kCellSizePx;
            const f64 radiusPx = kDiskRadiusNdc * kCellSizePx * 0.5 * kInnerSampleFraction;
            const f64 radiusSq = radiusPx * radiusPx;

            const auto yBegin = static_cast<u32>(std::max(0.0, std::floor(cy - radiusPx)));
            const auto yEnd = static_cast<u32>(
                std::min<f64>(kImageSize, std::ceil(cy + radiusPx) + 1.0));
            const auto xBegin = static_cast<u32>(std::max(0.0, std::floor(cx - radiusPx)));
            const auto xEnd = static_cast<u32>(
                std::min<f64>(kImageSize, std::ceil(cx + radiusPx) + 1.0));

            for (u32 y = yBegin; y < yEnd; ++y)
            {
                for (u32 x = xBegin; x < xEnd; ++x)
                {
                    const f64 dx = (static_cast<f64>(x) + 0.5) - cx;
                    const f64 dy = (static_cast<f64>(y) + 0.5) - cy;
                    if (dx * dx + dy * dy <= radiusSq)
                        fn((static_cast<std::size_t>(y) * kImageSize + x) * 4u);
                }
            }
        }

        // Mean luma (grey levels) over cell (col,row)'s inner disk.
        f64 DiskMeanLuma(const std::vector<u8>& px, u32 col, u32 row)
        {
            f64 sum = 0.0;
            u64 count = 0;
            ForEachInnerDiskPixel(col, row,
                                  [&](std::size_t idx)
                                  {
                                      sum += static_cast<f64>(LumaAt(px, idx));
                                      ++count;
                                  });
            return (count > 0) ? sum / static_cast<f64>(count) : 0.0;
        }

        // Peak luma (grey levels) over cell (col,row)'s inner disk.
        f32 DiskPeakLuma(const std::vector<u8>& px, u32 col, u32 row)
        {
            f32 peak = 0.0f;
            ForEachInnerDiskPixel(col, row, [&](std::size_t idx)
                                  { peak = std::max(peak, LumaAt(px, idx)); });
            return peak;
        }

        // Mean luma (grey levels) over the whole frame.
        f64 FrameMeanLuma(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            const std::size_t pixelCount = static_cast<std::size_t>(kImageSize) * kImageSize;
            for (std::size_t p = 0; p < pixelCount; ++p)
                sum += static_cast<f64>(LumaAt(px, p * 4u));
            return sum / static_cast<f64>(pixelCount);
        }

        // Mean absolute per-pixel luma difference (grey levels) between two
        // frames of identical size.
        f64 FrameMeanAbsLumaDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            f64 sum = 0.0;
            const std::size_t pixelCount = static_cast<std::size_t>(kImageSize) * kImageSize;
            for (std::size_t p = 0; p < pixelCount; ++p)
                sum += std::abs(static_cast<f64>(LumaAt(a, p * 4u)) -
                                static_cast<f64>(LumaAt(b, p * 4u)));
            return sum / static_cast<f64>(pixelCount);
        }

        // Root-mean-square error over the RGB bytes of two frames.
        f64 FrameRmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            f64 sumSq = 0.0;
            const std::size_t pixelCount = static_cast<std::size_t>(kImageSize) * kImageSize;
            for (std::size_t p = 0; p < pixelCount; ++p)
            {
                for (std::size_t c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[p * 4u + c]) -
                                  static_cast<f64>(b[p * 4u + c]);
                    sumSq += d * d;
                }
            }
            return std::sqrt(sumSq / (static_cast<f64>(pixelCount) * 3.0));
        }

        fs::path VisualOutputDir()
        {
            // Same location the sibling visual-evidence tests use: under
            // `visual/` so the output is never confused with regression-
            // tracked goldens. The test binary runs from OloEditor/ (or repo
            // root, where the relative path degrades gracefully).
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }
    } // namespace

    // ========================================================================
    // MaterialLabVisualEvidence.RoughnessMetallicSweepAcrossViewAngles
    //
    // ========================================================================
    TEST(MaterialLabVisualEvidence, RoughnessMetallicSweepAcrossViewAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        VisualHarness harness;
        ASSERT_TRUE(harness.m_Fb != nullptr) << "framebuffer creation failed";
        ASSERT_TRUE(harness.m_Shader != nullptr) << "shader compile failed";

        struct ViewAngle
        {
            const char* Name;
            glm::vec3 Dir; // normalized by the harness before upload
        };
        // Head-on looks straight down the reconstruction axis; oblique tips
        // ~40 deg off it; grazing ~70 deg — the sweep that exercises NdotV,
        // Fresnel and highlight placement across the whole grid.
        const std::array<ViewAngle, 3> kViews = { {
            { "HeadOn", { 0.0f, 0.0f, 1.0f } },
            { "Oblique", { 0.55f, 0.15f, 0.82f } },
            { "Grazing", { 0.9f, 0.1f, 0.42f } },
        } };
        constexpr u32 kHeadOn = 0;
        constexpr u32 kGrazing = 2;

        std::array<std::vector<u8>, 3> captures;
        std::array<std::string, 3> capturePaths;
        const fs::path outDir = VisualOutputDir();

        for (u32 v = 0; v < kViews.size(); ++v)
        {
            harness.Draw(kViews[v].Dir);
            harness.Readback(captures[v]);
            ASSERT_EQ(captures[v].size(),
                      static_cast<std::size_t>(kImageSize) * kImageSize * 4u)
                << "readback failed for " << kViews[v].Name;

            capturePaths[v] =
                (outDir / (std::string("MaterialLab_ClosureV2_") +
                           kViews[v].Name + ".png"))
                    .string();
            const int wrote = ::stbi_write_png(
                capturePaths[v].c_str(), static_cast<int>(kImageSize),
                static_cast<int>(kImageSize), 4, captures[v].data(),
                static_cast<int>(kImageSize) * 4);
            EXPECT_NE(wrote, 0)
                << "failed to write visual evidence PNG " << capturePaths[v];
        }

        std::vector<u8> headOnRepeat;
        harness.Draw(kViews[kHeadOn].Dir);
        harness.Readback(headOnRepeat);
        ASSERT_EQ(headOnRepeat.size(),
                  static_cast<std::size_t>(kImageSize) * kImageSize * 4u);

        // PNG round-trip: reload one written frame to confirm the evidence on
        // disk is a decodable image, not just a nonzero write return.
        {
            const std::string& probePath = capturePaths[kHeadOn];
            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(probePath.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "failed to reload written PNG " << probePath;
            EXPECT_EQ(w, static_cast<int>(kImageSize));
            EXPECT_EQ(h, static_cast<int>(kImageSize));
            ::stbi_image_free(loaded);
        }

        // Grid cells named by (col = roughness index, row = metallic index).
        constexpr u32 kMirrorMetalCol = 0, kMirrorMetalRow = 6; // roughness 0, metallic 1

        // ---- (a) Non-vacuity, every capture. The 10..240 window separates a
        // real render from an all-black (failed draw) or all-white (blown
        // tonemap) frame while staying far from any legitimate mean — the
        // backdrop alone (~43% of the frame) pins the mean near ~78. The
        // cell-mean spread floor of 10 separates a real sweep from a uniform
        // frame (spread ~0): the roughness axis alone moves cell means by
        // several tens of grey levels.
        for (u32 v = 0; v < kViews.size(); ++v)
        {
            const std::vector<u8>& px = captures[v];
            const f64 frameMean = FrameMeanLuma(px);
            EXPECT_GT(frameMean, 10.0)
                << kViews[v].Name
                << " is suspiciously dark; see " << capturePaths[v];
            EXPECT_LT(frameMean, 240.0)
                << kViews[v].Name
                << " is suspiciously blown out; see " << capturePaths[v];

            f64 minCell = 255.0, maxCell = 0.0;
            for (u32 row = 0; row < kGridSize; ++row)
            {
                for (u32 col = 0; col < kGridSize; ++col)
                {
                    const f64 cellMean = DiskMeanLuma(px, col, row);
                    minCell = std::min(minCell, cellMean);
                    maxCell = std::max(maxCell, cellMean);
                }
            }
            EXPECT_GT(maxCell - minCell, 10.0)
                << kViews[v].Name
                << " renders a near-uniform grid (cell means " << minCell << ".."
                << maxCell << ") — the sweep is not sweeping; see "
                << capturePaths[v];
        }

        const f32 v2Peak =
            DiskPeakLuma(captures[kHeadOn], kMirrorMetalCol, kMirrorMetalRow);
        EXPECT_GT(v2Peak, 180.0f)
            << "ClosureV2's near-mirror cell shows no bright specular peak (peak="
            << v2Peak << ") — the highlight may have left the sampled disk or the "
            << "closure went dark. See " << capturePaths[kHeadOn];

        // Compare the roughest metal and dielectric to the same closure with only
        // its Kulla–Conty term removed. The dielectric's low F0 leaves the control
        // nearly unchanged, while multiple scattering returns energy to rough metal.
        harness.Draw(kViews[kHeadOn].Dir, /*omitMultiScatter=*/true);
        std::vector<u8> singleScatter;
        harness.Readback(singleScatter);
        ASSERT_EQ(singleScatter.size(), captures[kHeadOn].size());
        const auto lumaGain = [&](u32 metallicRow)
        {
            constexpr u32 kRoughestColumn = kGridSize - 1;
            return DiskMeanLuma(captures[kHeadOn], kRoughestColumn, metallicRow) -
                   DiskMeanLuma(singleScatter, kRoughestColumn, metallicRow);
        };
        EXPECT_GT(lumaGain(kGridSize - 1), 4.0);
        EXPECT_LT(std::abs(lumaGain(0)), 4.0);

        const f64 angleDiff = FrameMeanAbsLumaDiff(captures[kGrazing],
                                                   captures[kHeadOn]);
        EXPECT_GT(angleDiff, kMultiAngleFloor)
            << "ClosureV2's grazing capture barely differs from head-on (mean abs diff="
            << angleDiff << " grey levels) — the view-direction UBO lane may be dead. See "
            << capturePaths[kGrazing] << " vs "
            << capturePaths[kHeadOn];

        const f64 repeatRmse = FrameRmse(captures[kHeadOn], headOnRepeat);
        EXPECT_LE(repeatRmse, 0.1)
            << "two identical head-on renders differ (RMSE=" << repeatRmse
            << ") — the probe is nondeterministic, so the differential contracts in "
            << "this test are not trustworthy on this driver. See "
            << capturePaths[kHeadOn];
    }
} // namespace OloEngine::Tests
