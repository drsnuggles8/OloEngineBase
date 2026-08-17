// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GPUPrefixSumPerfProbe.cpp — what did the #713 conversion actually cost?
//
// The PR claims the scan trades dispatches for determinism, and that the trade
// is worth it for particle compaction because that counter was heavily
// contended. Both halves of that were an argument, not a number. This measures
// them: the pre-#713 `atomicAdd` compaction (preserved verbatim as
// `assets/shaders/tests/PerfProbe_ParticleCompactAtomic.comp`) against the
// flag → scan → scatter compaction, over byte-identical input, on the same GPU
// in the same process, with GL_TIME_ELAPSED around each full compaction.
//
// DISABLED_ ON PURPOSE. This is an instrument, not a gate:
//   * it has no baseline and asserts no timing, so it cannot fail for
//     environmental reasons (docs/testing.md's anti-flake rule);
//   * GPU timings on this box are contended by other worktrees and by the
//     desktop, so a threshold here would be noise;
//   * L6 perf baselines are dev-workstation-only in this repo anyway.
// It exists so the PR's cost claim is reproducible rather than asserted, and so
// the next person converting a compaction can measure their own pass instead of
// re-deriving the argument. Run it with:
//
//   OloEngine-Tests.exe --gtest_also_run_disabled_tests \
//                       --gtest_filter='*GPUPrefixSumPerfProbe*'
//
// WHAT THIS PROBE DOES AND DOES NOT ISOLATE. It reports the end-to-end cost of
// each compaction. It is NOT a controlled contention experiment, and the numbers
// must not be read as one:
//
//   * every atomic-path invocation performs exactly one `atomicAdd` at every
//     alive fraction — the sweep changes only how the increments SPLIT between
//     `aliveCount` and `deadCount`, so it contrasts "mostly one address" (95 %)
//     with "two addresses" (50 %), not "contended" with "uncontended";
//   * isolating same-address contention would need a third variant with the
//     atomics removed as a control, which this does not build.
//
// The going-in hypothesis was that the atomic path would degrade as the particle
// count and alive fraction rose, and that the scan would converge with or beat
// it. Treat that as a hypothesis the probe REPORTS ON, not one it establishes —
// on the measured configuration it did not hold, and §3 of
// docs/agent-rules/gpu-scan-compaction.md states the scoped conclusion.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/GPUPrefixSum.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <algorithm>
#include <cstddef>
#include <random>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // One GL_TIME_ELAPSED sample around whatever `work` dispatches. The
        // query brackets GPU work only, so CPU-side binding cost is excluded
        // from both sides equally.
        template<typename Fn>
        f64 TimeGpuMillis(Fn&& work)
        {
            GLuint query = 0;
            ::glGenQueries(1, &query);
            ::glBeginQuery(GL_TIME_ELAPSED, query);
            work();
            ::glEndQuery(GL_TIME_ELAPSED);

            GLint done = 0;
            while (done == 0)
                ::glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &done);

            GLuint64 elapsedNs = 0;
            ::glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsedNs);
            ::glDeleteQueries(1, &query);
            return static_cast<f64>(elapsedNs) / 1.0e6;
        }

        // Median rather than mean: a single scheduling hiccup on a desktop GPU
        // shifts a mean and leaves a median alone, and this box is never idle.
        f64 Median(std::vector<f64> samples)
        {
            if (samples.empty())
                return 0.0;
            std::ranges::sort(samples);
            return samples[samples.size() / 2];
        }

        std::vector<GPUParticle> MakeParticles(u32 count, f64 aliveFraction, u32 seed)
        {
            std::mt19937 rng(seed);
            std::bernoulli_distribution alive(aliveFraction);
            std::vector<GPUParticle> particles(count);
            for (auto& p : particles)
                p.Misc.z = alive(rng) ? 1.0f : 0.0f;
            return particles;
        }
    } // namespace

    TEST(GPUPrefixSumPerfProbe, DISABLED_AtomicVersusScanCompaction)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto flagShader = ComputeShader::Create("assets/shaders/compute/Particle_Compact.comp");
        auto scatterShader = ComputeShader::Create("assets/shaders/compute/Particle_CompactScatter.comp");
        auto atomicShader = ComputeShader::Create("assets/shaders/tests/PerfProbe_ParticleCompactAtomic.comp");
        ASSERT_TRUE(flagShader && flagShader->IsValid());
        ASSERT_TRUE(scatterShader && scatterShader->IsValid());
        ASSERT_TRUE(atomicShader && atomicShader->IsValid());

        GPUPrefixSum scan;
        scan.EnsureInitialised();
        ASSERT_TRUE(scan.IsAvailable());

        constexpr u32 kWarmup = 5;
        constexpr u32 kSamples = 31;

        std::printf("\n  count      alive%%   atomic(ms)   scan(ms)   ratio\n");
        std::printf("  ---------- ------ ------------ ---------- -------\n");

        for (const u32 count : { 1000u, 10000u, 100000u, 1000000u })
        {
            for (const f64 aliveFraction : { 0.1, 0.5, 0.95 })
            {
                const std::vector<GPUParticle> particles = MakeParticles(count, aliveFraction, 0x713u);
                const u32 particleBytes = count * static_cast<u32>(sizeof(GPUParticle));
                const u32 indexBytes = count * static_cast<u32>(sizeof(u32));

                auto particleBuf = StorageBuffer::Create(particleBytes, ShaderBindingLayout::SSBO_GPU_PARTICLES,
                                                         StorageBufferUsage::DynamicCopy);
                particleBuf->SetData(particles.data(), particleBytes, 0);
                auto aliveBuf = StorageBuffer::Create(indexBytes, ShaderBindingLayout::SSBO_ALIVE_INDICES,
                                                      StorageBufferUsage::DynamicCopy);
                auto freeBuf = StorageBuffer::Create(indexBytes, ShaderBindingLayout::SSBO_FREE_LIST,
                                                     StorageBufferUsage::DynamicCopy);
                auto counterBuf = StorageBuffer::Create(static_cast<u32>(sizeof(GPUParticleCounters)),
                                                        ShaderBindingLayout::SSBO_COUNTERS,
                                                        StorageBufferUsage::DynamicCopy);
                auto scanBuf = StorageBuffer::Create(indexBytes, ShaderBindingLayout::SSBO_PREFIX_SUM_VALUES,
                                                     StorageBufferUsage::DynamicCopy);

                UBOStructures::GPUParticleParamsUBO params{};
                params.MaxParticles = count;
                auto paramsUBO = UniformBuffer::Create(UBOStructures::GPUParticleParamsUBO::GetSize(),
                                                       ShaderBindingLayout::UBO_PARTICLE_SIM);

                const u32 groups = (count + 255u) / 256u;

                // Both paths reset the counters first — the atomic one requires
                // it (atomics accumulate), and charging it to only one side
                // would be measuring the wrong thing.
                const auto resetCounters = [&counterBuf]()
                {
                    GPUParticleCounters zeroed{};
                    counterBuf->SetData(&zeroed, sizeof(GPUParticleCounters));
                };

                const auto runAtomic = [&]()
                {
                    particleBuf->Bind();
                    aliveBuf->Bind();
                    counterBuf->Bind();
                    freeBuf->Bind();
                    atomicShader->Bind();
                    paramsUBO->SetData(&params, sizeof(params));
                    paramsUBO->Bind();
                    RenderCommand::DispatchCompute(groups, 1, 1);
                    RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
                };

                const auto runScan = [&]()
                {
                    particleBuf->Bind();
                    scanBuf->Bind();
                    flagShader->Bind();
                    paramsUBO->SetData(&params, sizeof(params));
                    paramsUBO->Bind();
                    RenderCommand::DispatchCompute(groups, 1, 1);
                    RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

                    scan.ExclusiveScanInPlace(scanBuf, count, counterBuf);

                    particleBuf->Bind();
                    aliveBuf->Bind();
                    counterBuf->Bind();
                    freeBuf->Bind();
                    scanBuf->Bind();
                    scatterShader->Bind();
                    paramsUBO->SetData(&params, sizeof(params));
                    paramsUBO->Bind();
                    RenderCommand::DispatchCompute(groups, 1, 1);
                    RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
                };

                for (u32 i = 0; i < kWarmup; ++i)
                {
                    resetCounters();
                    runAtomic();
                    resetCounters();
                    runScan();
                }
                ::glFinish();

                std::vector<f64> atomicSamples;
                std::vector<f64> scanSamples;
                atomicSamples.reserve(kSamples);
                scanSamples.reserve(kSamples);
                for (u32 i = 0; i < kSamples; ++i)
                {
                    resetCounters();
                    atomicSamples.push_back(TimeGpuMillis(runAtomic));
                    resetCounters();
                    scanSamples.push_back(TimeGpuMillis(runScan));
                }

                const f64 atomicMs = Median(std::move(atomicSamples));
                const f64 scanMs = Median(std::move(scanSamples));
                std::printf("  %10u %5.0f%% %12.4f %10.4f %6.2fx\n", count, aliveFraction * 100.0,
                            atomicMs, scanMs, (atomicMs > 0.0) ? (scanMs / atomicMs) : 0.0);
                std::fflush(stdout);

                particleBuf->Unbind();
                aliveBuf->Unbind();
                freeBuf->Unbind();
                counterBuf->Unbind();
                scanBuf->Unbind();
            }
        }

        SUCCEED() << "measurement only — see the table above";
    }
} // namespace OloEngine::Tests
