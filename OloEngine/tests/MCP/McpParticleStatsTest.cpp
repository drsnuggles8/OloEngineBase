// OLO_TEST_LAYER: unit
// =============================================================================
// McpParticleStatsTest.cpp — the pure half of olo_particle_stats (#607, #1171).
//
// The tool exists to answer one question — "did the particle renderer submit any
// draws this frame?" — because nothing else could. During #1171's investigation a
// blank Vulkan frame could not be told apart from a frame with nothing submitted,
// and the search went into the shader instead (four probes, all exonerating it).
//
// So the thing worth testing is not the JSON shape but the VERDICT: that each
// cause produces a different answer, and that the tool refuses to pick between
// two causes it cannot distinguish. A verdict that reads plausibly for every
// input would recreate the exact ambiguity this tool was written to remove.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "MCP/McpParticleStats.h"

#include <string>
#include <vector>

namespace OloEditor::MCP::Tests
{
    namespace
    {
        ParticleEmitterFacts MakeEmitter(u32 alive, bool playing = true, bool useGPU = false)
        {
            ParticleEmitterFacts e;
            e.Name = "Emitter";
            e.EntityId = "1";
            e.AliveCount = alive;
            e.MaxParticles = 50000;
            e.Playing = playing;
            e.UseGPU = useGPU;
            e.RenderMode = "billboard";
            e.DistanceToCamera = 10.0f;
            e.LODMaxDistance = 200.0f;
            return e;
        }

        [[nodiscard]] bool Mentions(const std::string& haystack, std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The distinction the tool exists for
    // -------------------------------------------------------------------------
    TEST(McpParticleStats, AliveParticlesWithZeroDrawCallsBlamesSubmissionNotRasterisation)
    {
        const ParticleSubmissionFacts submission{ .DrawCalls = 0, .InstanceCount = 0 };
        const std::vector<ParticleEmitterFacts> emitters{ MakeEmitter(40000) };

        const std::string verdict = ExplainParticleFrame(submission, emitters);
        EXPECT_TRUE(Mentions(verdict, "ZERO draw calls")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "submission")) << verdict;
        // The whole point: it must NOT send the reader downstream.
        EXPECT_FALSE(Mentions(verdict, "at or after rasterisation")) << verdict;
    }

    TEST(McpParticleStats, DrawCallsSubmittedSendsTheReaderDownstreamInstead)
    {
        const ParticleSubmissionFacts submission{ .DrawCalls = 4, .InstanceCount = 40000 };
        const std::vector<ParticleEmitterFacts> emitters{ MakeEmitter(40000) };

        const std::string verdict = ExplainParticleFrame(submission, emitters);
        EXPECT_TRUE(Mentions(verdict, "rasterisation")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "NOT submission")) << verdict;
    }

