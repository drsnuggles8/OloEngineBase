// OLO_TEST_LAYER: L8
// =============================================================================
// MaterialLabVisualEvidenceTest.cpp
//
// Material-laboratory visual evidence for issue #975 (PBR closure v2).
// Acceptance criterion served: "The material laboratory benchmark captures v2
// roughness/metallicity sweeps from multiple view angles."
//
// Drives assets/shaders/tests/MaterialLabProbe.glsl — a fullscreen analytic
// probe that shades a 7x7 sphere grid (x = roughness 0..1, y = metallic 0..1)
// through the REAL versioned dispatch `evaluatePBRClosure` — for BOTH models
// (Legacy / ClosureV2) from THREE view angles (head-on / ~40 deg oblique /
// ~70 deg grazing), 1024x1024 each, and writes all six frames to
//   OloEditor/assets/tests/visual/MaterialLab_<Legacy|ClosureV2>_<Angle>.png
// BEFORE any assertion runs, so a reviewer always has the frames to look at.
//
// Deliberately NOT in scope: issue #974's benchmark-scene manifest/tooling.
// This test is self-contained (probe shader + local harness, no scene, no
// manifest) precisely so that seam stays clean.
//
// All contracts are differential / goldenless (no baseline images — the
// golden-layer tests own regression pinning). Per the evidence-test rules,
// nothing here assumes absolute darkness: the probe's backdrop is a flat
// vec3(0.08) linear (~grey 78 after tonemap), and every classifier below is
// paired with an anti-vacuous check.
//
//   (a) Non-vacuity: every capture's full-frame mean luma sits in (10, 240)
//       and the per-cell means actually spread (the sweep is not a flat
//       frame).
//   (b) Near-mirror fix: in the mirror-metal cell (roughness 0, metallic 1)
//       ClosureV2's brightest pixel beats Legacy's by >= 20 grey levels at
//       head-on. See the comment at the assertion for why peak-vs-peak at
//       head-on cannot suffer dual tonemap saturation here.
//   (c) Energy compensation: the rough-metal cell (roughness 1, metallic 1)
//       gains >= 4 grey levels of mean brightness under ClosureV2 at EVERY
//       view angle; the rough-dielectric cell (roughness 1, metallic 0)
//       moves by LESS than that margin (compensation scales with F0) — the
//       anti-vacuous pair.
//   (d) Multi-angle capture is real: ClosureV2's grazing frame differs from
//       its head-on frame by > 2 grey levels of mean absolute luma.
//   (e) Noise floor / determinism: two Legacy head-on renders are (near-)
//       byte-identical — the sanity check the memory notes demand before
//       trusting any of the differentials above.
//
// Buffer orientation: ReadbackRgba8 returns rows in GL texture order, so
// buffer row 0 corresponds to v_TexCoord.y = 0 = metallic 0 and the grid is
// indexed WITHOUT a flip (grid row j occupies buffer rows [j*cell,(j+1)*cell)
// with metallic = j/6). The PNG is written unflipped, so metallic increases
// downward in the image (top row = dielectrics, bottom row = metals) and
// roughness increases left to right.
//
// Classification: L8 / integration — full GL pipeline, RGBA8 readback + PNG
// evidence keyed to a specific feature, same layer as the sibling
// SphereAreaLightVisualTest / WaterVisualEvidenceTest probes. SKIPs cleanly
// (not fails) without a GL 4.5+ context.
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
        constexpr f32 kNearMirrorPeakMargin = 20.0f; // contract (b)
        constexpr f64 kEnergyCompMargin = 4.0;       // contract (c)
        constexpr f64 kMultiAngleFloor = 2.0;        // contract (d)

        // GPU-mirror of MaterialLabProbe.glsl's MaterialLabParams UBO
        // (std140, binding 18): two 16-byte members, 32 bytes. Bare
        // PascalCase per the GPU-mirror struct convention.
        struct MaterialLabParamsUbo
        {
            f32 ViewDir[4];     // xyz = normalized view direction, w unused
            i32 ProbeParams[4]; // x = pbr model (0 = Legacy, 1 = ClosureV2)
        };
        static_assert(sizeof(MaterialLabParamsUbo) == 32, "std140 layout mismatch");

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

            void Draw(const glm::vec3& viewDir, i32 pbrModel)
            {
                MaterialLabParamsUbo data{};
                const glm::vec3 v = glm::normalize(viewDir);
                data.ViewDir[0] = v.x;
                data.ViewDir[1] = v.y;
                data.ViewDir[2] = v.z;
                data.ViewDir[3] = 0.0f;
                data.ProbeParams[0] = pbrModel;
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
                ReadbackRgba8(m_Fb->GetColorAttachmentRendererID(0), kImageSize, kImageSize,
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
    // Renders the 7x7 roughness x metallic laboratory for both closure models
    // from three view angles, writes the six PNGs, then asserts the
    // differential contracts documented in the file header.
    // ========================================================================
    TEST(MaterialLabVisualEvidence, RoughnessMetallicSweepAcrossViewAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        VisualHarness harness;
        ASSERT_TRUE(harness.m_Fb != nullptr) << "framebuffer creation failed";
        ASSERT_TRUE(harness.m_Shader != nullptr) << "shader compile failed";

        struct ModelArm
        {
            const char* Name;
            i32 Model; // OLO_PBR_MODEL_LEGACY = 0 / OLO_PBR_MODEL_CLOSURE_V2 = 1
        };
        constexpr std::array<ModelArm, 2> kModels = { { { "Legacy", 0 }, { "ClosureV2", 1 } } };
        constexpr u32 kLegacy = 0;
        constexpr u32 kClosureV2 = 1;

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

        // ---- Render + write ALL SIX PNGs first, before any assertion, so
        // ---- the evidence exists even when a later contract fails.
        std::array<std::vector<u8>, 6> captures; // index = model * 3 + view
        std::array<std::string, 6> capturePaths;
        const fs::path outDir = VisualOutputDir();

        for (u32 m = 0; m < kModels.size(); ++m)
        {
            for (u32 v = 0; v < kViews.size(); ++v)
            {
                const u32 slot = m * 3u + v;
                harness.Draw(kViews[v].Dir, kModels[m].Model);
                harness.Readback(captures[slot]);
                ASSERT_EQ(captures[slot].size(),
                          static_cast<std::size_t>(kImageSize) * kImageSize * 4u)
                    << "readback failed for " << kModels[m].Name << "/" << kViews[v].Name;

                capturePaths[slot] =
                    (outDir / (std::string("MaterialLab_") + kModels[m].Name + "_" +
                               kViews[v].Name + ".png"))
                        .string();
                const int wrote = ::stbi_write_png(
                    capturePaths[slot].c_str(), static_cast<int>(kImageSize),
                    static_cast<int>(kImageSize), 4, captures[slot].data(),
                    static_cast<int>(kImageSize) * 4);
                EXPECT_NE(wrote, 0)
                    << "failed to write visual evidence PNG " << capturePaths[slot];
            }
        }

        // Contract (e) input: a SECOND Legacy head-on render, captured before
        // the assertions so the determinism check shares the exact code path.
        std::vector<u8> legacyHeadOnRepeat;
        harness.Draw(kViews[kHeadOn].Dir, kModels[kLegacy].Model);
        harness.Readback(legacyHeadOnRepeat);
        ASSERT_EQ(legacyHeadOnRepeat.size(),
                  static_cast<std::size_t>(kImageSize) * kImageSize * 4u);

        // PNG round-trip: reload one written frame to confirm the evidence on
        // disk is a decodable image, not just a nonzero write return.
        {
            const std::string& probePath = capturePaths[kClosureV2 * 3u + kHeadOn];
            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(probePath.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "failed to reload written PNG " << probePath;
            EXPECT_EQ(w, static_cast<int>(kImageSize));
            EXPECT_EQ(h, static_cast<int>(kImageSize));
            ::stbi_image_free(loaded);
        }

        // Grid cells named by (col = roughness index, row = metallic index).
        constexpr u32 kMirrorMetalCol = 0, kMirrorMetalRow = 6; // roughness 0, metallic 1
        constexpr u32 kRoughCol = 6;                            // roughness 1
        constexpr u32 kMetalRow = 6, kDielectricRow = 0;        // metallic 1 / 0

        // ---- (a) Non-vacuity, every capture. The 10..240 window separates a
        // real render from an all-black (failed draw) or all-white (blown
        // tonemap) frame while staying far from any legitimate mean — the
        // backdrop alone (~43% of the frame) pins the mean near ~78. The
        // cell-mean spread floor of 10 separates a real sweep from a uniform
        // frame (spread ~0): the roughness axis alone moves cell means by
        // several tens of grey levels.
        for (u32 m = 0; m < kModels.size(); ++m)
        {
            for (u32 v = 0; v < kViews.size(); ++v)
            {
                const u32 slot = m * 3u + v;
                const std::vector<u8>& px = captures[slot];
                const f64 frameMean = FrameMeanLuma(px);
                EXPECT_GT(frameMean, 10.0)
                    << kModels[m].Name << "/" << kViews[v].Name
                    << " is suspiciously dark; see " << capturePaths[slot];
                EXPECT_LT(frameMean, 240.0)
                    << kModels[m].Name << "/" << kViews[v].Name
                    << " is suspiciously blown out; see " << capturePaths[slot];

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
                    << kModels[m].Name << "/" << kViews[v].Name
                    << " renders a near-uniform grid (cell means " << minCell << ".."
                    << maxCell << ") — the sweep is not sweeping; see "
                    << capturePaths[slot];
            }
        }

        // ---- (b) The near-mirror fix, visible. Chosen variant: brightest-
        // pixel vs brightest-pixel at HEAD-ON (not the oblique / top-decile
        // fallbacks), because dual tonemap saturation is impossible here:
        // Legacy receives the authored roughness 0 unclamped (exactly as
        // calculateLightContribution passes it), and distributionGGX's
        // numerator is a2 = roughness^4 = 0 — its specular lobe is
        // identically zero, and with metallic = 1 the diffuse term is zero
        // too, so the Legacy mirror-metal cell renders ambient-only
        // (~grey 50, nowhere near 255). ClosureV2's alpha floor
        // (MIN_ROUGHNESS) instead yields a finite, very bright highlight
        // whose nearest-pixel value comfortably exceeds 200 even at the
        // worst pixel-phase alignment. The +20 margin is an order of
        // magnitude above 8-bit quantization and driver-level shading
        // variance.
        const f32 v2Peak =
            DiskPeakLuma(captures[kClosureV2 * 3u + kHeadOn], kMirrorMetalCol, kMirrorMetalRow);
        const f32 legacyPeak =
            DiskPeakLuma(captures[kLegacy * 3u + kHeadOn], kMirrorMetalCol, kMirrorMetalRow);
        EXPECT_GE(v2Peak, legacyPeak + kNearMirrorPeakMargin)
            << "ClosureV2's near-mirror specular peak no longer beats Legacy's collapsed "
            << "lobe in the roughness-0/metallic-1 cell (v2Peak=" << v2Peak
            << ", legacyPeak=" << legacyPeak << ") — the v2 alpha-floor fix may have "
            << "regressed. See " << capturePaths[kClosureV2 * 3u + kHeadOn] << " vs "
            << capturePaths[kLegacy * 3u + kHeadOn];
        // Anti-vacuous pair for (b): the v2 peak must itself be a bright
        // highlight (a real specular dot, not merely "less dark than
        // Legacy's ambient floor"). 180 sits far above the ambient-only
        // level (~50) and below the worst-case pixel-phase peak (~240+).
        EXPECT_GT(v2Peak, 180.0f)
            << "ClosureV2's near-mirror cell shows no bright specular peak (peak="
            << v2Peak << ") — the highlight may have left the sampled disk or the "
            << "closure went dark. See " << capturePaths[kClosureV2 * 3u + kHeadOn];

        // ---- (c) Energy compensation, visible at EVERY view angle. Rough
        // metals (F0 ~ albedo) gain the Kulla-Conty multi-scatter energy, so
        // the roughness-1/metallic-1 cell must brighten by >= 4 grey levels
        // of disk-mean under ClosureV2 — 4 is ~2x the estimated post-tonemap
        // gain's safety margin over 8-bit noise while well under the
        // predicted 6-10 level gain. The anti-vacuous pair: the
        // roughness-1/metallic-0 cell (F0 = 0.04) must move by LESS than
        // that same margin, because the compensation term scales with F0^2 —
        // if BOTH cells moved, the differential is measuring a global shift
        // (tonemap/ambient change), not energy compensation.
        for (u32 v = 0; v < kViews.size(); ++v)
        {
            const std::vector<u8>& v2Px = captures[kClosureV2 * 3u + v];
            const std::vector<u8>& legacyPx = captures[kLegacy * 3u + v];

            const f64 metalGain = DiskMeanLuma(v2Px, kRoughCol, kMetalRow) -
                                  DiskMeanLuma(legacyPx, kRoughCol, kMetalRow);
            EXPECT_GE(metalGain, kEnergyCompMargin)
                << "ClosureV2's rough-metal cell gained only " << metalGain
                << " grey levels over Legacy at " << kViews[v].Name
                << " — multi-scatter energy compensation may have regressed. See "
                << capturePaths[kClosureV2 * 3u + v] << " vs "
                << capturePaths[kLegacy * 3u + v];

            const f64 dielectricShift =
                std::abs(DiskMeanLuma(v2Px, kRoughCol, kDielectricRow) -
                         DiskMeanLuma(legacyPx, kRoughCol, kDielectricRow));
            EXPECT_LT(dielectricShift, kEnergyCompMargin)
                << "the rough-DIELECTRIC cell moved by " << dielectricShift
                << " grey levels at " << kViews[v].Name
                << " — v2-vs-Legacy is shifting globally, so the rough-metal gain above "
                << "is not evidence of F0-scaled energy compensation. See "
                << capturePaths[kClosureV2 * 3u + v] << " vs "
                << capturePaths[kLegacy * 3u + v];
        }

        // ---- (d) Multi-angle capture actually varies. If the view-dir UBO
        // lane were dead (or all three captures rendered the same pose), the
        // grazing and head-on ClosureV2 frames would be identical; a real
        // view sweep moves highlights and Fresnel across ~57% of the frame
        // (the disks), which lands far above 2 grey levels of frame-wide
        // mean absolute difference — while 2 is far above the measured-zero
        // noise floor contract (e) establishes.
        const f64 angleDiff = FrameMeanAbsLumaDiff(captures[kClosureV2 * 3u + kGrazing],
                                                   captures[kClosureV2 * 3u + kHeadOn]);
        EXPECT_GT(angleDiff, kMultiAngleFloor)
            << "ClosureV2's grazing capture barely differs from head-on (mean abs diff="
            << angleDiff << " grey levels) — the view-direction UBO lane may be dead. See "
            << capturePaths[kClosureV2 * 3u + kGrazing] << " vs "
            << capturePaths[kClosureV2 * 3u + kHeadOn];

        // ---- (e) Legacy arm is the frozen closure — deterministic. No
        // golden here (the golden layer owns Legacy regression pinning);
        // instead two renders of the same Legacy head-on frame must be
        // (near-)byte-identical. Expected RMSE is exactly 0 (same program,
        // same UBO, same rasterization); the 0.1 floor only absorbs a
        // hypothetical driver that dithers RGBA8 conversion. This is also
        // the noise-floor measurement that licenses every differential
        // threshold above: if this fails, none of the other margins mean
        // anything.
        const f64 repeatRmse = FrameRmse(captures[kLegacy * 3u + kHeadOn], legacyHeadOnRepeat);
        EXPECT_LE(repeatRmse, 0.1)
            << "two identical Legacy head-on renders differ (RMSE=" << repeatRmse
            << ") — the probe is nondeterministic, so the differential contracts in "
            << "this test are not trustworthy on this driver. See "
            << capturePaths[kLegacy * 3u + kHeadOn];
    }
} // namespace OloEngine::Tests
