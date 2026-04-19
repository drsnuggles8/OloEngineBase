// =============================================================================
// GoldenImageTests.cpp
//
// Layer 8 — Golden image comparison. End-to-end integration smoke test.
//
// Each test renders a deterministic procedural frame through production
// shader code (tone map, FXAA, etc.) at a fixed resolution and compares the
// RGBA8 readback against a baseline PNG stored in
// `OloEditor/assets/tests/golden/`.
//
// First run bootstrap
// -------------------
//   If the baseline PNG does not exist (or the env var
//   OLOENGINE_GOLDEN_REBASE is set to a truthy value), the test writes the
//   current output as the new baseline and passes. This is how baselines are
//   initially captured and how they are intentionally updated after a
//   validated shader change.
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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

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
                m_Ubo->SetData(&uboData, PostProcessUBOData::GetSize());
            }

            ~PostProcessHarness()
            {
                if (m_InputTex)
                    ::glDeleteTextures(1, &m_InputTex);
            }

            void SetInputTexture(u32 tex)
            {
                m_InputTex = tex;
            }

            void Draw()
            {
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
                ReadbackRgba8(m_OutputFB->GetColorAttachmentRendererID(0), m_Width, m_Height, out);
            }
        };

        // Baseline images live next to shader assets so OloEditor's asset
        // hot-reload is never confused by them.
        static fs::path GoldenBaselineDir()
        {
            return fs::path("assets") / "tests" / "golden";
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

        // Writes / reads baselines, performs comparison, and writes a diff
        // visualisation on failure.
        struct GoldenImageCheckResult
        {
            bool m_Passed = false;
            f32 m_Rmse = 0.0f;
            std::string m_Message;
        };

        static GoldenImageCheckResult CompareOrBootstrap(const std::string& name, u32 width, u32 height,
                                                         const std::vector<u8>& actualRgba)
        {
            GoldenImageCheckResult result{};

            fs::path dir = GoldenBaselineDir();
            std::error_code ec;
            fs::create_directories(dir, ec);
            fs::path baselinePath = dir / (name + ".png");

            const bool rebase = ShouldRebase();
            const bool baselineExists = fs::exists(baselinePath);

            if (!baselineExists || rebase)
            {
                // Bootstrap: write current output as the new baseline.
                const int ok = ::stbi_write_png(baselinePath.string().c_str(),
                                                static_cast<int>(width), static_cast<int>(height),
                                                4, actualRgba.data(), static_cast<int>(width) * 4);
                if (ok == 0)
                {
                    result.m_Message = "failed to write baseline PNG to " + baselinePath.string();
                    return result;
                }
                result.m_Passed = true;
                result.m_Message = std::string(rebase ? "REBASED" : "BOOTSTRAPPED") + " baseline at " + baselinePath.string();
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

            // Generous threshold — golden tests are integration smoke, not
            // pixel-exact regression guards. ~2 LSBs per channel over RGB.
            constexpr f32 kRmseThreshold = 0.008f;
            result.m_Passed = result.m_Rmse <= kRmseThreshold;

            if (!result.m_Passed)
            {
                // Write the actual image side-by-side for debugging.
                fs::path actualPath = dir / (name + ".actual.png");
                ::stbi_write_png(actualPath.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                                 4, actualRgba.data(), static_cast<int>(width) * 4);
                result.m_Message = "RMSE " + std::to_string(result.m_Rmse) +
                                   " exceeds threshold " + std::to_string(kRmseThreshold) +
                                   "; wrote actual frame to " + actualPath.string();
            }
            else
            {
                result.m_Message = "RMSE " + std::to_string(result.m_Rmse) + " within threshold";
            }
            return result;
        }
    } // namespace

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
} // namespace OloEngine::Tests
