#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L2

// =============================================================================
// WaterWakeParityTest — the #968 acceptance criterion "CPU WaterSurface and GPU
// water agree on wake height at sampled points within a documented tolerance".
//
// Layer-2 of the renderer pyramid: the production shader function, driven on
// the real GPU by an SSBO of inputs, read back and compared against its C++
// mirror. SKIPs cleanly where there is no GL 4.6 context, so headless CI is
// covered by WaterWakeShapeTest's L1 half instead.
//
// WHY THIS TEST CAN EXIST AT ALL, and it is the reason #968 is built the way it
// is: the #967 wake is a GPU-written decaying raster, and there is no way to
// ask a raster what it thinks the height is without a readback and a frame of
// lag. The wake SHAPE is instead an analytic function of a small CPU-built
// record, so both sides can be handed the SAME bytes and asked the same
// question. Parity is achievable by construction rather than by synchronising
// two representations.
//
// WHAT MAKES IT MEANINGFUL: the probe shader (tests/ShaderUnit_WaterWake.glsl)
// `#include`s include/WaterWakeCommon.glsl — the very text the water vertex and
// tess-eval stages run — rather than copying it, which the older probes here do
// (ShaderUnit_Fog.glsl duplicates FogCommon.glsl verbatim and says so). A copy
// would test the copy. The evaluator asks for its records through a
// `waterWakeFetch` hook, so this probe supplies them from an SSBO while the
// water stages supply them from WaterUBO, with one shared walk over them.
//
// THE TOLERANCE, and why it is what it is: both sides evaluate the same
// expressions in f32, but neither the order of operations nor the precision of
// `pow`, `sqrt` and `length` is pinned across a compiler and a GPU driver. The
// dominant term is `pow(1 - d/r, 2.5)` in the shared capsule falloff, whose
// GLSL implementation is only required to be accurate to a few ULP of a
// `exp2(y * log2(x))` decomposition. 1 mm on a wake whose features are tens of
// centimetres is roughly 0.3% and is comfortably inside what a driver can
// legitimately do; it is also far tighter than any difference a real bug would
// produce (a mirrored arm, a swapped starboard vector or a dropped band limit
// all move the answer by whole decimetres or to zero).
//
// MEASURED on this machine (RTX 4090 / clang-cl, 2026-08-30) across 5600 query
// points: worst height disagreement 3.0e-7 m, worst flatten disagreement
// 4.5e-7 — about 3300x inside the tolerance, i.e. the two sides agree to f32
// rounding. The margin is left generous anyway, because the tolerance exists
// for the driver this test has not run on yet; the run prints the actual worst
// values on success so a future change that eats that margin is visible before
// it becomes a failure.
// =============================================================================

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Water/WaterWake.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        /// Millimetre agreement. See the header block for why this number.
        constexpr f32 kHeightToleranceMetres = 1.0e-3f;
        /// The flatten is a smoothstep of a ratio — no transcendentals, so it
        /// should agree far more tightly than the height does.
        constexpr f32 kFlattenTolerance = 1.0e-4f;

        struct ScopedSsbo
        {
            GLuint m_Id = 0;
            explicit ScopedSsbo(GLsizeiptr bytes, GLbitfield flags)
            {
                ::glCreateBuffers(1, &m_Id);
                ::glNamedBufferStorage(m_Id, bytes, nullptr, flags);
            }
            ~ScopedSsbo()
            {
                if (m_Id != 0)
                    ::glDeleteBuffers(1, &m_Id);
            }
            ScopedSsbo(const ScopedSsbo&) = delete;
            ScopedSsbo& operator=(const ScopedSsbo&) = delete;
        };

        /// A hull under way along +Z, with a full historical arm polyline —
        /// the same fixture shape WaterWakeShapeTest uses.
        [[nodiscard]] WaterWakeHullDesc Runner(glm::vec2 centre, glm::vec2 forward, f32 speed)
        {
            WaterWakeHullDesc desc;
            desc.m_CentreXZ = centre;
            desc.m_ForwardXZ = forward;
            desc.m_HalfBeam = 1.2f;
            desc.m_HalfLength = 3.0f;
            desc.m_Speed = speed;
            desc.m_Gate = 1.0f;
            desc.m_ArmSampleCount = WaterWake::kMaxArmSamples;
            for (u32 i = 0; i < WaterWake::kMaxArmSamples; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(WaterWake::kMaxArmSamples - 1u);
                const f32 age = 0.15f + t * (2.5f - 0.15f);
                // Curve the historical heading so the polyline is not a straight
                // line: a straight trail hides a whole class of packing error
                // (using the CURRENT starboard vector for every sample) that a
                // curved one exposes on both sides identically.
                const f32 theta = t * 0.6f;
                const glm::vec2 fwd{ forward.x * std::cos(theta) - forward.y * std::sin(theta),
                                     forward.x * std::sin(theta) + forward.y * std::cos(theta) };
                WaterWakeArmSample& s = desc.m_Arms[i];
                s.m_CentreXZ = centre - fwd * (speed * age);
                s.m_ForwardXZ = fwd;
                s.m_AgeSeconds = age;
                s.m_Speed = speed;
                s.m_Gate = 1.0f;
            }
            return desc;
        }
    } // namespace

    TEST(WaterWakeParityTest, CpuAndGpuAgreeOnWakeHeightAtSampledPoints)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // --- Build a record set the CPU and the GPU will both be handed ------
        // Three hulls, deliberately different: one fast and curving, one slow
        // (so its gate-scaled arms sit close to the beam), one stopped (so only
        // its footprint contributes). A single-hull fixture would not exercise
        // the loop's per-hull indexing at all, which is where a stride mistake
        // in the flat vec4 layout would live.
        WaterWakeSystem::Reset();
        WaterWakeSystem::BeginFrame();
        ASSERT_TRUE(WaterWakeSystem::SubmitHull(Runner({ 0.0f, 0.0f }, { 0.0f, 1.0f }, 12.0f)));
        ASSERT_TRUE(WaterWakeSystem::SubmitHull(Runner({ 30.0f, -8.0f }, { 0.7071f, 0.7071f }, 2.0f)));
        {
            WaterWakeHullDesc stopped = Runner({ -22.0f, 14.0f }, { -0.6f, 0.8f }, 0.0f);
            stopped.m_Gate = 0.0f;
            for (auto& arm : stopped.m_Arms)
            {
                arm.m_Speed = 0.0f;
                arm.m_Gate = 0.0f;
            }
            ASSERT_TRUE(WaterWakeSystem::SubmitHull(stopped));
        }
        const u32 hullCount = WaterWakeSystem::GetHullCount();
        ASSERT_EQ(hullCount, 3u);

        constexpr f32 kHeightScale = 1.3f;
        constexpr f32 kFlattenStrength = 0.85f;

        // --- The query grid --------------------------------------------------
        // Deliberately NOT on a round lattice relative to the hulls: an offset
        // grid lands inside the footprint, on the fade rim, on and beside both
        // arms, in the bow bump and the stern trough, and out in open water.
        // Several vertex spacings per point, including 0, so the band limit is
        // covered on both sides rather than sidestepped (WaterWake.h section 5).
        struct Query
        {
            glm::vec2 m_WorldXZ;
            f32 m_VertexSpacing;
        };
        std::vector<Query> queries;
        for (const f32 spacing : { 0.0f, 0.35f, 1.1f, 2.7f, 6.0f })
        {
            for (f32 z = -34.3f; z <= 20.0f; z += 1.7f)
            {
                for (f32 x = -28.7f; x <= 36.0f; x += 1.9f)
                {
                    queries.push_back({ { x, z }, spacing });
                }
            }
        }
        ASSERT_GT(queries.size(), 2000u);

        // --- CPU side --------------------------------------------------------
        std::vector<WaterWake::Sample> cpu;
        cpu.reserve(queries.size());
        for (const Query& q : queries)
        {
            cpu.push_back(WaterWake::Evaluate(WaterWakeSystem::GetHullData(), hullCount, kHeightScale,
                                              kFlattenStrength, q.m_WorldXZ, q.m_VertexSpacing));
        }

        // NEGATIVE CONTROL, and the load-bearing one. Every assertion below is
        // an agreement check, and agreement is trivially satisfied if both
        // sides return zero everywhere — which is exactly what a grid that
        // misses the wake, a disabled height scale, or an evaluator that early-
        // outs would produce. Insist first that the fixture actually contains a
        // wake, in both signs, and a footprint.
        {
            u32 raised = 0, lowered = 0, flattened = 0;
            for (const WaterWake::Sample& s : cpu)
            {
                if (s.m_Height > 0.01f)
                    ++raised;
                if (s.m_Height < -0.01f)
                    ++lowered;
                if (s.m_Flatten > 0.01f)
                    ++flattened;
            }
            ASSERT_GT(raised, 20u) << "the query grid finds no raised wake — parity would be vacuous";
            ASSERT_GT(lowered, 5u) << "the query grid finds no stern trough";
            ASSERT_GT(flattened, 5u) << "the query grid never lands on a hull footprint";
        }

        // --- GPU side --------------------------------------------------------
        // Header vec4 + the records, exactly as the probe expects.
        std::vector<glm::vec4> hullBuffer;
        hullBuffer.reserve(1u + WaterWake::kHullVec4Count);
        hullBuffer.emplace_back(static_cast<f32>(hullCount), kHeightScale, kFlattenStrength, 0.0f);
        hullBuffer.insert(hullBuffer.end(), WaterWakeSystem::GetHullData(),
                          WaterWakeSystem::GetHullData() + WaterWake::kHullVec4Count);

        std::vector<glm::vec4> queryBuffer;
        queryBuffer.reserve(queries.size());
        for (const Query& q : queries)
            queryBuffer.emplace_back(q.m_WorldXZ.x, q.m_WorldXZ.y, q.m_VertexSpacing, 0.0f);

        const auto hullBytes = static_cast<GLsizeiptr>(hullBuffer.size() * sizeof(glm::vec4));
        const auto queryBytes = static_cast<GLsizeiptr>(queryBuffer.size() * sizeof(glm::vec4));

        ScopedSsbo hullSsbo(hullBytes, GL_DYNAMIC_STORAGE_BIT);
        ScopedSsbo querySsbo(queryBytes, GL_DYNAMIC_STORAGE_BIT);
        ScopedSsbo outSsbo(queryBytes, GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
        ::glNamedBufferSubData(hullSsbo.m_Id, 0, hullBytes, hullBuffer.data());
        ::glNamedBufferSubData(querySsbo.m_Id, 0, queryBytes, queryBuffer.data());

        auto probe = ComputeShader::Create("assets/shaders/tests/ShaderUnit_WaterWake.glsl");
        ASSERT_TRUE(probe && probe->IsValid())
            << "the wake parity probe failed to compile — if this is an #include error, "
               "WaterWakeCommon.glsl is what the water stages run too";
        probe->Bind();

        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, hullSsbo.m_Id);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, querySsbo.m_Id);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, outSsbo.m_Id);
        constexpr u32 kLocal = 64;
        const auto count = static_cast<u32>(queries.size());
        ::glDispatchCompute((count + kLocal - 1) / kLocal, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::vector<glm::vec4> gpu(queries.size());
        ::glGetNamedBufferSubData(outSsbo.m_Id, 0, queryBytes, gpu.data());

        // --- Compare ---------------------------------------------------------
        // Report the WORST disagreement rather than the first, so a failure says
        // how far apart the two halves are (a driver ULP difference and a
        // mirrored arm are the same "not equal" to a first-failure assertion,
        // and they need completely different fixes).
        f32 worstHeight = 0.0f;
        f32 worstFlatten = 0.0f;
        sizet worstHeightIndex = 0;
        sizet worstFlattenIndex = 0;
        for (sizet i = 0; i < queries.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(gpu[i].x) && std::isfinite(gpu[i].y))
                << "GPU returned a non-finite wake at query " << i;
            if (const f32 dh = std::abs(gpu[i].x - cpu[i].m_Height); dh > worstHeight)
            {
                worstHeight = dh;
                worstHeightIndex = i;
            }
            if (const f32 df = std::abs(gpu[i].y - cpu[i].m_Flatten); df > worstFlatten)
            {
                worstFlatten = df;
                worstFlattenIndex = i;
            }
        }

        // Printed on success too: the margin against the tolerance is the number
        // that says whether this test is measuring anything, and a future
        // driver or compiler change that halves the margin is worth seeing
        // BEFORE it becomes a failure somebody has to diagnose cold.
        std::cout << "[wake-parity] " << queries.size() << " points, worst height disagreement "
                  << worstHeight << " m (tolerance " << kHeightToleranceMetres
                  << "), worst flatten disagreement " << worstFlatten << " (tolerance "
                  << kFlattenTolerance << ")" << std::endl;

        EXPECT_LE(worstHeight, kHeightToleranceMetres)
            << "worst wake-height disagreement " << worstHeight << " m at ("
            << queries[worstHeightIndex].m_WorldXZ.x << ", " << queries[worstHeightIndex].m_WorldXZ.y
            << ") spacing " << queries[worstHeightIndex].m_VertexSpacing << " — CPU "
            << cpu[worstHeightIndex].m_Height << " m, GPU " << gpu[worstHeightIndex].x << " m";

        EXPECT_LE(worstFlatten, kFlattenTolerance)
            << "worst hull-flatten disagreement " << worstFlatten << " at ("
            << queries[worstFlattenIndex].m_WorldXZ.x << ", " << queries[worstFlattenIndex].m_WorldXZ.y
            << ") — CPU " << cpu[worstFlattenIndex].m_Flatten << ", GPU " << gpu[worstFlattenIndex].y;
    }

    TEST(WaterWakeParityTest, CpuAndGpuAgreeThatADisabledWakeIsExactlyNothing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The disabled state is worth its own case because it is the one both
        // halves reach through an EARLY OUT rather than through the shared
        // arithmetic, and an early out that disagrees is how a stale footprint
        // stays pressed into the sea on one side only. It must be exactly zero
        // on both, not merely close.
        WaterWakeSystem::Reset();
        WaterWakeSystem::BeginFrame();
        ASSERT_TRUE(WaterWakeSystem::SubmitHull(Runner({ 0.0f, 0.0f }, { 0.0f, 1.0f }, 10.0f)));

        std::vector<glm::vec4> hullBuffer;
        hullBuffer.emplace_back(static_cast<f32>(WaterWakeSystem::GetHullCount()), 0.0f, 0.9f, 0.0f);
        hullBuffer.insert(hullBuffer.end(), WaterWakeSystem::GetHullData(),
                          WaterWakeSystem::GetHullData() + WaterWake::kHullVec4Count);

        std::vector<glm::vec4> queryBuffer;
        for (f32 z = -30.0f; z <= 10.0f; z += 2.0f)
            for (f32 x = -10.0f; x <= 10.0f; x += 2.0f)
                queryBuffer.emplace_back(x, z, 0.0f, 0.0f);

        const auto hullBytes = static_cast<GLsizeiptr>(hullBuffer.size() * sizeof(glm::vec4));
        const auto queryBytes = static_cast<GLsizeiptr>(queryBuffer.size() * sizeof(glm::vec4));
        ScopedSsbo hullSsbo(hullBytes, GL_DYNAMIC_STORAGE_BIT);
        ScopedSsbo querySsbo(queryBytes, GL_DYNAMIC_STORAGE_BIT);
        ScopedSsbo outSsbo(queryBytes, GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
        ::glNamedBufferSubData(hullSsbo.m_Id, 0, hullBytes, hullBuffer.data());
        ::glNamedBufferSubData(querySsbo.m_Id, 0, queryBytes, queryBuffer.data());

        auto probe = ComputeShader::Create("assets/shaders/tests/ShaderUnit_WaterWake.glsl");
        ASSERT_TRUE(probe && probe->IsValid());
        probe->Bind();
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, hullSsbo.m_Id);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, querySsbo.m_Id);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, outSsbo.m_Id);
        const auto count = static_cast<u32>(queryBuffer.size());
        ::glDispatchCompute((count + 63u) / 64u, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        std::vector<glm::vec4> gpu(queryBuffer.size());
        ::glGetNamedBufferSubData(outSsbo.m_Id, 0, queryBytes, gpu.data());

        for (sizet i = 0; i < queryBuffer.size(); ++i)
        {
            const WaterWake::Sample c =
                WaterWake::Evaluate(WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(), 0.0f,
                                    0.9f, glm::vec2(queryBuffer[i].x, queryBuffer[i].y), 0.0f);
            ASSERT_FLOAT_EQ(c.m_Height, 0.0f);
            ASSERT_FLOAT_EQ(c.m_Flatten, 0.0f);
            ASSERT_FLOAT_EQ(gpu[i].x, 0.0f) << "GPU still displaces water with the wake disabled";
            ASSERT_FLOAT_EQ(gpu[i].y, 0.0f) << "GPU still flattens water with the wake disabled";
        }
    }
} // namespace OloEngine::Tests
