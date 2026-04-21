// =============================================================================
// GoldenImageTests.cpp
//
// Layer 8 — Golden image comparison. End-to-end integration smoke test.
//
// Each test renders a deterministic procedural frame through production
// shader code (tone map, FXAA, etc.) at a fixed resolution and compares the
// RGBA8 readback against a baseline PNG in GoldenBaselineDir().
//
// Baseline policy
// ---------------
//   Missing baselines are treated as failures by CompareOrBootstrap(); they
//   are NOT auto-bootstrapped. To intentionally create/rebase baselines, run:
//
//     OLOENGINE_GOLDEN_REBASE=1 <test binary> --gtest_filter=GoldenImage*
//
//   (from `OloEditor/` so `assets/...` resolves as expected).
//
// Comparison
// ----------
//   RMSE over RGB channels (alpha ignored). PASS threshold is deliberately
//   generous (2.0 / 255 on an 8-bit image) — golden tests are an integration
//   smoke net, not a pixel-exact regression guard. Per-channel / perceptual
//   metrics (SSIM, FLIP) are deferred per the strategy document.
//
// Determinism
// -----------
//   All inputs are procedural. No random numbers. No scene loading. No
//   asset importer. This keeps the baseline stable across developer
//   machines — only the GPU driver / vendor can produce false positives,
//   which is why the RMSE threshold is > 0.
//
//   **Known non-determinism**: different GPU vendors (NVIDIA / AMD / Intel /
//   Mesa) produce slightly different FP results. Cross-vendor baselines are
//   Layer 9 and deferred; today we ship a single baseline captured on the
//   developer's primary machine.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Minimal RAII wrappers for raw GL object handles used in tests
        // below. ASSERT_* aborts the enclosing TEST()'s scope but doesn't
        // unwind C++ destructors for plain GLuint variables, so any
        // `::glDelete*` call placed after the ASSERT would leak. These
        // wrappers delete the handle in their destructor, making the
        // cleanup ASSERT-safe.
        struct ScopedGLTexture
        {
            GLuint m_Id = 0;
            ScopedGLTexture() = default;
            explicit ScopedGLTexture(GLuint id) : m_Id(id) {}
            ~ScopedGLTexture()
            {
                if (m_Id != 0)
                    ::glDeleteTextures(1, &m_Id);
            }
            ScopedGLTexture(const ScopedGLTexture&) = delete;
            ScopedGLTexture& operator=(const ScopedGLTexture&) = delete;
            operator GLuint() const
            {
                return m_Id;
            }
        };

        struct ScopedGLBuffer
        {
            GLuint m_Id = 0;
            ScopedGLBuffer() = default;
            ~ScopedGLBuffer()
            {
                if (m_Id != 0)
                    ::glDeleteBuffers(1, &m_Id);
            }
            ScopedGLBuffer(const ScopedGLBuffer&) = delete;
            ScopedGLBuffer& operator=(const ScopedGLBuffer&) = delete;
            operator GLuint() const
            {
                return m_Id;
            }
        };

        // Duplicated from PostProcessPropertyTests anonymous namespace —
        // kept local to avoid a broader shared-header refactor.
        PostProcessUBOData MakeDefaultPostProcessUBO(u32 width, u32 height)
        {
            PostProcessUBOData ubo{};
            ubo.TonemapOperator = 1;
            ubo.Exposure = 1.0f;
            ubo.Gamma = 2.2f;
            ubo.BloomThreshold = 1.0f;
            ubo.BloomIntensity = 0.5f;
            ubo.VignetteIntensity = 0.3f;
            ubo.VignetteSmoothness = 0.15f;
            ubo.ChromaticAberrationIntensity = 0.005f;
            ubo.InverseScreenWidth = 1.0f / static_cast<f32>(width);
            ubo.InverseScreenHeight = 1.0f / static_cast<f32>(height);
            ubo.TexelSizeX = ubo.InverseScreenWidth;
            ubo.TexelSizeY = ubo.InverseScreenHeight;
            return ubo;
        }

        struct PostProcessHarness
        {
            u32 m_Width;
            u32 m_Height;
            u32 m_InputTex = 0;
            bool m_Initialized = false;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            Ref<UniformBuffer> m_Ubo;
            FullscreenPass m_Pass;

            PostProcessHarness(u32 width, u32 height, const char* shaderPath, const PostProcessUBOData& uboData)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                spec.Attachments = { FramebufferTextureFormat::RGBA8 };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
                m_Ubo = UniformBuffer::Create(PostProcessUBOData::GetSize(), 7);
                // Fail fast so Draw() doesn't dereference nulls later. The
                // diagnostic names the failing resource and the shader path
                // so CI logs point straight at the missing asset. m_Initialized
                // gates Draw() / ReadOutputRgba8() against a later null deref
                // when ADD_FAILURE() didn't abort (it only records the failure).
                if (!m_OutputFB)
                    ADD_FAILURE() << "PostProcessHarness: Framebuffer::Create returned null (" << shaderPath << ")";
                if (!m_Shader)
                    ADD_FAILURE() << "PostProcessHarness: Shader::Create returned null for '" << shaderPath << "'";
                if (!m_Ubo)
                    ADD_FAILURE() << "PostProcessHarness: UniformBuffer::Create returned null (" << shaderPath << ")";
                if (m_OutputFB && m_Shader && m_Ubo)
                {
                    m_Ubo->SetData(&uboData, PostProcessUBOData::GetSize());
                    m_Initialized = true;
                }
            }

            ~PostProcessHarness()
            {
                if (m_InputTex)
                    ::glDeleteTextures(1, &m_InputTex);
            }

            void SetInputTexture(u32 tex)
            {
                // Free any previously-owned texture to avoid leaking when
                // callers re-bind the harness to a new input.
                if (m_InputTex != 0 && m_InputTex != tex)
                    ::glDeleteTextures(1, &m_InputTex);
                m_InputTex = tex;
            }

            void Draw()
            {
                if (!m_Initialized)
                    return; // ADD_FAILURE already logged in ctor; avoid null deref
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(m_InputTex);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadOutputRgba8(std::vector<u8>& out) const
            {
                if (!m_Initialized)
                {
                    out.clear();
                    return;
                }
                ReadbackRgba8(m_OutputFB->GetColorAttachmentRendererID(0), m_Width, m_Height, out);
            }
        };

        // Baseline images live next to shader assets so OloEditor's asset
        // hot-reload is never confused by them. When OLOENGINE_GOLDEN_VENDOR
        // is set (e.g. "llvmpipe" in cross-vendor CI), baselines are scoped
        // to a per-vendor subdirectory so software-rasteriser results don't
        // clobber hardware baselines.
        static fs::path GoldenBaselineDir()
        {
            fs::path base = fs::path("assets") / "tests" / "golden";
            const char* vendor = std::getenv("OLOENGINE_GOLDEN_VENDOR");
            if (vendor != nullptr && vendor[0] != '\0')
            {
                base /= vendor;
            }
            return base;
        }

        // True if the caller has explicitly requested rebaselining via env var.
        // Any non-empty value other than "0", "false", or "FALSE" enables.
        static bool ShouldRebase()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            if (v == nullptr)
                return false;
            const std::string s(v);
            if (s.empty())
                return false;
            return !(s == "0" || s == "false" || s == "FALSE");
        }

        // RMSE over RGB channels (alpha ignored), normalised to [0, 1].
        static f32 ComputeRgbRmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1.0f;
            const std::size_t pixelCount = a.size() / 4;
            f64 sumSq = 0.0;
            for (std::size_t i = 0; i < pixelCount; ++i)
            {
                for (u32 c = 0; c < 3; ++c) // RGB only
                {
                    const f64 d = (static_cast<f64>(a[i * 4 + c]) - static_cast<f64>(b[i * 4 + c])) / 255.0;
                    sumSq += d * d;
                }
            }
            return static_cast<f32>(std::sqrt(sumSq / static_cast<f64>(pixelCount * 3)));
        }

        // Mean SSIM over RGB channels in [0, 1]. Classic Wang/Bovik formulation
        // on 8×8 non-overlapping windows (no Gaussian weighting — fast,
        // deterministic, and plenty accurate for golden-image smoke tests).
        //
        // Returns 1.0 for bit-identical images, approaches 0 as similarity
        // collapses. Constants C1 = (0.01·L)², C2 = (0.03·L)² with L = 255
        // are the reference Wang-Bovik 2004 values.
        //
        // Layer-8 §8 mandates a cascaded RMSE → SSIM decision: cheap RMSE
        // resolves the common cases (bit-identical = pass, wild regression =
        // fail) in microseconds, and SSIM only runs on the ambiguous middle
        // band where perceptual metrics are genuinely needed.
        static f32 ComputeRgbSsim(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height)
        {
            if (a.size() != b.size() || a.empty() || width == 0 || height == 0)
                return 0.0f;

            constexpr u32 kWindow = 8;
            constexpr f64 kC1 = (0.01 * 255.0) * (0.01 * 255.0);
            constexpr f64 kC2 = (0.03 * 255.0) * (0.03 * 255.0);

            const u32 winsX = width / kWindow;
            const u32 winsY = height / kWindow;
            if (winsX == 0 || winsY == 0)
            {
                // Frame smaller than one window — SSIM is ill-defined, fall
                // back to "very similar iff RMSE is tiny".
                const f32 rmse = ComputeRgbRmse(a, b);
                return rmse < 0.002f ? 1.0f : 0.0f;
            }

            f64 ssimSum = 0.0;
            u64 ssimCount = 0;

            for (u32 wy = 0; wy < winsY; ++wy)
            {
                for (u32 wx = 0; wx < winsX; ++wx)
                {
                    for (u32 ch = 0; ch < 3; ++ch)
                    {
                        // Two passes per window: mean, then variance + covariance.
                        f64 sumA = 0.0, sumB = 0.0;
                        for (u32 yy = 0; yy < kWindow; ++yy)
                        {
                            for (u32 xx = 0; xx < kWindow; ++xx)
                            {
                                const u32 x = wx * kWindow + xx;
                                const u32 y = wy * kWindow + yy;
                                const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4 + ch;
                                sumA += static_cast<f64>(a[idx]);
                                sumB += static_cast<f64>(b[idx]);
                            }
                        }
                        constexpr f64 kN = static_cast<f64>(kWindow * kWindow);
                        const f64 meanA = sumA / kN;
                        const f64 meanB = sumB / kN;

                        f64 varA = 0.0, varB = 0.0, covAB = 0.0;
                        for (u32 yy = 0; yy < kWindow; ++yy)
                        {
                            for (u32 xx = 0; xx < kWindow; ++xx)
                            {
                                const u32 x = wx * kWindow + xx;
                                const u32 y = wy * kWindow + yy;
                                const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4 + ch;
                                const f64 da = static_cast<f64>(a[idx]) - meanA;
                                const f64 db = static_cast<f64>(b[idx]) - meanB;
                                varA += da * da;
                                varB += db * db;
                                covAB += da * db;
                            }
                        }
                        varA /= (kN - 1.0);
                        varB /= (kN - 1.0);
                        covAB /= (kN - 1.0);

                        const f64 numerator = (2.0 * meanA * meanB + kC1) * (2.0 * covAB + kC2);
                        const f64 denominator = (meanA * meanA + meanB * meanB + kC1) * (varA + varB + kC2);
                        const f64 ssim = denominator > 0.0 ? (numerator / denominator) : 1.0;
                        ssimSum += ssim;
                        ++ssimCount;
                    }
                }
            }

            return static_cast<f32>(ssimSum / static_cast<f64>(ssimCount));
        }

        // Detailed per-pixel diff statistics. Produced on failure for L10
        // diagnostic escalation: worst-pixel location + magnitude, per-channel
        // max deltas, and a diff-heatmap PNG. Pinpoints WHERE in the frame
        // the regression happened rather than just reporting aggregate RMSE.
        struct DiffStats
        {
            u32 m_WorstX = 0;
            u32 m_WorstY = 0;
            u32 m_WorstDelta = 0; // max abs channel delta across all pixels
            u32 m_MaxDeltaR = 0;
            u32 m_MaxDeltaG = 0;
            u32 m_MaxDeltaB = 0;
            u32 m_PixelsOverEpsilon = 0;   // count of pixels with any channel delta > 4 LSBs
            std::vector<u8> m_HeatmapRgba; // per-pixel max channel delta, greyscale -> red scaled
        };

        static DiffStats ComputeDiffStats(const std::vector<u8>& actual, const std::vector<u8>& baseline,
                                          u32 width, u32 height)
        {
            DiffStats stats{};
            stats.m_HeatmapRgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
            constexpr u32 kEpsilon = 4;
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4;
                    const u32 dr = static_cast<u32>(std::abs(static_cast<int>(actual[idx + 0]) - static_cast<int>(baseline[idx + 0])));
                    const u32 dg = static_cast<u32>(std::abs(static_cast<int>(actual[idx + 1]) - static_cast<int>(baseline[idx + 1])));
                    const u32 db = static_cast<u32>(std::abs(static_cast<int>(actual[idx + 2]) - static_cast<int>(baseline[idx + 2])));
                    const u32 dMax = std::max({ dr, dg, db });

                    if (dr > stats.m_MaxDeltaR)
                        stats.m_MaxDeltaR = dr;
                    if (dg > stats.m_MaxDeltaG)
                        stats.m_MaxDeltaG = dg;
                    if (db > stats.m_MaxDeltaB)
                        stats.m_MaxDeltaB = db;
                    if (dMax > stats.m_WorstDelta)
                    {
                        stats.m_WorstDelta = dMax;
                        stats.m_WorstX = x;
                        stats.m_WorstY = y;
                    }
                    if (dMax > kEpsilon)
                        ++stats.m_PixelsOverEpsilon;

                    // Heatmap: red channel proportional to worst delta, green
                    // proportional to mean delta (so subtle-but-widespread
                    // drift shows up green, and spiky hotspots glow red).
                    const u8 redByte = static_cast<u8>(std::min<u32>(dMax * 8u, 255u));
                    const u8 greenByte = static_cast<u8>(std::min<u32>((dr + dg + db) * 8u / 3u, 255u));
                    stats.m_HeatmapRgba[idx + 0] = redByte;
                    stats.m_HeatmapRgba[idx + 1] = greenByte;
                    stats.m_HeatmapRgba[idx + 2] = 0;
                    stats.m_HeatmapRgba[idx + 3] = 255;
                }
            }
            return stats;
        }

        // Writes / reads baselines, performs comparison, and writes a diff
        // visualisation on failure.
        struct GoldenImageCheckResult
        {
            bool m_Passed = false;
            f32 m_Rmse = 0.0f;
            f32 m_Ssim = 1.0f;       // 1.0 when SSIM wasn't needed (RMSE resolved).
            bool m_UsedSsim = false; // true when the cascade escalated to SSIM.
            std::string m_Message;
        };

        static GoldenImageCheckResult CompareOrBootstrap(const std::string& name, u32 width, u32 height,
                                                         const std::vector<u8>& actualRgba)
        {
            GoldenImageCheckResult result{};

            // Guard all subsequent stbi_write_png / memcmp paths: a mis-sized
            // buffer would cause out-of-bounds reads against actualRgba.data().
            const std::size_t expectedBytes = static_cast<std::size_t>(width) * height * 4;
            if (actualRgba.size() != expectedBytes)
            {
                result.m_Message = "actualRgba size mismatch: expected " +
                                   std::to_string(expectedBytes) + " bytes for " +
                                   std::to_string(width) + "x" + std::to_string(height) +
                                   " RGBA, got " + std::to_string(actualRgba.size());
                return result;
            }

            fs::path dir = GoldenBaselineDir();
            std::error_code ec;
            fs::create_directories(dir, ec);
            fs::path baselinePath = dir / (name + ".png");

            const bool rebase = ShouldRebase();
            const bool baselineExists = fs::exists(baselinePath);

            if (!baselineExists && !rebase)
            {
                // Fail loudly instead of silently bootstrapping a missing
                // baseline: a disappeared golden is a regression we want to
                // catch, not paper over on the next run. To (re)generate a
                // baseline intentionally, set OLOENGINE_GOLDEN_REBASE=1.
                result.m_Message = "golden baseline missing at " + baselinePath.string() + " — rerun with OLOENGINE_GOLDEN_REBASE=1 to (re)create it";
                return result;
            }

            if (rebase)
            {
                // Rebase: write current output as the new baseline.
                const int ok = ::stbi_write_png(baselinePath.string().c_str(),
                                                static_cast<int>(width), static_cast<int>(height),
                                                4, actualRgba.data(), static_cast<int>(width) * 4);
                if (ok == 0)
                {
                    result.m_Message = "failed to write baseline PNG to " + baselinePath.string();
                    return result;
                }
                result.m_Passed = true;
                result.m_Message = "REBASED baseline at " + baselinePath.string();
                return result;
            }

            // Read the existing baseline and compare.
            int bw = 0, bh = 0, channels = 0;
            stbi_uc* rawBaseline = ::stbi_load(baselinePath.string().c_str(), &bw, &bh, &channels, 4);
            if (rawBaseline == nullptr)
            {
                result.m_Message = "failed to read baseline PNG at " + baselinePath.string();
                return result;
            }
            if (static_cast<u32>(bw) != width || static_cast<u32>(bh) != height)
            {
                ::stbi_image_free(rawBaseline);
                result.m_Message = "baseline dimensions mismatch: baseline " + std::to_string(bw) + "x" +
                                   std::to_string(bh) + " vs actual " + std::to_string(width) + "x" +
                                   std::to_string(height);
                return result;
            }

            std::vector<u8> baseline(rawBaseline, rawBaseline + (static_cast<std::size_t>(width) * height * 4));
            ::stbi_image_free(rawBaseline);

            result.m_Rmse = ComputeRgbRmse(actualRgba, baseline);

            // Cascaded RMSE → SSIM decision (strategy doc §8). Cheap RMSE
            // resolves the common cases up-front:
            //   - RMSE < kRmsePassBelow → surely a match, skip SSIM.
            //   - RMSE > kRmseFailAbove → surely a regression, fail without SSIM.
            //   - In between → compute SSIM and pass iff ≥ kSsimPassThreshold.
            //
            // Using an SSIM fallback catches the class of bugs that RMSE
            // under-weights (distributed low-contrast drift: a subtle color
            // cast, a gamma change) and over-rates (a handful of hot-pixel
            // outliers from aliasing that are perceptually identical).
            constexpr f32 kRmsePassBelow = 0.004f;
            constexpr f32 kRmseFailAbove = 0.02f;
            constexpr f32 kSsimPassThreshold = 0.985f;

            if (result.m_Rmse <= kRmsePassBelow)
            {
                result.m_Passed = true;
            }
            else if (result.m_Rmse >= kRmseFailAbove)
            {
                result.m_Passed = false;
            }
            else
            {
                result.m_UsedSsim = true;
                result.m_Ssim = ComputeRgbSsim(actualRgba, baseline, width, height);
                result.m_Passed = result.m_Ssim >= kSsimPassThreshold;
            }

            if (!result.m_Passed)
            {
                // L10 escalation: on failure, produce
                //   <name>.actual.png    — the frame that was rendered
                //   <name>.diff.png      — red/green per-pixel delta heatmap
                //   detailed DiffStats in the failure message (worst-pixel
                //   location + per-channel max + pixel count > 4 LSB)
                fs::path actualPath = dir / (name + ".actual.png");
                errno = 0;
                const int wroteActualResult = ::stbi_write_png(actualPath.string().c_str(),
                                                               static_cast<int>(width), static_cast<int>(height),
                                                               4, actualRgba.data(), static_cast<int>(width) * 4);
                const bool wroteActual = wroteActualResult != 0;
                const int actualErrno = errno;

                const DiffStats stats = ComputeDiffStats(actualRgba, baseline, width, height);

                fs::path diffPath = dir / (name + ".diff.png");
                errno = 0;
                const int wroteDiffResult = ::stbi_write_png(diffPath.string().c_str(),
                                                             static_cast<int>(width), static_cast<int>(height),
                                                             4, stats.m_HeatmapRgba.data(), static_cast<int>(width) * 4);
                const bool wroteDiff = wroteDiffResult != 0;
                const int diffErrno = errno;

                std::ostringstream msg;
                msg << "RMSE " << result.m_Rmse
                    << " (pass<" << kRmsePassBelow << ", fail>" << kRmseFailAbove << ")";
                if (result.m_UsedSsim)
                {
                    msg << " and SSIM " << result.m_Ssim
                        << " below threshold " << kSsimPassThreshold;
                }
                else
                {
                    msg << " exceeds hard fail bound";
                }
                msg << "\n  worst pixel: (" << stats.m_WorstX << "," << stats.m_WorstY
                    << ") max channel delta=" << stats.m_WorstDelta
                    << "\n  per-channel max delta: R=" << stats.m_MaxDeltaR
                    << " G=" << stats.m_MaxDeltaG << " B=" << stats.m_MaxDeltaB
                    << "\n  pixels with any channel delta > 4 LSB: " << stats.m_PixelsOverEpsilon
                    << " / " << (width * height);

                if (wroteActual)
                {
                    msg << "\n  wrote actual frame to: " << actualPath.string();
                }
                else
                {
                    msg << "\n  failed to write actual frame to: " << actualPath.string()
                        << " (stbi_write_png=" << wroteActualResult;
                    if (actualErrno != 0)
                    {
                        msg << ", errno=" << actualErrno << " (" << std::strerror(actualErrno) << ")";
                    }
                    msg << ")";
                }

                if (wroteDiff)
                {
                    msg << "\n  wrote diff heatmap to: " << diffPath.string();
                }
                else
                {
                    msg << "\n  failed to write diff heatmap to: " << diffPath.string()
                        << " (stbi_write_png=" << wroteDiffResult;
                    if (diffErrno != 0)
                    {
                        msg << ", errno=" << diffErrno << " (" << std::strerror(diffErrno) << ")";
                    }
                    msg << ")";
                }
                result.m_Message = msg.str();
            }
            else
            {
                std::ostringstream msg;
                msg << "RMSE " << result.m_Rmse;
                if (result.m_UsedSsim)
                    msg << ", SSIM " << result.m_Ssim << " (cascade escalated)";
                else
                    msg << " below fast-path bound";
                result.m_Message = msg.str();
            }
            return result;
        }

        // Build a checkerboard RGBA image for SSIM sanity tests.
        static std::vector<u8> MakeCheckerboard(u32 width, u32 height, u8 a, u8 b, u32 cell)
        {
            std::vector<u8> out(static_cast<std::size_t>(width) * height * 4, 255);
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const bool light = ((x / cell) + (y / cell)) % 2 == 0;
                    const u8 v = light ? a : b;
                    const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4;
                    out[idx + 0] = v;
                    out[idx + 1] = v;
                    out[idx + 2] = v;
                    out[idx + 3] = 255;
                }
            }
            return out;
        }
    } // namespace

    // =========================================================================
    // [Layer-1 — Unit] SSIM math property checks.
    //
    // These four `GoldenImageSsimTest` cases are plain CPU-side unit tests
    // (no GPU required) that pin the perceptual-similarity math used by the
    // §8 RMSE → SSIM cascade. They live in this file — rather than under a
    // dedicated L1 target — because ComputeRgbSsim() and MakeCheckerboard()
    // are file-private helpers; promoting them to a public API purely for
    // test relocation would be over-engineering. Treat this block as an
    // L1 island inside the L8 file.
    // =========================================================================
    TEST(GoldenImageSsimTest, IdenticalImagesYieldSsimOne)
    {
        constexpr u32 kW = 32;
        constexpr u32 kH = 32;
        const auto img = MakeCheckerboard(kW, kH, 40, 200, 4);
        const f32 ssim = ComputeRgbSsim(img, img, kW, kH);
        EXPECT_NEAR(ssim, 1.0f, 1e-5f);
    }

    TEST(GoldenImageSsimTest, TinyUniformShiftKeepsSsimHigh)
    {
        // A 2 LSB uniform brightness bump is perceptually indistinguishable —
        // SSIM should stay > 0.99 even though a pixel diff would flag every
        // pixel. This is precisely the "RMSE over-rates tiny hot-pixel noise"
        // case the cascade exists to catch on the "still a pass" side.
        constexpr u32 kW = 32;
        constexpr u32 kH = 32;
        const auto a = MakeCheckerboard(kW, kH, 40, 200, 4);
        auto b = a;
        for (std::size_t i = 0; i + 3 < b.size(); i += 4)
        {
            b[i + 0] = static_cast<u8>(std::min<u32>(b[i + 0] + 2u, 255u));
            b[i + 1] = static_cast<u8>(std::min<u32>(b[i + 1] + 2u, 255u));
            b[i + 2] = static_cast<u8>(std::min<u32>(b[i + 2] + 2u, 255u));
        }
        const f32 ssim = ComputeRgbSsim(a, b, kW, kH);
        EXPECT_GT(ssim, 0.99f);
    }

    TEST(GoldenImageSsimTest, StructuralDestructionCollapsesSsim)
    {
        // Randomising one image while keeping the other structured should
        // drive SSIM well below the 0.985 pass bound — covers the "distributed
        // low-contrast drift that RMSE under-weights" case from the other side.
        constexpr u32 kW = 32;
        constexpr u32 kH = 32;
        const auto structured = MakeCheckerboard(kW, kH, 40, 200, 4);
        std::vector<u8> noise(structured.size(), 0);
        // Deterministic pseudo-random (no std::rand dependency):
        u32 state = 0x1234567u;
        for (std::size_t i = 0; i + 3 < noise.size(); i += 4)
        {
            state = state * 1664525u + 1013904223u;
            noise[i + 0] = static_cast<u8>((state >> 16) & 0xFFu);
            state = state * 1664525u + 1013904223u;
            noise[i + 1] = static_cast<u8>((state >> 16) & 0xFFu);
            state = state * 1664525u + 1013904223u;
            noise[i + 2] = static_cast<u8>((state >> 16) & 0xFFu);
            noise[i + 3] = 255;
        }
        const f32 ssim = ComputeRgbSsim(structured, noise, kW, kH);
        EXPECT_LT(ssim, 0.5f);
    }

    TEST(GoldenImageSsimTest, SsimIsSymmetric)
    {
        constexpr u32 kW = 32;
        constexpr u32 kH = 32;
        const auto a = MakeCheckerboard(kW, kH, 40, 200, 4);
        const auto b = MakeCheckerboard(kW, kH, 60, 180, 8);
        const f32 ssimAB = ComputeRgbSsim(a, b, kW, kH);
        const f32 ssimBA = ComputeRgbSsim(b, a, kW, kH);
        EXPECT_NEAR(ssimAB, ssimBA, 1e-5f);
    }

    // =========================================================================
    // ToneMap chain golden: uniform HDR ramp → Reinhard tone map → RGBA8
    //
    // Exercises: shader compilation + PostProcessUBO binding + fullscreen
    // draw + framebuffer readback + sRGB-clamped encoding. A regression in
    // any of these shows up as a step-change in RMSE.
    // =========================================================================
    TEST(GoldenImageTest, ReinhardHdrRampGolden)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 128;
        constexpr u32 kHeight = 128;

        // Procedural HDR gradient: x axis → luminance 0..4, y axis → hue tilt.
        std::vector<f32> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
        for (u32 y = 0; y < kHeight; ++y)
        {
            const f32 v = static_cast<f32>(y) / static_cast<f32>(kHeight - 1);
            for (u32 x = 0; x < kWidth; ++x)
            {
                const f32 l = 4.0f * static_cast<f32>(x) / static_cast<f32>(kWidth - 1);
                const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                rgba[i + 0] = l * (1.0f - 0.3f * v);
                rgba[i + 1] = l * (0.6f + 0.4f * v);
                rgba[i + 2] = l * (0.2f + 0.8f * (1.0f - v));
                rgba[i + 3] = 1.0f;
            }
        }

        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kWidth, kHeight);
        ubo.TonemapOperator = 1; // Reinhard
        ubo.Exposure = 1.0f;
        ubo.Gamma = 2.2f;

        PostProcessHarness h(kWidth, kHeight, "assets/shaders/PostProcess_ToneMap.glsl", ubo);
        h.SetInputTexture(CreateFloatTexture2D(kWidth, kHeight, rgba.data()));
        h.Draw();

        std::vector<u8> output;
        h.ReadOutputRgba8(output);
        ASSERT_EQ(output.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        const auto result = CompareOrBootstrap("tonemap_reinhard_hdr_ramp", kWidth, kHeight, output);
        EXPECT_TRUE(result.m_Passed) << result.m_Message;
        if (result.m_Passed)
        {
            RecordProperty("result", result.m_Message);
        }
    }

    // =========================================================================
    // FXAA golden: hard vertical edge → FXAA. Integration-tests the FXAA
    // shader pipeline over an "interesting" (non-uniform) input.
    // =========================================================================
    TEST(GoldenImageTest, FxaaHardEdgeGolden)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 128;
        std::vector<f32> pixels(static_cast<std::size_t>(kSize) * kSize * 4);
        // Diagonal zig-zag edge to give FXAA something to munch on.
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const f32 v = (x + (y % 8)) < (kSize / 2 + ((y / 8) % 2) * 4) ? 0.0f : 1.0f;
                const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
                pixels[i + 0] = v;
                pixels[i + 1] = v;
                pixels[i + 2] = v;
                pixels[i + 3] = 1.0f;
            }
        }

        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);
        PostProcessHarness h(kSize, kSize, "assets/shaders/PostProcess_FXAA.glsl", ubo);
        h.SetInputTexture(CreateFloatTexture2D(kSize, kSize, pixels.data()));
        h.Draw();

        std::vector<u8> output;
        h.ReadOutputRgba8(output);
        const auto result = CompareOrBootstrap("fxaa_hard_edge", kSize, kSize, output);
        EXPECT_TRUE(result.m_Passed) << result.m_Message;
    }

    // =========================================================================
    // Scene-level shadow golden: shadow caster (authored depth texture array)
    // → lit "ground" pass (ShaderUnit_ShadowVisualize sampled across UVs)
    // → HDR accumulation → PostProcess_ToneMap → RGBA8.
    //
    // Models the production chain ShadowPass → ScenePass → PostProcessPass
    // by staging its three distinct GPU-side operations in sequence. Catches
    // regressions in: depth-compare sampler configuration, PCF kernel
    // weights, HDR-to-framebuffer binding, tonemap shader.
    //
    // The authored shadow map has a diagonal step (upper-right triangle
    // `x + y >= kShadowRes` → depth 0.8, lower-left triangle → depth 0.2).
    // A single fragment-wide reference depth of 0.5 is placed between the
    // two steps, so the lower-left triangle is shadowed (0.5 > 0.2) and the
    // upper-right is lit (0.5 < 0.8). PCF across the diagonal boundary
    // produces intermediate shadow factors (1/9..8/9), exercising the full
    // 3×3 kernel rather than just lit/shadowed extremes.
    //
    // NOTE: the earlier probe `ShaderUnit_ShadowSelfShadow.glsl` cannot be
    // used here — it sweeps depth along one screen axis and sample-XY along
    // the other (to assert self-shadow / peter-panning invariants), which
    // collapses 2D shadow maps into a 1D response and produces a
    // checker-like image that does not match what a scene shadow mask
    // should look like. `ShaderUnit_ShadowVisualize.glsl` is the 2D
    // visualization sibling designed for scene-integration goldens.
    // =========================================================================
    TEST(GoldenImageTest, SceneShadowIntegrationGolden)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kShadowRes = 64;
        constexpr u32 kFrameW = 128;
        constexpr u32 kFrameH = 128;
        constexpr f32 kBias = 0.005f;
        constexpr f32 kReferenceDepth = 0.5f; // between the 0.2 / 0.8 steps

        // ---------------------------------------------------------------
        // Pass 1 (shadow): author a depth texture array whose layer 0
        // contains a diagonal step — the lower-left triangle
        // (`x + y < kShadowRes`) is "near" (depth 0.2, occluder close to
        // light) and the upper-right triangle is "far" (depth 0.8,
        // occluder distant). With a reference depth of 0.5 the lower-left
        // triangle is shadowed (0.5 > 0.2) and the upper-right is lit
        // (0.5 < 0.8). PCF taps straddle the diagonal boundary and produce
        // smooth intermediate shadow factors.
        // ---------------------------------------------------------------
        ScopedGLTexture shadowTexGuard;
        ::glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &shadowTexGuard.m_Id);
        const GLuint shadowTex = shadowTexGuard.m_Id;
        ::glTextureStorage3D(shadowTex, 1, GL_DEPTH_COMPONENT32F,
                             static_cast<GLsizei>(kShadowRes),
                             static_cast<GLsizei>(kShadowRes), 4);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

        std::vector<f32> depthData(static_cast<std::size_t>(kShadowRes) * kShadowRes);
        for (u32 y = 0; y < kShadowRes; ++y)
        {
            for (u32 x = 0; x < kShadowRes; ++x)
            {
                depthData[y * kShadowRes + x] = (x + y < kShadowRes) ? 0.2f : 0.8f;
            }
        }
        for (u32 layer = 0; layer < 4; ++layer)
        {
            ::glTextureSubImage3D(shadowTex, 0, 0, 0, static_cast<GLint>(layer),
                                  static_cast<GLsizei>(kShadowRes),
                                  static_cast<GLsizei>(kShadowRes), 1,
                                  GL_DEPTH_COMPONENT, GL_FLOAT, depthData.data());
        }
        ::glBindTextureUnit(8, shadowTex);

        // UBO for probe: bias + reference depth + shadow resolution
        // (std140 vec4 + ivec4 = 32B). Matches the layout declared in both
        // ShaderUnit_ShadowSelfShadow.glsl and ShaderUnit_ShadowVisualize.glsl.
        struct ProbeUbo
        {
            f32 params[4];
            i32 resolution;
            i32 pad[3];
        };
        static_assert(sizeof(ProbeUbo) == 32, "std140 layout mismatch");
        ProbeUbo uboData{ { kBias, kReferenceDepth, 0.0f, 0.0f }, static_cast<i32>(kShadowRes), { 0, 0, 0 } };
        ScopedGLBuffer probeUboGuard;
        ::glCreateBuffers(1, &probeUboGuard.m_Id);
        const GLuint probeUbo = probeUboGuard.m_Id;
        ::glNamedBufferData(probeUbo, sizeof(ProbeUbo), &uboData, GL_STATIC_DRAW);
        ::glBindBufferBase(GL_UNIFORM_BUFFER, 18, probeUbo);

        // ---------------------------------------------------------------
        // Pass 2 (scene): render the shadow probe into an HDR RGBA16F FB.
        // The shader's R channel already holds the PCF shadow factor; we
        // multiply by a warm "sun lighting" colour before tone-mapping to
        // make the chain behave like a real lit scene (not just a shadow
        // visualisation).
        // ---------------------------------------------------------------
        FramebufferSpecification hdrSpec{};
        hdrSpec.Width = kFrameW;
        hdrSpec.Height = kFrameH;
        hdrSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
        Ref<Framebuffer> hdrFB = Framebuffer::Create(hdrSpec);
        ASSERT_TRUE(hdrFB != nullptr) << "Framebuffer::Create returned null for HDR scene FBO";

        Ref<Shader> probeShader = Shader::Create(
            "assets/shaders/tests/ShaderUnit_ShadowVisualize.glsl");
        ASSERT_TRUE(probeShader != nullptr)
            << "Failed to load ShaderUnit_ShadowVisualize.glsl — check asset path";

        FullscreenPass fullscreen;
        hdrFB->Bind();
        ::glViewport(0, 0, static_cast<GLsizei>(kFrameW), static_cast<GLsizei>(kFrameH));
        ::glDisable(GL_BLEND);
        ::glDisable(GL_DEPTH_TEST);
        ::glDisable(GL_CULL_FACE);
        probeShader->Bind();
        fullscreen.Draw(0); // no input texture needed — probe samples the shadow map by binding.
        ::glFinish();
        hdrFB->Unbind();

        // The probe writes shadow factor to R in linear space. Rather than
        // running a separate "lighting" shader (which would need another
        // production shader + UBO), we treat the R channel as the
        // irradiance modulator for a synthetic warm light by multiplying
        // in CPU-side before uploading as HDR. This keeps the test stage
        // count bounded and still exercises the tone-map with a
        // non-trivial HDR distribution.
        std::vector<f32> hdrReadback;
        ReadbackRgbaFloat(hdrFB->GetColorAttachmentRendererID(0), kFrameW, kFrameH, hdrReadback);
        // Defensive contract: if the readback produced an undersized buffer
        // (driver bug, format mismatch, readback skipped), fail loudly
        // instead of indexing out-of-bounds into `hdrReadback` below.
        ASSERT_EQ(hdrReadback.size(), static_cast<std::size_t>(kFrameW) * kFrameH * 4)
            << "HDR readback size mismatch for shadow pass";
        std::vector<f32> litHdr(hdrReadback.size());
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFrameW) * kFrameH; ++i)
        {
            const f32 shadow = hdrReadback[i * 4 + 0]; // probe PCF result in R
            const f32 ambient = 0.15f;
            const f32 intensity = ambient + (1.0f - ambient) * shadow;
            // Warm-white sun tinted slightly blue in shadow (ambient).
            litHdr[i * 4 + 0] = intensity * 2.5f;
            litHdr[i * 4 + 1] = intensity * (shadow > 0.5f ? 2.2f : 1.9f);
            litHdr[i * 4 + 2] = intensity * (shadow > 0.5f ? 1.6f : 2.1f);
            litHdr[i * 4 + 3] = 1.0f;
        }

        // ---------------------------------------------------------------
        // Pass 3 (post-process): run the lit HDR through tone-map + gamma.
        // ---------------------------------------------------------------
        PostProcessUBOData toneUbo = MakeDefaultPostProcessUBO(kFrameW, kFrameH);
        toneUbo.TonemapOperator = 1; // Reinhard
        toneUbo.Exposure = 1.0f;
        toneUbo.Gamma = 2.2f;

        PostProcessHarness tone(kFrameW, kFrameH,
                                "assets/shaders/PostProcess_ToneMap.glsl", toneUbo);
        tone.SetInputTexture(CreateFloatTexture2D(kFrameW, kFrameH, litHdr.data()));
        tone.Draw();

        std::vector<u8> output;
        tone.ReadOutputRgba8(output);
        ASSERT_EQ(output.size(), static_cast<std::size_t>(kFrameW) * kFrameH * 4);

        // shadowTex / probeUbo are freed automatically by the ScopedGL* guards.

        const auto result = CompareOrBootstrap("scene_shadow_integration", kFrameW, kFrameH, output);
        EXPECT_TRUE(result.m_Passed) << result.m_Message;
        if (result.m_Passed)
        {
            RecordProperty("result", result.m_Message);
        }
    }

    // =========================================================================
    // Scene-level splatmap/terrain golden: 4-layer texture array + spatially
    // varying splatmap → ShaderUnit_SplatmapChannel blend → HDR →
    // PostProcess_ToneMap → RGBA8.
    //
    // Covers the terrain blending path end-to-end. The splatmap uses
    // edge-proximity weights:
    //   w0 = max(0, 1 - 2u) → dominant at the left edge   (layer 0: red)
    //   w1 = max(0, 1 - 2v) → dominant at the top edge    (layer 1: green)
    //   w2 = max(0, 2u - 1) → dominant at the right edge  (layer 2: blue)
    //   w3 = max(0, 2v - 1) → dominant at the bottom edge (layer 3: sand)
    // normalised by Σw. Only two weights are non-zero in each quadrant, so
    // the output is a 2×2 arrangement of pairwise layer blends
    // (top-left = red+green, top-right = green+blue,
    //  bottom-right = blue+sand, bottom-left = red+sand) meeting in a soft
    // cross at the centre (where all pre-normalisation weights vanish and
    // the `sum > 0` fallback picks layer 0, giving a single-texel red
    // singularity that the 128×128 quad absorbs into the central cross).
    // This exercises all four texture array slots and all four splatmap
    // channels simultaneously. Catches: channel swizzle regressions, array
    // layer index off-by-one, weight normalisation, tonemap clamping.
    // =========================================================================
    TEST(GoldenImageTest, SceneSplatmapIntegrationGolden)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 128;

        // ---------------------------------------------------------------
        // Layer array: 4 solid HDR colours (slightly > 1 so tonemapping
        // has something to do). Red / green / blue / warm-yellow.
        // ---------------------------------------------------------------
        ScopedGLTexture layerArrayGuard;
        ::glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &layerArrayGuard.m_Id);
        const GLuint layerArray = layerArrayGuard.m_Id;
        ::glTextureStorage3D(layerArray, 1, GL_RGBA16F,
                             static_cast<GLsizei>(kSize), static_cast<GLsizei>(kSize), 4);
        ::glTextureParameteri(layerArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        ::glTextureParameteri(layerArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ::glTextureParameteri(layerArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(layerArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        const f32 kLayerColors[4][4] = {
            { 1.8f, 0.25f, 0.2f, 1.0f },  // layer 0 — red rock
            { 0.2f, 1.5f, 0.3f, 1.0f },   // layer 1 — grass
            { 0.15f, 0.35f, 1.6f, 1.0f }, // layer 2 — water-ish blue
            { 1.7f, 1.4f, 0.25f, 1.0f },  // layer 3 — warm sand
        };
        std::vector<f32> layerPixels(static_cast<std::size_t>(kSize) * kSize * 4);
        for (u32 layer = 0; layer < 4; ++layer)
        {
            for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
            {
                for (u32 c = 0; c < 4; ++c)
                    layerPixels[i * 4 + c] = kLayerColors[layer][c];
            }
            ::glTextureSubImage3D(layerArray, 0, 0, 0, static_cast<GLint>(layer),
                                  static_cast<GLsizei>(kSize), static_cast<GLsizei>(kSize), 1,
                                  GL_RGBA, GL_FLOAT, layerPixels.data());
        }
        ::glBindTextureUnit(20, layerArray);

        // ---------------------------------------------------------------
        // Splatmap: spatially varying weights. Radial gradient from centre
        // selects different layers at different distances, with a diagonal
        // phase shift so all four channels participate meaningfully.
        // Weights always sum to 1.0 per texel (normalised) so no layer is
        // implicitly doubled.
        // ---------------------------------------------------------------
        std::vector<f32> splatPixels(static_cast<std::size_t>(kSize) * kSize * 4);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kSize);
                const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kSize);
                f32 w0 = std::max(0.0f, 1.0f - 2.0f * u);
                f32 w1 = std::max(0.0f, 1.0f - 2.0f * v);
                f32 w2 = std::max(0.0f, 2.0f * u - 1.0f);
                f32 w3 = std::max(0.0f, 2.0f * v - 1.0f);
                const f32 sum = w0 + w1 + w2 + w3;
                if (sum > 0.0f)
                {
                    w0 /= sum;
                    w1 /= sum;
                    w2 /= sum;
                    w3 /= sum;
                }
                else
                {
                    w0 = 1.0f;
                }
                const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
                splatPixels[i + 0] = w0;
                splatPixels[i + 1] = w1;
                splatPixels[i + 2] = w2;
                splatPixels[i + 3] = w3;
            }
        }
        GLuint splatTex = CreateFloatTexture2D(kSize, kSize, splatPixels.data());
        ScopedGLTexture splatTexGuard(splatTex);
        ::glBindTextureUnit(24, splatTex);

        // ---------------------------------------------------------------
        // Terrain blend pass: HDR RGBA16F output.
        // ---------------------------------------------------------------
        FramebufferSpecification hdrSpec{};
        hdrSpec.Width = kSize;
        hdrSpec.Height = kSize;
        hdrSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
        Ref<Framebuffer> hdrFB = Framebuffer::Create(hdrSpec);
        ASSERT_TRUE(hdrFB != nullptr) << "Framebuffer::Create returned null for terrain HDR FBO";

        Ref<Shader> splatShader = Shader::Create(
            "assets/shaders/tests/ShaderUnit_SplatmapChannel.glsl");
        ASSERT_TRUE(splatShader != nullptr)
            << "Failed to load ShaderUnit_SplatmapChannel.glsl — check asset path";

        FullscreenPass fullscreen;
        hdrFB->Bind();
        ::glViewport(0, 0, static_cast<GLsizei>(kSize), static_cast<GLsizei>(kSize));
        ::glDisable(GL_BLEND);
        ::glDisable(GL_DEPTH_TEST);
        ::glDisable(GL_CULL_FACE);
        splatShader->Bind();
        fullscreen.Draw(0);
        ::glFinish();
        hdrFB->Unbind();

        // ---------------------------------------------------------------
        // Tone-map pass.
        // ---------------------------------------------------------------
        std::vector<f32> hdrReadback;
        ReadbackRgbaFloat(hdrFB->GetColorAttachmentRendererID(0), kSize, kSize, hdrReadback);
        // Defensive contract: see the shadow-golden test above.
        ASSERT_EQ(hdrReadback.size(), static_cast<std::size_t>(kSize) * kSize * 4)
            << "HDR readback size mismatch for splatmap pass";

        PostProcessUBOData toneUbo = MakeDefaultPostProcessUBO(kSize, kSize);
        toneUbo.TonemapOperator = 1; // Reinhard
        toneUbo.Exposure = 1.0f;
        toneUbo.Gamma = 2.2f;

        PostProcessHarness tone(kSize, kSize,
                                "assets/shaders/PostProcess_ToneMap.glsl", toneUbo);
        tone.SetInputTexture(CreateFloatTexture2D(kSize, kSize, hdrReadback.data()));
        tone.Draw();

        std::vector<u8> output;
        tone.ReadOutputRgba8(output);
        ASSERT_EQ(output.size(), static_cast<std::size_t>(kSize) * kSize * 4);

        // splatTex / layerArray are freed automatically by the ScopedGL* guards.

        const auto result = CompareOrBootstrap("scene_splatmap_integration", kSize, kSize, output);
        EXPECT_TRUE(result.m_Passed) << result.m_Message;
        if (result.m_Passed)
        {
            RecordProperty("result", result.m_Message);
        }
    }
} // namespace OloEngine::Tests
