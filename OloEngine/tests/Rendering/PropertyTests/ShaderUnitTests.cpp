// =============================================================================
// ShaderUnitTests.cpp
//
// Layer-2 of the renderer pyramid (docs/testing.md §6.3 — L2):
// compute-shader harnesses that exercise individual shader math functions on
// the actual GPU. Complements the Layer-1 property tests by catching
// math-level bugs (clamping, endpoints, round-trip error) before they
// propagate into a full pipeline.
//
// Pattern: production shader functions are lifted into a test-only compute
// shader (under OloEditor/assets/shaders/tests/), driven by an SSBO of
// inputs and writing to an SSBO of outputs. The test side dispatches,
// reads back, and verifies numerical invariants.
//
// First test: sRGB ↔ linear round-trip (every float in [0,1] should survive
// encode→decode within 1 LSB).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "VirtualRasterCoverageMirror.h"

#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Plain RAII holder for a pair of SSBOs. Keeps the test code below
        // readable. Each buffer is its own binding point (0 = in, 1 = out).
        struct ComputeBuffers
        {
            GLuint m_In = 0;
            GLuint m_Out = 0;

            ComputeBuffers(std::size_t inBytes, std::size_t outBytes)
            {
                ::glCreateBuffers(1, &m_In);
                ::glNamedBufferStorage(m_In, static_cast<GLsizeiptr>(inBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
                ::glCreateBuffers(1, &m_Out);
                ::glNamedBufferStorage(m_Out, static_cast<GLsizeiptr>(outBytes), nullptr,
                                       GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
            }
            ~ComputeBuffers()
            {
                if (m_In)
                    ::glDeleteBuffers(1, &m_In);
                if (m_Out)
                    ::glDeleteBuffers(1, &m_Out);
            }
            ComputeBuffers(const ComputeBuffers&) = delete;
            ComputeBuffers& operator=(const ComputeBuffers&) = delete;
        };

        struct ScopedBuffer
        {
            GLuint m_Id = 0;

            ScopedBuffer(GLsizeiptr byteSize, GLbitfield flags)
            {
                ::glCreateBuffers(1, &m_Id);
                ::glNamedBufferStorage(m_Id, byteSize, nullptr, flags);
            }

            ~ScopedBuffer()
            {
                if (m_Id != 0)
                    ::glDeleteBuffers(1, &m_Id);
            }

            ScopedBuffer(const ScopedBuffer&) = delete;
            ScopedBuffer& operator=(const ScopedBuffer&) = delete;

            operator GLuint() const
            {
                return m_Id;
            }
        };

        // A float texture for a shader probe to sample. The format and wrap
        // arguments default to what every caller before issue #903 used
        // (R32F fed from GL_RED, and the GL default GL_REPEAT this ctor used
        // to inherit by not setting wrap at all), so adding them changed no
        // existing call site. Filtering stays NEAREST for every caller: a
        // probe wants the texel it asked for, not a blend of its neighbours.
        struct ScopedTexture2D
        {
            GLuint m_Id = 0;

            ScopedTexture2D(u32 width, u32 height, const f32* pixels, GLenum internalFormat = GL_R32F,
                            GLenum uploadFormat = GL_RED, GLenum wrapMode = GL_REPEAT)
            {
                ::glCreateTextures(GL_TEXTURE_2D, 1, &m_Id);
                ::glTextureStorage2D(m_Id, 1, internalFormat, static_cast<GLsizei>(width),
                                     static_cast<GLsizei>(height));
                ::glTextureSubImage2D(m_Id, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                                      uploadFormat, GL_FLOAT, pixels);
                ::glTextureParameteri(m_Id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                ::glTextureParameteri(m_Id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                ::glTextureParameteri(m_Id, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapMode));
                ::glTextureParameteri(m_Id, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapMode));
            }

            ~ScopedTexture2D()
            {
                if (m_Id != 0)
                    ::glDeleteTextures(1, &m_Id);
            }

            ScopedTexture2D(const ScopedTexture2D&) = delete;
            ScopedTexture2D& operator=(const ScopedTexture2D&) = delete;

            operator GLuint() const
            {
                return m_Id;
            }
        };

        struct CloudDepthProbeSample
        {
            f32 Depth;
            f32 ContainsGeometry;
        };
        static_assert(sizeof(CloudDepthProbeSample) == 2 * sizeof(f32));

        std::vector<CloudDepthProbeSample> RunCloudDepthProbe(u32 width, u32 height,
                                                              const std::vector<f32>& depthPixels)
        {
            const auto expectedPixelCount = static_cast<sizet>(width) * height;
            if (depthPixels.size() != expectedPixelCount)
            {
                ADD_FAILURE() << "cloud depth probe expected " << expectedPixelCount
                              << " depth pixels, got " << depthPixels.size();
                return {};
            }

            const u32 halfWidth = (width + 1u) / 2u;
            const u32 halfHeight = (height + 1u) / 2u;
            std::vector<CloudDepthProbeSample> output(static_cast<sizet>(halfWidth) * halfHeight);

            ScopedTexture2D depthTexture(width, height, depthPixels.data());
            ScopedBuffer outputBuffer(static_cast<GLsizeiptr>(output.size() * sizeof(CloudDepthProbeSample)),
                                      GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);

            auto shader = ComputeShader::Create("assets/shaders/tests/ShaderUnit_CloudscapeDepth.glsl");
            if (!shader || !shader->IsValid())
            {
                ADD_FAILURE() << "cloud depth compute shader failed to compile";
                return {};
            }

            shader->Bind();
            ::glBindTextureUnit(0, depthTexture);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, outputBuffer);
            ::glDispatchCompute(halfWidth, halfHeight, 1);
            ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            ::glGetNamedBufferSubData(outputBuffer, 0,
                                      static_cast<GLsizeiptr>(output.size() * sizeof(CloudDepthProbeSample)),
                                      output.data());

            ::glBindTextureUnit(0, 0);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            ::glUseProgram(0);
            return output;
        }
    } // namespace

    TEST(ShaderUnitCloudDepthTest, MixedFootprintUsesMinimumGeometryDepth)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 4;
        constexpr u32 kHeight = 2;
        std::vector<f32> depthPixels(kWidth * kHeight, 1.0f);
        depthPixels[0] = 0.42f;

        const auto output = RunCloudDepthProbe(kWidth, kHeight, depthPixels);
        ASSERT_EQ(output.size(), 2u);
        EXPECT_FLOAT_EQ(output[0].Depth, 0.42f);
        EXPECT_FLOAT_EQ(output[1].Depth, 1.0f);
    }

    TEST(ShaderUnitCloudDepthTest, OddExtentClampsTheFinalFootprint)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 5;
        constexpr u32 kHeight = 3;
        std::vector<f32> depthPixels(kWidth * kHeight, 1.0f);
        depthPixels.back() = 0.625f;

        const auto output = RunCloudDepthProbe(kWidth, kHeight, depthPixels);
        ASSERT_EQ(output.size(), 6u);
        EXPECT_FLOAT_EQ(output.back().Depth, 0.625f);
    }

    TEST(ShaderUnitCloudDepthTest, ExactClearSentinelAloneClassifiesAsSky)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 4;
        constexpr u32 kHeight = 2;
        std::vector<f32> depthPixels(kWidth * kHeight, 1.0f);
        for (u32 y = 0; y < kHeight; ++y)
        {
            depthPixels[static_cast<sizet>(y) * kWidth] = 0.99995f;
            depthPixels[static_cast<sizet>(y) * kWidth + 1u] = 0.99995f;
        }

        const auto output = RunCloudDepthProbe(kWidth, kHeight, depthPixels);
        ASSERT_EQ(output.size(), 2u);
        EXPECT_FLOAT_EQ(output[0].ContainsGeometry, 1.0f);
        EXPECT_FLOAT_EQ(output[1].ContainsGeometry, 0.0f);
    }

    // =========================================================================
    // sRGB ↔ linear round-trip. For every input in [0, 1], linear→sRGB→linear
    // must be within ~1 LSB of the original (1/255 tolerance). Endpoints must
    // be preserved exactly.
    // =========================================================================
    TEST(ShaderUnitSrgbTest, RoundTripWithinOneLsb)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // 256 samples across [0, 1].
        constexpr u32 kN = 256;
        std::vector<f32> inputs(kN);
        for (u32 i = 0; i < kN; ++i)
            inputs[i] = static_cast<f32>(i) / static_cast<f32>(kN - 1);

        struct OutputPair
        {
            f32 sRGB;
            f32 decoded;
        };

        ComputeBuffers buffers(inputs.size() * sizeof(f32), kN * sizeof(OutputPair));
        ::glNamedBufferSubData(buffers.m_In, 0,
                               static_cast<GLsizeiptr>(inputs.size() * sizeof(f32)), inputs.data());

        auto cs = ComputeShader::Create("assets/shaders/tests/ShaderUnit_SrgbRoundTrip.glsl");
        ASSERT_TRUE(cs && cs->IsValid()) << "compute shader failed to compile";
        cs->Bind();

        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers.m_In);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers.m_Out);

        constexpr u32 kLocalSize = 64;
        ::glDispatchCompute((kN + kLocalSize - 1) / kLocalSize, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::vector<OutputPair> outputs(kN);
        ::glGetNamedBufferSubData(buffers.m_Out, 0,
                                  static_cast<GLsizeiptr>(outputs.size() * sizeof(OutputPair)), outputs.data());

        // Endpoints: 0 → 0, 1 → 1.
        EXPECT_NEAR(outputs[0].sRGB, 0.0f, 1e-6f);
        EXPECT_NEAR(outputs[0].decoded, 0.0f, 1e-6f);
        EXPECT_NEAR(outputs[kN - 1].sRGB, 1.0f, 1e-5f);
        EXPECT_NEAR(outputs[kN - 1].decoded, 1.0f, 1e-4f);

        // Round-trip error within 1 LSB of 8-bit sRGB (1/255 ≈ 0.004).
        f32 maxErr = 0.0f;
        for (u32 i = 0; i < kN; ++i)
        {
            const f32 err = std::abs(outputs[i].decoded - inputs[i]);
            maxErr = std::max(maxErr, err);
        }
        constexpr f32 kOneLsb = 1.0f / 255.0f;
        EXPECT_LT(maxErr, kOneLsb) << "max sRGB round-trip error = " << maxErr
                                   << " (allowed " << kOneLsb << ")";

        // Monotonicity: sRGB encode must be non-decreasing in its input.
        u32 monotoneViolations = 0;
        for (u32 i = 1; i < kN; ++i)
        {
            if (outputs[i].sRGB + 1e-6f < outputs[i - 1].sRGB)
                ++monotoneViolations;
        }
        EXPECT_EQ(monotoneViolations, 0u) << "LinearToSrgb must be monotonically non-decreasing";
    }

    // =========================================================================
    // BRDF LUT generation smoke test. Fully metallic PBR surfaces rely on the
    // split-sum BRDF LUT for specular IBL. A degenerate fullscreen primitive or
    // wrong vertex attribute layout silently produces an all-zero RG32F LUT,
    // which removes the entire specular IBL term and turns metals black.
    // =========================================================================
    TEST(ShaderUnitIBLTest, BRDFLutGenerationProducesNonZeroSplitSum)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 32;

        ShaderLibrary shaderLibrary;
        auto shader = shaderLibrary.Load("BRDFLutGeneration", "assets/shaders/BRDFLutGeneration.glsl");
        ASSERT_TRUE(shader != nullptr) << "Failed to load BRDFLutGeneration shader";

        TextureSpecification spec{};
        spec.Width = kSize;
        spec.Height = kSize;
        spec.Format = ImageFormat::RG32F;
        spec.GenerateMips = false;
        auto lut = Texture2D::Create(spec);
        ASSERT_TRUE(lut != nullptr) << "Texture2D::Create returned null for BRDF LUT";
        const auto cleanupLut = [&lut]()
        {
            lut.Reset();
            MeshPrimitives::Shutdown();
            FrameResourceManager::Get().FlushAllDeletionQueues();
        };

        IBLPrecompute::GenerateBRDFLut(lut, shaderLibrary);
        ::glFinish();

        std::vector<u8> bytes;
        if (!lut->GetData(bytes, 0))
        {
            cleanupLut();
            FAIL() << "BRDF LUT readback failed";
        }
        if (bytes.size() != static_cast<std::size_t>(kSize) * kSize * 2 * sizeof(f32))
        {
            cleanupLut();
            FAIL() << "BRDF LUT readback size mismatch";
        }

        f32 maxA = 0.0f;
        f32 maxB = 0.0f;
        f64 sum = 0.0;
        u32 invalidCount = 0;
        const auto sampleCount = bytes.size() / (2 * sizeof(f32));
        for (std::size_t i = 0; i < sampleCount; ++i)
        {
            f32 a = 0.0f;
            f32 b = 0.0f;
            std::memcpy(&a, bytes.data() + i * 2 * sizeof(f32), sizeof(f32));
            std::memcpy(&b, bytes.data() + (i * 2 + 1) * sizeof(f32), sizeof(f32));

            if (!std::isfinite(a) || !std::isfinite(b))
            {
                ++invalidCount;
                continue;
            }

            maxA = std::max(maxA, a);
            maxB = std::max(maxB, b);
            sum += static_cast<f64>(std::abs(a)) + static_cast<f64>(std::abs(b));
        }

        EXPECT_EQ(invalidCount, 0u) << "BRDF LUT contains NaN/Inf samples";
        EXPECT_GT(maxA, 0.1f) << "BRDF LUT A channel is unexpectedly dark";
        EXPECT_GT(maxB, 0.001f) << "BRDF LUT B channel is unexpectedly dark";
        EXPECT_GT(sum, 1.0) << "BRDF LUT payload is all zero";

        cleanupLut();
    }

    // =========================================================================
    // Visible skybox orientation: the authored Sandbox skybox has bright sky in
    // the top rows/top face and dark ground in the bottom rows/bottom face. The
    // production GetSkyboxSampleDirection helper must keep that relationship
    // intact when sampled through the actual GPU cubemap path.
    // =========================================================================
    TEST(ShaderUnitSkyboxTest, SamplingKeepsSkyAboveGround)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::vector<std::string> facePaths = {
            "assets/textures/Skybox/right.jpg",
            "assets/textures/Skybox/left.jpg",
            "assets/textures/Skybox/top.jpg",
            "assets/textures/Skybox/bottom.jpg",
            "assets/textures/Skybox/front.jpg",
            "assets/textures/Skybox/back.jpg",
        };

        for (const auto& facePath : facePaths)
        {
            std::error_code ec;
            ASSERT_TRUE(std::filesystem::exists(facePath, ec)) << "Missing skybox fixture: " << facePath;
            ASSERT_FALSE(ec) << "Failed to probe skybox fixture: " << facePath << ": " << ec.message();
        }

        Ref<TextureCubemap> skybox = TextureCubemap::Create(facePaths);
        ASSERT_TRUE(skybox != nullptr);
        ASSERT_TRUE(skybox->IsLoaded());

        struct OutputColor
        {
            f32 r = 0.0f;
            f32 g = 0.0f;
            f32 b = 0.0f;
            f32 a = 0.0f;
        };

        constexpr u32 kProbeCount = 4;
        ScopedBuffer outputBuffer(static_cast<GLsizeiptr>(sizeof(OutputColor) * kProbeCount),
                                  GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);

        auto cs = ComputeShader::Create("assets/shaders/tests/ShaderUnit_SkyboxOrientation.glsl");
        ASSERT_TRUE(cs && cs->IsValid()) << "skybox orientation compute shader failed to compile";
        cs->Bind();

        skybox->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outputBuffer);
        ::glDispatchCompute(kProbeCount, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        std::array<OutputColor, kProbeCount> outputs{};
        ::glGetNamedBufferSubData(outputBuffer, 0,
                                  static_cast<GLsizeiptr>(sizeof(OutputColor) * outputs.size()), outputs.data());

        auto luminance = [](const OutputColor& color) -> f32
        {
            return color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f;
        };

        const f32 screenTop = luminance(outputs[0]);
        const f32 screenBottom = luminance(outputs[1]);
        const f32 lookUp = luminance(outputs[2]);
        const f32 lookDown = luminance(outputs[3]);

        constexpr f32 kMinSkyGroundSeparation = 0.10f;
        EXPECT_GT(screenTop, screenBottom + kMinSkyGroundSeparation)
            << "Top-of-screen skybox sample should be brighter than bottom-of-screen sample; "
            << "this catches vertical cubemap inversions.";
        EXPECT_GT(lookUp, lookDown + kMinSkyGroundSeparation)
            << "Mostly-up skybox sample should be brighter than mostly-down sample; "
            << "this catches swapped +Y/-Y cubemap sampling.";

        skybox.Reset();
        FrameResourceManager::Get().FlushAllDeletionQueues();
    }

    // =========================================================================
    // sRGB midpoint anchor — linear 0.5 encodes to ≈0.7353585 per the
    // IEC 61966-2-1 transfer function. This catches off-by-constant bugs in
    // the piecewise formula.
    // =========================================================================
    TEST(ShaderUnitSrgbTest, MidpointMatchesReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<f32> inputs = { 0.0f, 0.0031308f, 0.5f, 1.0f };

        struct OutputPair
        {
            f32 sRGB;
            f32 decoded;
        };
        ComputeBuffers buffers(inputs.size() * sizeof(f32), inputs.size() * sizeof(OutputPair));
        ::glNamedBufferSubData(buffers.m_In, 0,
                               static_cast<GLsizeiptr>(inputs.size() * sizeof(f32)), inputs.data());

        auto cs = ComputeShader::Create("assets/shaders/tests/ShaderUnit_SrgbRoundTrip.glsl");
        ASSERT_TRUE(cs && cs->IsValid());
        cs->Bind();

        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers.m_In);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers.m_Out);
        ::glDispatchCompute(1, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::vector<OutputPair> outputs(inputs.size());
        ::glGetNamedBufferSubData(buffers.m_Out, 0,
                                  static_cast<GLsizeiptr>(outputs.size() * sizeof(OutputPair)), outputs.data());

        EXPECT_NEAR(outputs[0].sRGB, 0.0f, 1e-6f);
        // Boundary of the piecewise formula: 12.92 * 0.0031308 ≈ 0.04045.
        EXPECT_NEAR(outputs[1].sRGB, 0.04045f, 1e-4f);
        // 1.055 * pow(0.5, 1/2.4) - 0.055 ≈ 0.73535585.
        EXPECT_NEAR(outputs[2].sRGB, 0.73535585f, 1e-3f);
        EXPECT_NEAR(outputs[3].sRGB, 1.0f, 1e-5f);
    }

    // =========================================================================
    // Tone mapping reference values. For each operator and each reference HDR
    // input, the GPU shader output must match a hand-computed reference within
    // tight tolerance (not 1-LSB — single-float math here, no 8-bit quantize).
    // =========================================================================
    namespace
    {
        // CPU reference implementations — IDENTICAL math to the GLSL in
        // assets/shaders/tests/ShaderUnit_ToneMap.glsl. Deliberate duplication
        // so that shader edits without matching CPU edits produce a failure.

        // Reinhard (1/(x+1)) — Reinhard et al. 2002,
        // "Photographic Tone Reproduction for Digital Images",
        // https://www.cs.utah.edu/docs/techreports/2002/pdf/UUCS-02-001.pdf
        f32 ReinhardRef(f32 x)
        {
            return x / (x + 1.0f);
        }

        // ACES filmic approximation — Krzysztof Narkowicz, 2015,
        // "ACES Filmic Tone Mapping Curve",
        // https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
        // (fit of the full ACES RRT+ODT pipeline to a 5-constant rational curve).
        f32 AcesRef(f32 x)
        {
            constexpr f32 a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
            const f32 num = x * (a * x + b);
            const f32 den = x * (c * x + d) + e;
            return std::clamp(num / den, 0.0f, 1.0f);
        }

        // Uncharted 2 filmic — John Hable, 2010,
        // "Filmic Tonemapping Operators",
        // http://filmicworlds.com/blog/filmic-tonemapping-operators/
        // (constants A..F match Hable's reference shader verbatim).
        f32 Uncharted2Ref(f32 x)
        {
            constexpr f32 A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
            return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
        }

        // Run N rgb triples through the tone-map compute shader and return the
        // mapped triples.
        std::vector<f32> RunToneMap(int op, const std::vector<f32>& rgbInputs)
        {
            ComputeBuffers buffers(rgbInputs.size() * sizeof(f32), rgbInputs.size() * sizeof(f32));
            ::glNamedBufferSubData(buffers.m_In, 0,
                                   static_cast<GLsizeiptr>(rgbInputs.size() * sizeof(f32)), rgbInputs.data());

            auto cs = ComputeShader::Create("assets/shaders/tests/ShaderUnit_ToneMap.glsl");
            if (!cs || !cs->IsValid())
            {
                ADD_FAILURE() << "ShaderUnit_ToneMap compute shader failed to compile/link";
                return {};
            }
            cs->Bind();
            cs->SetInt("u_Op", op);

            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers.m_In);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers.m_Out);
            const u32 triples = static_cast<u32>(rgbInputs.size() / 3);
            const u32 kLocal = 64;
            ::glDispatchCompute((triples + kLocal - 1) / kLocal, 1, 1);
            ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            std::vector<f32> out(rgbInputs.size());
            ::glGetNamedBufferSubData(buffers.m_Out, 0,
                                      static_cast<GLsizeiptr>(out.size() * sizeof(f32)), out.data());
            return out;
        }
    } // namespace

    TEST(ShaderUnitToneMapTest, ReinhardMatchesReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<f32> inputs = {
            0.0f,
            0.0f,
            0.0f,
            0.5f,
            0.5f,
            0.5f,
            1.0f,
            1.0f,
            1.0f,
            4.0f,
            2.0f,
            1.0f,
            100.0f,
            100.0f,
            100.0f, // very bright — should converge to 1
        };
        const auto mapped = RunToneMap(0, inputs);
        ASSERT_EQ(mapped.size(), inputs.size());

        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const f32 expected = ReinhardRef(inputs[i]);
            EXPECT_NEAR(mapped[i], expected, 1e-5f)
                << "Reinhard mismatch at channel " << i << " (input=" << inputs[i] << ")";
        }
        // Very bright → 100/101 ≈ 0.9901
        EXPECT_NEAR(mapped[12], 100.0f / 101.0f, 1e-5f);
    }

    TEST(ShaderUnitToneMapTest, AcesMatchesReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<f32> inputs = {
            0.0f,
            0.0f,
            0.0f,
            0.25f,
            0.25f,
            0.25f,
            1.0f,
            1.0f,
            1.0f,
            2.0f,
            1.5f,
            0.5f,
            50.0f,
            50.0f,
            50.0f,
        };
        const auto mapped = RunToneMap(1, inputs);
        ASSERT_EQ(mapped.size(), inputs.size());

        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const f32 expected = AcesRef(inputs[i]);
            EXPECT_NEAR(mapped[i], expected, 1e-5f)
                << "ACES mismatch at channel " << i << " (input=" << inputs[i] << ")";
        }
        // ACES must clamp to [0, 1] for any input.
        for (f32 v : mapped)
        {
            EXPECT_GE(v, 0.0f);
            EXPECT_LE(v, 1.0f);
        }
    }

    TEST(ShaderUnitToneMapTest, Uncharted2MatchesReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Uncharted2 is NOT normalized — values > 1 pass through the curve
        // unchanged at their input scale. Output is later divided by
        // Uncharted2(white_point) in the production shader to bring it to
        // [0,1]; the raw operator here is tested against reference directly.
        std::vector<f32> inputs = {
            0.0f,
            0.0f,
            0.0f,
            0.5f,
            0.5f,
            0.5f,
            1.0f,
            1.0f,
            1.0f,
            11.2f,
            11.2f,
            11.2f, // the canonical Hable white point
        };
        const auto mapped = RunToneMap(2, inputs);
        ASSERT_EQ(mapped.size(), inputs.size());

        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const f32 expected = Uncharted2Ref(inputs[i]);
            EXPECT_NEAR(mapped[i], expected, 1e-4f)
                << "Uncharted2 mismatch at channel " << i << " (input=" << inputs[i] << ")";
        }
        // Black in → black out (offset term E/F is subtracted to zero).
        EXPECT_NEAR(mapped[0], 0.0f, 1e-5f);
    }

    // =========================================================================
    // GGX NDF hemisphere integral: ∫ D(h) * cos(θ_h) dω = 1. Standard
    // normalization test for any normal distribution function. Uses a
    // stratified-grid midpoint quadrature on (θ, φ). Discretization error
    // bounds the achievable tolerance.
    // =========================================================================
    TEST(ShaderUnitGgxTest, HemisphereIntegralIsOne)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Grid size: 256 × 128 = 32k cells, stratified midpoint quadrature.
        // Valid range for this resolution: roughness ∈ [0.25, 1.0]. Lower
        // roughness values produce a needle-thin specular lobe near θ=0 that
        // the grid undersamples. A uniform midpoint rule is the wrong tool
        // for roughness < 0.1; importance sampling would be required and is
        // a separate test (not implemented here). The important regression
        // signal is "integral off by 0.5 or more" (missing 2π factor, wrong
        // Jacobian, etc.), which this test catches reliably.
        constexpr int kTheta = 256;
        constexpr int kPhi = 128;

        auto cs = ComputeShader::Create("assets/shaders/tests/ShaderUnit_GgxIntegral.glsl");
        ASSERT_TRUE(cs && cs->IsValid());
        cs->Bind();
        cs->SetInt("u_ThetaSteps", kTheta);
        cs->SetInt("u_PhiSteps", kPhi);

        for (f32 roughness : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            std::vector<f32> inputs = { roughness };
            const std::size_t cellCount = static_cast<std::size_t>(kTheta) * kPhi;
            ComputeBuffers buffers(inputs.size() * sizeof(f32), cellCount * sizeof(f32));
            ::glNamedBufferSubData(buffers.m_In, 0,
                                   static_cast<GLsizeiptr>(inputs.size() * sizeof(f32)), inputs.data());

            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers.m_In);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers.m_Out);

            constexpr u32 kLocal = 8;
            // Ceiling division so we always cover the grid, even if kTheta /
            // kPhi stop being exact multiples of kLocal. Consistent with the
            // other dispatches in this file.
            ::glDispatchCompute((kTheta + kLocal - 1) / kLocal, (kPhi + kLocal - 1) / kLocal, 1);
            ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            std::vector<f32> cells(cellCount);
            ::glGetNamedBufferSubData(buffers.m_Out, 0,
                                      static_cast<GLsizeiptr>(cells.size() * sizeof(f32)), cells.data());

            // Sum in double precision to avoid catastrophic cancellation.
            f64 integral = 0.0;
            for (f32 v : cells)
                integral += static_cast<f64>(v);

            // Midpoint-rule hemisphere integration error grows as roughness
            // shrinks (tighter lobe). 2% tolerance covers all sampled rough-
            // nesses; the interesting failure is "off by 0.5" (missing 2π
            // factor, etc.), not "off by 0.005".
            EXPECT_NEAR(integral, 1.0, 0.02)
                << "GGX hemisphere integral at roughness=" << roughness << " is " << integral;
        }
    }

    // =========================================================================
    // Octahedral normal encode/decode round-trip. For any unit normal on the
    // sphere, decode(encode(n)) must equal n within fp32 precision. Catches
    // sign-flip bugs in the encode/decode asymmetry for the lower hemisphere
    // (where the Z<0 branch of octEncode flips the sign of x and y).
    // =========================================================================
    TEST(ShaderUnitOctNormalTest, RoundTripPreservesUnitNormals)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Deterministic fibonacci-like sphere sampling for good sphere coverage
        // without depending on std::random (which differs between stdlibs).
        constexpr int kCount = 256;
        std::vector<f32> inputs(static_cast<std::size_t>(kCount) * 3);
        constexpr f64 kGoldenAngle = 2.399963229728653; // π * (3 - sqrt(5))
        for (int i = 0; i < kCount; ++i)
        {
            const f64 t = (static_cast<f64>(i) + 0.5) / kCount;
            const f64 z = 1.0 - 2.0 * t; // [-1, 1]
            const f64 r = std::sqrt(std::max(0.0, 1.0 - z * z));
            const f64 phi = static_cast<f64>(i) * kGoldenAngle;
            inputs[i * 3 + 0] = static_cast<f32>(r * std::cos(phi));
            inputs[i * 3 + 1] = static_cast<f32>(r * std::sin(phi));
            inputs[i * 3 + 2] = static_cast<f32>(z);
        }

        ComputeBuffers buffers(inputs.size() * sizeof(f32), inputs.size() * sizeof(f32));
        ::glNamedBufferSubData(buffers.m_In, 0,
                               static_cast<GLsizeiptr>(inputs.size() * sizeof(f32)), inputs.data());

        auto cs = ComputeShader::Create("assets/shaders/tests/ShaderUnit_OctNormal.glsl");
        ASSERT_TRUE(cs && cs->IsValid());
        cs->Bind();

        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers.m_In);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers.m_Out);
        constexpr u32 kLocal = 64;
        const u32 triples = static_cast<u32>(inputs.size() / 3);
        ::glDispatchCompute((triples + kLocal - 1) / kLocal, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::vector<f32> output(inputs.size());
        ::glGetNamedBufferSubData(buffers.m_Out, 0,
                                  static_cast<GLsizeiptr>(output.size() * sizeof(f32)), output.data());

        // Max per-component drift must be small. Octahedral round-trip through
        // fp32 arithmetic is typically exact to ~1e-6; 1e-5 gives margin for
        // sign-handling near axis-aligned normals.
        u32 violations = 0;
        f32 maxDrift = 0.0f;
        for (int i = 0; i < kCount; ++i)
        {
            const f32 dx = std::abs(inputs[i * 3 + 0] - output[i * 3 + 0]);
            const f32 dy = std::abs(inputs[i * 3 + 1] - output[i * 3 + 1]);
            const f32 dz = std::abs(inputs[i * 3 + 2] - output[i * 3 + 2]);
            maxDrift = std::max({ maxDrift, dx, dy, dz });
            if (dx > 1e-5f || dy > 1e-5f || dz > 1e-5f)
                ++violations;
        }

        EXPECT_EQ(violations, 0u) << "Octahedral round-trip violated 1e-5 threshold for "
                                  << violations << " / " << kCount << " samples (max drift "
                                  << maxDrift << ")";
    }

    // =========================================================================
    // Distance-fog endpoint behavior. Catches "fog at zero distance" and
    // "fog at infinite distance" regressions in FogCommon.glsl. The test runs
    // the three production modes (linear, exponential, exponential-squared)
    // for a grid of distances and verifies the physical invariants.
    //
    // Covers two catalog items from docs/testing.md §6.3 (L1 properties):
    //   - Fog at zero distance     → factor == 0
    //   - Fog at infinite distance → factor == 1 (or saturates at fogEnd for linear)
    // =========================================================================
    TEST(ShaderUnitFogTest, EndpointInvariants)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Build test vectors: [mode, density, fogStart, fogEnd, dist]
        // One "zero distance" case per mode + several large-distance cases.
        struct Case
        {
            int mode;
            f32 density;
            f32 fogStart;
            f32 fogEnd;
            f32 dist;
        };
        const std::vector<Case> cases = {
            // Zero-distance: fog contribution must be exactly 0 in every mode.
            { 0 /* linear     */, 0.0f, 10.0f, 100.0f, 0.0f },
            { 1 /* exp        */, 0.05f, 0.0f, 0.0f, 0.0f },
            { 2 /* exp-squared*/, 0.05f, 0.0f, 0.0f, 0.0f },

            // Large-distance: exponential modes saturate to ~1. We pick a
            // distance such that density*dist = 25 so exp(-25) < 1e-10.
            { 1 /* exp        */, 0.05f, 0.0f, 0.0f, 500.0f },
            { 2 /* exp-squared*/, 0.1f, 0.0f, 0.0f, 50.0f }, // dd=5 → exp(-25)

            // Linear beyond fogEnd must clamp to exactly 1.
            { 0 /* linear     */, 0.0f, 10.0f, 100.0f, 200.0f },

            // Linear before fogStart must be exactly 0.
            { 0 /* linear     */, 0.0f, 10.0f, 100.0f, 5.0f },
        };

        std::vector<f32> inputs(cases.size() * 5);
        for (std::size_t i = 0; i < cases.size(); ++i)
        {
            inputs[i * 5 + 0] = static_cast<f32>(cases[i].mode);
            inputs[i * 5 + 1] = cases[i].density;
            inputs[i * 5 + 2] = cases[i].fogStart;
            inputs[i * 5 + 3] = cases[i].fogEnd;
            inputs[i * 5 + 4] = cases[i].dist;
        }

        ComputeBuffers buffers(inputs.size() * sizeof(f32), cases.size() * sizeof(f32));
        ::glNamedBufferSubData(buffers.m_In, 0,
                               static_cast<GLsizeiptr>(inputs.size() * sizeof(f32)), inputs.data());

        auto cs = ComputeShader::Create("assets/shaders/tests/ShaderUnit_Fog.glsl");
        ASSERT_TRUE(cs && cs->IsValid());
        cs->Bind();

        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers.m_In);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers.m_Out);
        constexpr u32 kLocal = 64;
        const u32 count = static_cast<u32>(cases.size());
        ::glDispatchCompute((count + kLocal - 1) / kLocal, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::vector<f32> output(cases.size());
        ::glGetNamedBufferSubData(buffers.m_Out, 0,
                                  static_cast<GLsizeiptr>(output.size() * sizeof(f32)), output.data());

        // Case 0-2: zero-distance → exactly 0.
        EXPECT_FLOAT_EQ(output[0], 0.0f) << "Linear fog at dist=0 must be 0";
        EXPECT_FLOAT_EQ(output[1], 0.0f) << "Exponential fog at dist=0 must be 0";
        EXPECT_FLOAT_EQ(output[2], 0.0f) << "Exp-sq fog at dist=0 must be 0";

        // Case 3-4: large distance → ≈ 1 within 1e-6.
        EXPECT_NEAR(output[3], 1.0f, 1e-6f) << "Exponential fog must saturate to 1 for density*dist=25";
        EXPECT_NEAR(output[4], 1.0f, 1e-6f) << "Exp-sq fog must saturate to 1 for (density*dist)^2=25";

        // Case 5: linear fog beyond fogEnd → exactly 1 (clamp).
        EXPECT_FLOAT_EQ(output[5], 1.0f) << "Linear fog beyond fogEnd must clamp to 1";

        // Case 6: linear fog before fogStart → exactly 0 (clamp).
        EXPECT_FLOAT_EQ(output[6], 0.0f) << "Linear fog before fogStart must clamp to 0";
    }
    // =========================================================================
    // Temporal resolve, RGBA entry point (issue #903)
    //
    // The cloudscape resolve moved onto include/TemporalResolve.glsl, whose
    // alpha half exists because transmittance is not a colour channel. What is
    // worth probing on the real GPU is not the arithmetic — the CPU mirror in
    // StochasticSamplerTest covers that — but OLO_TEMPORAL_GATHER_3X3_RGBA,
    // which fills TWO statistics structs from ONE nine-tap loop. The way that
    // breaks is a crossed accumulator, and a mirror of the functions it feeds
    // would never notice.
    // =========================================================================
    namespace
    {
        struct TemporalRgbaProbeResult
        {
            std::array<f32, 4> Rgba{};       // OloTemporalResolveRGBA
            std::array<f32, 4> RgbOnly{};    // OloTemporalResolve via the original macro
            std::array<f32, 4> AlphaStats{}; // mean, stddev, min, max
        };

        // `signal` is 9 RGBA texels; `cases` is one {history, (gamma, feedback,
        // confidence, 0)} pair per invocation.
        std::vector<TemporalRgbaProbeResult> RunTemporalRgbaProbe(
            const std::array<std::array<f32, 4>, 9>& signal,
            const std::vector<std::pair<std::array<f32, 4>, std::array<f32, 4>>>& cases)
        {
            std::vector<TemporalRgbaProbeResult> results(cases.size());

            std::vector<f32> signalPixels;
            signalPixels.reserve(9u * 4u);
            for (const auto& texel : signal)
                signalPixels.insert(signalPixels.end(), texel.begin(), texel.end());

            std::vector<f32> inputs;
            inputs.reserve(cases.size() * 8u);
            for (const auto& [history, params] : cases)
            {
                inputs.insert(inputs.end(), history.begin(), history.end());
                inputs.insert(inputs.end(), params.begin(), params.end());
            }

            const auto outputFloats = results.size() * 12u; // 3 vec4 per case
            // Row-major from the bottom-left texel (GL's origin), so index 4 is
            // the centre the probe samples. CLAMP_TO_EDGE is belt-and-braces:
            // the 3x3 gather stays inside the texture, so nothing wraps anyway.
            ScopedTexture2D signalTexture(3u, 3u, signalPixels.data(), GL_RGBA32F, GL_RGBA,
                                          GL_CLAMP_TO_EDGE);
            ScopedBuffer inputBuffer(static_cast<GLsizeiptr>(inputs.size() * sizeof(f32)),
                                     GL_DYNAMIC_STORAGE_BIT);
            ScopedBuffer outputBuffer(static_cast<GLsizeiptr>(outputFloats * sizeof(f32)),
                                      GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
            ::glNamedBufferSubData(inputBuffer, 0, static_cast<GLsizeiptr>(inputs.size() * sizeof(f32)),
                                   inputs.data());

            auto shader = ComputeShader::Create("assets/shaders/tests/ShaderUnit_TemporalResolveRGBA.glsl");
            if (!shader || !shader->IsValid())
            {
                ADD_FAILURE() << "temporal resolve RGBA compute shader failed to compile";
                return {};
            }

            shader->Bind();
            ::glBindTextureUnit(0, signalTexture);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, outputBuffer);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, inputBuffer);
            ::glDispatchCompute(static_cast<GLuint>(cases.size()), 1, 1);
            ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            std::vector<f32> raw(outputFloats);
            ::glGetNamedBufferSubData(outputBuffer, 0, static_cast<GLsizeiptr>(outputFloats * sizeof(f32)),
                                      raw.data());

            ::glBindTextureUnit(0, 0);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
            ::glUseProgram(0);

            for (std::size_t i = 0; i < results.size(); ++i)
            {
                const f32* base = raw.data() + i * 12u;
                std::memcpy(results[i].Rgba.data(), base, 4u * sizeof(f32));
                std::memcpy(results[i].RgbOnly.data(), base + 4u, 4u * sizeof(f32));
                std::memcpy(results[i].AlphaStats.data(), base + 8u, 4u * sizeof(f32));
            }
            return results;
        }

        // A signal whose COLOUR is uniform and whose ALPHA varies widely. If the
        // gather ever crosses its accumulators, this is the neighbourhood that
        // says so loudest: the colour box is degenerate and the alpha box is not.
        constexpr std::array<std::array<f32, 4>, 9> kFlatColourVariedAlpha = { {
            { { 0.5f, 0.5f, 0.5f, 0.0f } },
            { { 0.5f, 0.5f, 0.5f, 0.25f } },
            { { 0.5f, 0.5f, 0.5f, 0.5f } },
            { { 0.5f, 0.5f, 0.5f, 0.75f } },
            { { 0.5f, 0.5f, 0.5f, 1.0f } }, // centre
            { { 0.5f, 0.5f, 0.5f, 0.75f } },
            { { 0.5f, 0.5f, 0.5f, 0.5f } },
            { { 0.5f, 0.5f, 0.5f, 0.25f } },
            { { 0.5f, 0.5f, 0.5f, 0.0f } },
        } };

        // The mirror image: alpha uniform, colour varied.
        constexpr std::array<std::array<f32, 4>, 9> kVariedColourFlatAlpha = { {
            { { 0.0f, 0.1f, 0.9f, 0.6f } },
            { { 0.2f, 0.3f, 0.7f, 0.6f } },
            { { 0.4f, 0.5f, 0.5f, 0.6f } },
            { { 0.6f, 0.7f, 0.3f, 0.6f } },
            { { 0.8f, 0.9f, 0.1f, 0.6f } }, // centre
            { { 0.7f, 0.2f, 0.4f, 0.6f } },
            { { 0.1f, 0.8f, 0.6f, 0.6f } },
            { { 0.3f, 0.4f, 0.2f, 0.6f } },
            { { 0.9f, 0.0f, 0.8f, 0.6f } },
        } };
    } // namespace

    // The alpha statistics must come from the alpha channel and nothing else.
    // Hand-computed for kFlatColourVariedAlpha: the nine alphas are
    // {0, .25, .5, .75, 1, .75, .5, .25, 0}, mean 4.0/9.
    TEST(ShaderUnitTemporalResolveTest, AlphaStatisticsComeFromTheAlphaChannel)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto results = RunTemporalRgbaProbe(kFlatColourVariedAlpha,
                                                  { { { { 0.5f, 0.5f, 0.5f, 0.5f } }, { { 1.25f, 0.9f, 1.0f, 0.0f } } } });
        ASSERT_EQ(results.size(), 1u);

        constexpr f32 kExpectedMean = 4.0f / 9.0f;
        constexpr f32 kExpectedSecondMoment = (0.0f + 0.0625f + 0.25f + 0.5625f + 1.0f + 0.5625f + 0.25f + 0.0625f + 0.0f) / 9.0f;
        const f32 expectedStdDev = std::sqrt(kExpectedSecondMoment - kExpectedMean * kExpectedMean);

        EXPECT_NEAR(results[0].AlphaStats[0], kExpectedMean, 1e-5f) << "alpha mean";
        EXPECT_NEAR(results[0].AlphaStats[1], expectedStdDev, 1e-5f) << "alpha stddev";
        EXPECT_NEAR(results[0].AlphaStats[2], 0.0f, 1e-5f) << "alpha min";
        EXPECT_NEAR(results[0].AlphaStats[3], 1.0f, 1e-5f) << "alpha max";

        // And the colour box is degenerate on this signal, so any history colour
        // resolves to the neighbourhood's single colour — which is also the
        // current one. If alpha had leaked into the colour accumulators this
        // would not hold.
        EXPECT_NEAR(results[0].Rgba[0], 0.5f, 1e-5f);
        EXPECT_NEAR(results[0].Rgba[1], 0.5f, 1e-5f);
        EXPECT_NEAR(results[0].Rgba[2], 0.5f, 1e-5f);
    }

    // The reverse leak: a wide COLOUR spread must not widen the ALPHA box. With
    // alpha uniform at 0.6 the hard box is [0.6, 0.6], so a history alpha of
    // 0.05 must be clipped all the way back to 0.6 no matter how much room the
    // colour channels have.
    //
    // Note what the stddev is NOT asserted to be. `sqrt(E[x^2] - mean^2)` on a
    // FLAT channel does not return zero in fp32: one ULP of cancellation in the
    // subtraction (5.96e-8 for 0.6) becomes 2.4e-4 once the sqrt is taken,
    // because sqrt has unbounded slope at the origin. Measured here at 3.2e-4.
    // So the variance box on a flat neighbourhood is ~+/-4e-4 wide rather than
    // degenerate, and it is the INTERSECTION with the hard min/max that pins it
    // back to exactly [0.6, 0.6]. That intersection is load-bearing, not belt
    // and braces — which is the reason this test asserts the resolved value and
    // merely bounds the statistic.
    TEST(ShaderUnitTemporalResolveTest, ColourSpreadDoesNotWidenTheAlphaBox)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto results = RunTemporalRgbaProbe(kVariedColourFlatAlpha,
                                                  { { { { 0.8f, 0.9f, 0.1f, 0.05f } }, { { 1.25f, 0.9f, 1.0f, 0.0f } } } });
        ASSERT_EQ(results.size(), 1u);

        // Bounded, not zero — see above. A crossed accumulator would put the
        // COLOUR spread here, which on this signal is order 0.1, three orders
        // of magnitude above the cancellation floor.
        EXPECT_LT(results[0].AlphaStats[1], 1.0e-3f)
            << "a flat alpha channel reported a real spread — the gather is reading the colour accumulator";
        EXPECT_NEAR(results[0].AlphaStats[2], 0.6f, 1e-5f) << "alpha hard min";
        EXPECT_NEAR(results[0].AlphaStats[3], 0.6f, 1e-5f) << "alpha hard max";

        // The decisive one: the history alpha lands on the neighbourhood value.
        EXPECT_NEAR(results[0].Rgba[3], 0.6f, 1e-5f)
            << "the history alpha was not clipped to its own neighbourhood — the colour box widened it";
    }

    // The colour path must be bit-for-bit what it was before the RGBA macro
    // existed. Both gathers run in the same invocation over the same texels, so
    // any disagreement is the new macro's doing.
    //
    // OLO_TEMPORAL_GATHER_3X3 now DELEGATES to the RGBA one, so this is close to
    // a tautology on the current code and that is the point: it is the guard on
    // the delegation. The moment someone "optimises" the colour-only path back
    // into its own loop — the obvious change, since the alpha accumulators look
    // like waste to a reader who has not checked that they are dead stores —
    // this test is what says whether the copy still agrees.
    TEST(ShaderUnitTemporalResolveTest, TheRgbaGatherLeavesTheColourPathUnchanged)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<std::pair<std::array<f32, 4>, std::array<f32, 4>>> cases = {
            { { { 0.8f, 0.9f, 0.1f, 0.6f } }, { { 1.25f, 0.9f, 1.0f, 0.0f } } },   // history == current
            { { { 5.0f, -2.0f, 3.0f, 0.6f } }, { { 1.25f, 0.9f, 1.0f, 0.0f } } },  // far outside the box
            { { { 0.35f, 0.55f, 0.45f, 0.6f } }, { { 2.0f, 0.5f, 1.0f, 0.0f } } }, // looser gamma, lower feedback
        };

        for (const auto& signal : { kVariedColourFlatAlpha, kFlatColourVariedAlpha })
        {
            const auto results = RunTemporalRgbaProbe(signal, cases);
            ASSERT_EQ(results.size(), cases.size());
            for (std::size_t i = 0; i < results.size(); ++i)
            {
                for (std::size_t c = 0; c < 3u; ++c)
                {
                    EXPECT_NEAR(results[i].Rgba[c], results[i].RgbOnly[c], 1e-5f)
                        << "case " << i << ", channel " << c
                        << ": OLO_TEMPORAL_GATHER_3X3_RGBA disagreed with OLO_TEMPORAL_GATHER_3X3";
                }
            }
        }
    }

    // Confidence 0 is "there is no history here", and the only correct answer is
    // the current frame exactly — in all FOUR channels. The vec3 kernel could
    // not make that promise about alpha; this entry point can, and the
    // cloudscape's occlusion gate (#987) depends on it.
    TEST(ShaderUnitTemporalResolveTest, ZeroConfidenceReturnsTheCurrentTexelIncludingAlpha)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto results = RunTemporalRgbaProbe(kFlatColourVariedAlpha,
                                                  { { { { 9.0f, -4.0f, 7.0f, 0.0f } }, { { 1.25f, 0.98f, 0.0f, 0.0f } } } });
        ASSERT_EQ(results.size(), 1u);

        EXPECT_NEAR(results[0].Rgba[0], 0.5f, 1e-6f);
        EXPECT_NEAR(results[0].Rgba[1], 0.5f, 1e-6f);
        EXPECT_NEAR(results[0].Rgba[2], 0.5f, 1e-6f);
        EXPECT_NEAR(results[0].Rgba[3], 1.0f, 1e-6f) << "alpha ignored confidence";
    }

    TEST(ShaderUnitSurfaceHistoryTest, StableIdentityRejectionAndFirstFrameMomentsMatchTheSharedContract)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ProbeOutput
        {
            std::array<u32, 4> Reasons{};
            std::array<f32, 4> First{};
            std::array<f32, 4> Second{};
            std::array<f32, 4> Metadata{};
        };

        ScopedBuffer output(sizeof(ProbeOutput), GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
        auto shader = ComputeShader::Create("assets/shaders/tests/ShaderUnit_SurfaceHistory.glsl");
        ASSERT_TRUE(shader && shader->IsValid()) << "surface-history contract probe failed to compile";
        shader->Bind();
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, output.m_Id);
        ::glDispatchCompute(1, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        ProbeOutput result{};
        ::glGetNamedBufferSubData(output.m_Id, 0, sizeof(result), &result);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        ::glUseProgram(0);

        constexpr u32 kRejectInstance = 1u << 6u;
        constexpr u32 kRejectMaterial = 1u << 8u;
        constexpr u32 kRejectIdentityMissing = 1u << 14u;
        EXPECT_NE(result.Reasons[0] & kRejectInstance, 0u);
        EXPECT_NE(result.Reasons[1] & kRejectMaterial, 0u);
        EXPECT_NE(result.Reasons[2] & kRejectIdentityMissing, 0u);
        EXPECT_EQ(result.Reasons[3], 0u);

        const std::array<f32, 4> signal{ 0.25f, 0.5f, 0.75f, 1.0f };
        for (std::size_t channel = 0; channel < signal.size(); ++channel)
        {
            EXPECT_FLOAT_EQ(result.First[channel], signal[channel]);
            EXPECT_FLOAT_EQ(result.Second[channel], signal[channel] * signal[channel]);
        }
        EXPECT_FLOAT_EQ(result.Metadata[0], 1.0f);
        EXPECT_FLOAT_EQ(result.Metadata[1], 0.0f);
        EXPECT_FLOAT_EQ(result.Metadata[2], 0.0f);
    }

    TEST(ShaderUnitDepthAwareClusterTest, TileDepthAndPixelBoundaryHelpersMatchTheirContract)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kOutputCount = 9u;
        ScopedBuffer output(kOutputCount * sizeof(u32), GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
        auto shader = ComputeShader::Create("assets/shaders/tests/ShaderUnit_DepthAwareCluster.glsl");
        ASSERT_TRUE(shader && shader->IsValid()) << "depth-aware cluster probe failed to compile";

        shader->Bind();
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, output.m_Id);
        ::glDispatchCompute(1, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::array<u32, kOutputCount> values{};
        ::glGetNamedBufferSubData(output.m_Id, 0, sizeof(values), values.data());
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        shader->Unbind();

        // Expected values are derived from the CPU mirror in
        // ClusteredLighting.h, not duplicated magic constants.
        EXPECT_EQ(values[0], ClusteredLighting::DepthCellForViewDepth(2.0f, 2.0f, 18.0f));
        EXPECT_EQ(values[1], ClusteredLighting::DepthCellForViewDepth(10.0f, 2.0f, 18.0f));
        EXPECT_EQ(values[2], ClusteredLighting::DepthCellForViewDepth(18.0f, 2.0f, 18.0f));
        EXPECT_EQ(values[3], ClusteredLighting::DepthCellMaskForViewRange(4.9f, 5.1f, 0.0f, 32.0f));
        EXPECT_EQ(values[4], ClusteredLighting::TilePixelBoundary(1u, 32u, 1366u));
        EXPECT_EQ(values[5], ClusteredLighting::TileForPixelCenter(20u, 32u, 164u));
        const glm::mat4 identityProjection{ 1.0f };
        EXPECT_EQ(values[6], std::bit_cast<u32>(
                                 ClusteredLighting::ViewDepthFromDeviceDepth(0.25f, identityProjection)));
        const auto slicing = ClusteredLighting::ComputeDepthSliceParams(24u, 0.1f, 1000.0f);
        EXPECT_EQ(values[7], ClusteredLighting::SliceForViewDepth(0.1f, slicing, 24u));
        constexpr f32 sphereCenterZ = -5.0f;
        constexpr f32 sphereRadius = 0.25f;
        const bool sphereIntersects = ClusteredLighting::DepthRangeIntersectsMask(
            1u << 5u, -sphereCenterZ - sphereRadius, -sphereCenterZ + sphereRadius, 0.0f, 32.0f);
        EXPECT_EQ(values[8], static_cast<u32>(sphereIntersects));
    }

    // Layer-2 for the virtual-geometry raster's sub-sample-miss rule (issue
    // #712). The probe includes the SHIPPED VirtualRasterCoverage.glsl, so this
    // is what fails when the GLSL and the CPU mirror in
    // VirtualRasterCoverageMirror.h drift — the rule's own correctness is
    // pinned, without a GL context, by VirtualRasterCoverageTest.
    TEST(ShaderUnitVirtualSampleBoundsTest, SampleRangeAndAreaSignMatchTheCpuMirror)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        namespace Mirror = OloEngine::Tests::VirtualRasterCoverage;

        struct Case
        {
            glm::vec2 S0;
            glm::vec2 S1;
            glm::vec2 S2;
            glm::vec2 Viewport;
        };

        // Far outside the int range but still finite, and small enough that the
        // area determinant does not overflow either: an inf/NaN case here would
        // be testing the driver's fast-math flags, not the rule.
        constexpr f32 kHuge = 1.0e18f;
        // Coordinates are multiples of 1/64 (or plainly separated from a pixel
        // centre) so ceil/floor land on the same side on any float unit.
        const std::array<Case, 12> cases{ {
            // Wholly inside one pixel, missing its centre -> reject.
            { { 10.0625f, 10.0625f }, { 10.375f, 10.0625f }, { 10.0625f, 10.375f }, { 64.0f, 64.0f } },
            // Same triangle nudged onto the centre -> kept, one pixel wide.
            { { 10.4375f, 10.4375f }, { 10.75f, 10.4375f }, { 10.4375f, 10.75f }, { 64.0f, 64.0f } },
            // Vertices exactly on pixel centres: both bounds inclusive.
            { { 4.5f, 4.5f }, { 8.5f, 4.5f }, { 4.5f, 8.5f }, { 64.0f, 64.0f } },
            // A hair inside those centres: both boundary pixels drop out.
            { { 4.515625f, 4.515625f }, { 8.484375f, 4.515625f }, { 4.515625f, 8.484375f }, { 64.0f, 64.0f } },
            // Clockwise winding — the area sign flips, the range does not.
            { { 4.5f, 4.5f }, { 4.5f, 8.5f }, { 8.5f, 4.5f }, { 64.0f, 64.0f } },
            // Degenerate (collinear): area is neither positive nor negative.
            { { 3.0f, 3.0f }, { 6.0f, 6.0f }, { 9.0f, 9.0f }, { 64.0f, 64.0f } },
            // Off-screen on each side -> reject, with in-range int bounds.
            { { -40.0f, 10.0f }, { -20.0f, 10.0f }, { -40.0f, 20.0f }, { 64.0f, 64.0f } },
            { { 90.0f, 10.0f }, { 120.0f, 10.0f }, { 90.0f, 20.0f }, { 64.0f, 64.0f } },
            { { 10.0f, -40.0f }, { 20.0f, -40.0f }, { 10.0f, -20.0f }, { 64.0f, 64.0f } },
            { { 10.0f, 90.0f }, { 20.0f, 90.0f }, { 10.0f, 120.0f }, { 64.0f, 64.0f } },
            // Hanging off the near corner: clamped to the screen edge.
            { { -5.5f, -5.5f }, { 6.5f, -5.5f }, { -5.5f, 6.5f }, { 37.0f, 23.0f } },
            // A box far outside the int range: the float clamps keep the
            // conversion legal and the range comes back as the whole screen.
            { { -kHuge, -kHuge }, { kHuge, -kHuge }, { -kHuge, kHuge }, { 37.0f, 23.0f } },
        } };

        std::vector<glm::vec4> inputs;
        inputs.reserve(cases.size() * 2);
        for (const Case& c : cases)
        {
            inputs.emplace_back(c.S0.x, c.S0.y, c.S1.x, c.S1.y);
            inputs.emplace_back(c.S2.x, c.S2.y, c.Viewport.x, c.Viewport.y);
        }

        const auto count = static_cast<u32>(cases.size());
        const auto inputBytes = static_cast<GLsizeiptr>(inputs.size() * sizeof(glm::vec4));
        ScopedBuffer input(inputBytes, GL_DYNAMIC_STORAGE_BIT);
        ::glNamedBufferSubData(input.m_Id, 0, inputBytes, inputs.data());
        ScopedBuffer ranges(static_cast<GLsizeiptr>(count * sizeof(glm::ivec4)),
                            GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
        ScopedBuffer meta(static_cast<GLsizeiptr>(count * sizeof(glm::uvec4)),
                          GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);

        auto shader = ComputeShader::Create("assets/shaders/tests/ShaderUnit_VirtualSampleBounds.glsl");
        ASSERT_TRUE(shader && shader->IsValid())
            << "ShaderUnit_VirtualSampleBounds failed to compile/link — "
               "include/VirtualRasterCoverage.glsl does not build as included";

        shader->Bind();
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, input.m_Id);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ranges.m_Id);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, meta.m_Id);
        ::glDispatchCompute(count, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::vector<glm::ivec4> gpuRanges(count);
        std::vector<glm::uvec4> gpuMeta(count);
        ::glGetNamedBufferSubData(ranges.m_Id, 0,
                                  static_cast<GLsizeiptr>(count * sizeof(glm::ivec4)),
                                  gpuRanges.data());
        ::glGetNamedBufferSubData(meta.m_Id, 0,
                                  static_cast<GLsizeiptr>(count * sizeof(glm::uvec4)),
                                  gpuMeta.data());
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
        shader->Unbind();

        u32 kept = 0;
        for (u32 i = 0; i < count; ++i)
        {
            const Case& c = cases[i];
            const Mirror::SampleRange expected =
                Mirror::SampleRangeFromTriangle(c.S0, c.S1, c.S2, c.Viewport);
            SCOPED_TRACE(::testing::Message() << "case " << i);

            EXPECT_EQ(gpuMeta[i].x, static_cast<u32>(expected.Covers));
            if (expected.Covers)
            {
                ++kept;
                EXPECT_EQ(gpuRanges[i].x, expected.Min.x);
                EXPECT_EQ(gpuRanges[i].y, expected.Min.y);
                EXPECT_EQ(gpuRanges[i].z, expected.Max.x);
                EXPECT_EQ(gpuRanges[i].w, expected.Max.y);
            }
            else
            {
                // Even on the reject path the bounds must stay legal ints —
                // that is what the float-side clamping is there for.
                EXPECT_GE(gpuRanges[i].x, 0);
                EXPECT_LE(gpuRanges[i].x, static_cast<i32>(c.Viewport.x));
                EXPECT_GE(gpuRanges[i].z, -1);
                EXPECT_LE(gpuRanges[i].z, static_cast<i32>(c.Viewport.x) - 1);
                EXPECT_GE(gpuRanges[i].y, 0);
                EXPECT_LE(gpuRanges[i].y, static_cast<i32>(c.Viewport.y));
                EXPECT_GE(gpuRanges[i].w, -1);
                EXPECT_LE(gpuRanges[i].w, static_cast<i32>(c.Viewport.y) - 1);
            }

            // Only the SIGN of the area is contractual (the GPU may contract
            // the products into an FMA), and a CCW triangle must read positive:
            // the raster's backface reject depends on that orientation.
            const f32 expectedArea = Mirror::SignedArea2(c.S0, c.S1, c.S2);
            const u32 expectedSign = (expectedArea > 0.0f) ? 1u : ((expectedArea < 0.0f) ? 0u : 2u);
            EXPECT_EQ(gpuMeta[i].y, expectedSign);
        }

        // Anti-vacuous: the table must contain both outcomes, or every
        // comparison above passed against one branch of the rule.
        EXPECT_GT(kept, 0u);
        EXPECT_LT(kept, count);
    }

} // namespace OloEngine::Tests