    // -------------------------------------------------------------------------
    // Causes that are NOT a rendering fault must not be reported as one
    // -------------------------------------------------------------------------
    TEST(McpParticleStats, NoEmittersIsExpectedNotAFault)
    {
        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{}, {});
        EXPECT_TRUE(Mentions(verdict, "expected, not a fault")) << verdict;
    }

    TEST(McpParticleStats, StoppedEmittersAreASimulationStateNotARenderingFault)
    {
        const std::vector<ParticleEmitterFacts> emitters{ MakeEmitter(0, /*playing=*/false) };
        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{}, emitters);
        EXPECT_TRUE(Mentions(verdict, "not a rendering fault")) << verdict;
    }

    TEST(McpParticleStats, EveryEmitterBeyondLodExplainsTheZeroBySuppressedSpawning)
    {
        // NB: not `near` / `far` — both are windef.h macros on Windows.
        ParticleEmitterFacts distant = MakeEmitter(0);
        distant.DistanceToCamera = 900.0f;
        distant.LODMaxDistance = 200.0f;

        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{}, { distant });
        EXPECT_TRUE(Mentions(verdict, "LODMaxDistance")) << verdict;
    }

    TEST(McpParticleStats, PlayingButNoParticlesAliveBlamesSimulationNotRendering)
    {
        const std::vector<ParticleEmitterFacts> emitters{ MakeEmitter(0, /*playing=*/true) };
        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{}, emitters);
        EXPECT_TRUE(Mentions(verdict, "simulation")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "not in rendering")) << verdict;
    }

    // A GPU-driven emitter draws indirectly, so zero CPU draw calls is CORRECT.
    // Reporting that as a missing draw would manufacture exactly the false lead
    // this tool removes.
    TEST(McpParticleStats, GpuDrivenEmittersDoNotCountZeroCpuDrawCallsAsAMissingDraw)
    {
        ParticleEmitterFacts gpu = MakeEmitter(/*alive=*/0, /*playing=*/true, /*useGPU=*/true);
        gpu.GpuAliveCount = 40000;

        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{}, { gpu });
        EXPECT_FALSE(Mentions(verdict, "ZERO draw calls")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "say nothing")) << verdict;
    }

    // The regression this file exists for: the FIRST version of this tool summed
    // a GPU emitter's empty-by-design CPU pool into the alive total, hit the
    // "no particle is alive" branch, and reported "the fault is in simulation"
    // for a scene whose simulation was fine and running on the GPU. That is the
    // false lead the tool was written to prevent, produced by the tool itself.
    TEST(McpParticleStats, GpuEmitterWithLiveParticlesIsNeverReportedAsASimulationFault)
    {
        ParticleEmitterFacts gpu = MakeEmitter(/*alive=*/0, /*playing=*/true, /*useGPU=*/true);
        gpu.GpuAliveCount = 41000;

        // Draw calls non-zero: the CPU batch renderer ran for OTHER work.
        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{ .DrawCalls = 5 }, { gpu });
        EXPECT_FALSE(Mentions(verdict, "fault is in simulation")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "compute chain")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "INDIRECT")) << verdict;
    }

    TEST(McpParticleStats, GpuEmitterWithNoLiveParticlesPointsAtTheComputeChain)
    {
        ParticleEmitterFacts gpu = MakeEmitter(0, true, /*useGPU=*/true);
        gpu.GpuAliveCount = 0;

        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{ .DrawCalls = 5 }, { gpu });
        EXPECT_TRUE(Mentions(verdict, "emit/simulate")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "BY DESIGN")) << verdict;
    }

    // -1 means "not initialised yet", which must not read as a confirmed zero.
    TEST(McpParticleStats, UninitialisedGpuCountIsNotTreatedAsAConfirmedZero)
    {
        ParticleEmitterFacts gpu = MakeEmitter(0, true, /*useGPU=*/true);
        gpu.GpuAliveCount = -1;

        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{}, { gpu });
        EXPECT_TRUE(Mentions(verdict, "zero or unavailable")) << verdict;
    }

    // A GPU emitter's empty CPU pool must not drag the CPU total down either.
    TEST(McpParticleStatsJson, GpuEmitterPoolIsExcludedFromTheCpuAliveTotal)
    {
        ParticleEmitterFacts cpu = MakeEmitter(700, true, /*useGPU=*/false);
        ParticleEmitterFacts gpu = MakeEmitter(0, true, /*useGPU=*/true);
        gpu.GpuAliveCount = 41000;

        const nlohmann::json j = ParticleStatsJson(ParticleSubmissionFacts{ .DrawCalls = 1 }, { cpu, gpu });
        EXPECT_EQ(j.at("aliveTotal").get<u32>(), 700u);
        EXPECT_EQ(j.at("emitters")[1].at("gpuAliveCount").get<i64>(), 41000);
    }

    // A dead compute stage outranks every other explanation, including a live
    // GPU alive count: the dispatch helpers bail silently, so nothing
    // downstream of a stage that never compiled can be trusted.
    TEST(McpParticleStats, DeadComputeStageOutranksEveryOtherVerdict)
    {
        ParticleEmitterFacts gpu = MakeEmitter(0, true, /*useGPU=*/true);
        gpu.GpuAliveCount = 41000;
        gpu.DeadGpuStages = { "emit", "simulate" };

        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{ .DrawCalls = 5 }, { gpu });
        EXPECT_TRUE(Mentions(verdict, "FAILED TO COMPILE")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "emit, simulate")) << verdict;
        EXPECT_FALSE(Mentions(verdict, "compute chain")) << verdict;
    }

    // The regression CodeRabbit caught: a stopped CPU emitter beside a live GPU
    // one blamed CPU simulation, because the branch never consulted gpuAlive.
    TEST(McpParticleStats, IdleCpuEmitterBesideALiveGpuOneDoesNotBlameSimulation)
    {
        ParticleEmitterFacts cpu = MakeEmitter(/*alive=*/0, /*playing=*/true, /*useGPU=*/false);
        ParticleEmitterFacts gpu = MakeEmitter(0, true, /*useGPU=*/true);
        gpu.GpuAliveCount = 41000;

        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{ .DrawCalls = 5 }, { cpu, gpu });
        EXPECT_FALSE(Mentions(verdict, "fault is in simulation")) << verdict;
        EXPECT_TRUE(Mentions(verdict, "GPU simulation reports live particles")) << verdict;
    }

    // A mixed scene must still flag the CPU emitter rather than being excused by
    // the GPU one — the "every emitter is GPU-driven" branch is deliberately all,
    // not any.
    TEST(McpParticleStats, OneCpuEmitterAmongGpuOnesStillFlagsTheMissingSubmission)
    {
        const std::vector<ParticleEmitterFacts> emitters{ MakeEmitter(1000, true, /*useGPU=*/true),
                                                          MakeEmitter(1000, true, /*useGPU=*/false) };
        const std::string verdict = ExplainParticleFrame(ParticleSubmissionFacts{}, emitters);
        EXPECT_TRUE(Mentions(verdict, "ZERO draw calls")) << verdict;
    }

    // -------------------------------------------------------------------------
    // Serialization carries the facts the verdict was derived from
    // -------------------------------------------------------------------------
    TEST(McpParticleStatsJson, ReportsTotalsPerEmitterFlagsAndTheVerdict)
    {
        ParticleEmitterFacts close = MakeEmitter(120);
        ParticleEmitterFacts distant = MakeEmitter(0);
        distant.DistanceToCamera = 900.0f;

        const nlohmann::json j =
            ParticleStatsJson(ParticleSubmissionFacts{ .DrawCalls = 2, .InstanceCount = 120 }, { close, distant });

        EXPECT_EQ(j.at("emitterCount").get<u32>(), 2u);
        EXPECT_EQ(j.at("aliveTotal").get<u32>(), 120u);
        EXPECT_EQ(j.at("submission").at("drawCalls").get<u32>(), 2u);
        EXPECT_EQ(j.at("submission").at("instanceCount").get<u32>(), 120u);
        ASSERT_EQ(j.at("emitters").size(), 2u);
        EXPECT_FALSE(j.at("emitters")[0].at("beyondLOD").get<bool>());
        EXPECT_TRUE(j.at("emitters")[1].at("beyondLOD").get<bool>());
        EXPECT_FALSE(j.at("verdict").get<std::string>().empty());
    }

    // A zero LODMaxDistance means "unset / unknown", not "everything is beyond
    // it" — the camera-less path reports 0 and must not then claim every emitter
    // is LOD-suppressed.
    TEST(McpParticleStatsJson, ZeroLodMaxDistanceIsNeverReportedAsBeyondLod)
    {
        ParticleEmitterFacts unknown = MakeEmitter(0);
        unknown.DistanceToCamera = 0.0f;
        unknown.LODMaxDistance = 0.0f;

        const nlohmann::json j = ParticleStatsJson(ParticleSubmissionFacts{}, { unknown });
        EXPECT_FALSE(j.at("emitters")[0].at("beyondLOD").get<bool>());
        EXPECT_FALSE(Mentions(j.at("verdict").get<std::string>(), "LODMaxDistance"));
    }
} // namespace OloEditor::MCP::Tests
