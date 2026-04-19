// =============================================================================
// ShaderUnitTests.cpp
//
// Layer-2 of the testing pyramid (docs/renderer-testing-strategy.md §2):
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

#include "OloEngine/Renderer/ComputeShader.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
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
} // namespace OloEngine::Tests
