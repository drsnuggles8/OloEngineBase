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

#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
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
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
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
    } // namespace

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
} // namespace OloEngine::Tests
