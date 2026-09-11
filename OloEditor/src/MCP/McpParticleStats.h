#pragma once

// =============================================================================
// McpParticleStats.h — the pure half of olo_particle_stats (#607, #1171).
//
// Why this tool exists: while diagnosing #1171 (Vulkan billboards draw nothing)
// there was no way to answer the first question a rendering bug asks — "did the
// renderer submit any draws this frame?" ParticleBatchRenderer::GetStats()
// already tracked it and nothing exposed it, and olo_perf_snapshot's counters do
// not attribute particles: the same scene reads 0/14/256 draw calls on Vulkan and
// 24/6/127 on OpenGL while only OpenGL shows particles. So a zero on screen could
// not be told apart from a zero in submission, and the investigation bisected the
// shader instead — four probes, all exonerating it.
//
// That distinction is the whole value here, so it is a first-class output field
// rather than something the caller derives: `verdict` says which side of the
// submit/rasterise line the frame failed on, and never guesses when it cannot
// tell.
//
// Pure: no renderer, no httplib, no McpServer — see notes-mcp-tool-authoring.md
// §1. Everything here is driven directly by McpParticleStatsTest.cpp.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace OloEditor::MCP
{
    // One emitter's simulation-side state, gathered on the main thread.
    struct ParticleEmitterFacts
    {
        std::string Name;
        std::string EntityId;
        u32 AliveCount = 0;
        u32 MaxParticles = 0;
        bool Playing = false;
        bool UseGPU = false;
        // GPU emitters simulate on-device: their CPU pool is empty by design, so
        // AliveCount is meaningless for them and this is the count that matters.
        // -1 when the GPU system is not initialised (nothing to read yet).
        i64 GpuAliveCount = -1;
        // Empty when every GPU compute stage compiled. Each dispatch helper
        // returns early and silently on an invalid shader, so a dead stage is
        // indistinguishable from "nothing to do" without this.
        std::vector<std::string> DeadGpuStages;
        // Raw GPU counter block, straight off the device. DeadCount is the
        // discriminator that matters: a readable counter buffer reports the free
        // slot pool, so alive == 0 with dead == 0 means the buffer never reached
        // the CPU, while alive == 0 with dead == maxParticles means the
        // simulation genuinely produced nothing. -1 = not read.
        i64 GpuCounterAlive = -1;
        i64 GpuCounterDead = -1;
        i64 GpuCounterEmit = -1;
        i64 GpuIndirectInstanceCount = -1;
        // How many particles the CPU emitter handed the GPU system last update,
        // and the LOD multiplier feeding it. Separates "the emitter asked for
        // nothing" from "it asked and the dispatch did nothing".
        i64 LastGpuEmitRequest = -1;
        f32 LodSpawnRateMultiplier = -1.0f;
        // How many of the first N slots in the particle SSBO carry a non-zero
        // lifetime. The emit shader writes particles AND bumps the counters; if
        // this is non-zero while the counters are not, the dispatch ran and only
        // the atomics failed to land, which is a different bug from "the
        // dispatch did nothing". -1 = not sampled.
        i64 GpuParticleSlotsWritten = -1;
        i64 GpuParticleSlotsSampled = -1;
        std::string RenderMode;
        f32 DistanceToCamera = 0.0f;
        f32 LODMaxDistance = 0.0f;
    };

    // The renderer-side counters for the same frame.
    struct ParticleSubmissionFacts
    {
        u32 DrawCalls = 0;
        u32 InstanceCount = 0;
    };

    // The one question this tool exists to answer, decided from the two fact
    // sets rather than left to the caller. Deliberately conservative: every
    // branch that cannot distinguish two causes says so instead of picking one.
    [[nodiscard]] inline std::string ExplainParticleFrame(const ParticleSubmissionFacts& submission,
                                                          const std::vector<ParticleEmitterFacts>& emitters)
    {
        if (emitters.empty())
        {
            return "No ParticleSystemComponent in the scene — a zero here is expected, not a fault.";
        }

        u32 alive = 0;
        u32 playing = 0;
        u32 beyondLod = 0;
        u32 cpuEmitters = 0;
        i64 gpuAlive = 0;
        for (const ParticleEmitterFacts& e : emitters)
        {
            playing += e.Playing ? 1u : 0u;
            if (e.UseGPU)
            {
                gpuAlive += e.GpuAliveCount > 0 ? e.GpuAliveCount : 0;
            }
            else
            {
                ++cpuEmitters;
                // Only a CPU emitter's pool count means anything. Summing a GPU
                // emitter's empty-by-design pool into this total is how the
                // first version of this tool reported "the fault is in
                // simulation" for a scene whose simulation was fine and living
                // on the GPU — precisely the false lead it exists to prevent.
                alive += e.AliveCount;
            }
            if (e.LODMaxDistance > 0.0f && e.DistanceToCamera > e.LODMaxDistance)
            {
                ++beyondLod;
            }
        }

        // A dead compute stage outranks every other explanation: the dispatch
        // helpers bail silently, so nothing downstream can be trusted.
        for (const ParticleEmitterFacts& e : emitters)
        {
            if (!e.DeadGpuStages.empty())
            {
                std::string stages;
                for (const std::string& stage : e.DeadGpuStages)
                {
                    stages += stages.empty() ? stage : (", " + stage);
                }
                return "GPU compute stage(s) FAILED TO COMPILE on this backend: " + stages +
                       ". Those dispatches return early and silently, so the emitter can never produce a "
                       "particle and every downstream symptom is a consequence, not a cause. Fix the shader "
                       "first; nothing else here is evidence.";
            }
        }

        if (cpuEmitters == 0)
        {
            if (gpuAlive > 0)
            {
                return "Every emitter is GPU-driven and the GPU simulation reports live particles. The CPU "
                       "batch renderer is not involved, so its counters say nothing: look at the compute "
                       "chain (emit/simulate/compact/build-indirect) and the INDIRECT draw, not the CPU "
                       "billboard path.";
            }
            return "Every emitter is GPU-driven and the GPU alive count is zero or unavailable. Nothing "
                   "is being simulated on-device, so nothing can be drawn — investigate the compute "
                   "emit/simulate chain. The CPU counters here are empty BY DESIGN and are not evidence.";
        }

        if (alive == 0)
        {
            if (playing == 0)
            {
                return "Every emitter is stopped (Playing == false), so nothing is simulated and nothing "
                       "is submitted. This is a simulation state, not a rendering fault.";
            }
            if (beyondLod == emitters.size())
            {
                return "Every emitter is past its LODMaxDistance, so spawning is suppressed. Move the "
                       "camera closer or raise LODMaxDistance before reading anything into a zero.";
            }
            return "Emitters exist and are playing but no particle is alive, so there is nothing to "
                   "submit. The fault is in simulation (emission/lifetime), not in rendering.";
        }

        // Reaching here means at least one CPU emitter exists (the all-GPU case
        // returned above), so a zero draw count is genuinely unexplained.
        if (submission.DrawCalls == 0)
        {
            return "Particles are ALIVE but the batch renderer submitted ZERO draw calls this frame. "
                   "The break is upstream of the GPU: submission, not rasterisation. Rendering state, "
                   "shaders and bindings are all downstream of a draw that never happened.";
        }

        return "The batch renderer submitted draws this frame. If nothing is visible, the break is at "
               "or after rasterisation — pipeline state, blending, depth, the bound attachments or the "
               "shader — NOT submission.";
    }

    [[nodiscard]] inline nlohmann::json ParticleStatsJson(const ParticleSubmissionFacts& submission,
                                                          const std::vector<ParticleEmitterFacts>& emitters)
    {
        nlohmann::json emittersJson = nlohmann::json::array();
        u32 alive = 0;
        for (const ParticleEmitterFacts& e : emitters)
        {
            nlohmann::json one;
            one["name"] = e.Name;
            one["entity"] = e.EntityId;
            one["aliveCount"] = e.AliveCount;
            one["maxParticles"] = e.MaxParticles;
            one["playing"] = e.Playing;
            one["useGPU"] = e.UseGPU;
            one["gpuAliveCount"] = e.GpuAliveCount;
            one["deadGpuStages"] = e.DeadGpuStages;
            one["gpuCounters"] = { { "alive", e.GpuCounterAlive },
                                   { "dead", e.GpuCounterDead },
                                   { "emit", e.GpuCounterEmit } };
            one["gpuIndirectInstanceCount"] = e.GpuIndirectInstanceCount;
            one["lastGpuEmitRequest"] = e.LastGpuEmitRequest;
            one["lodSpawnRateMultiplier"] = e.LodSpawnRateMultiplier;
            one["gpuParticleSlotsWritten"] = e.GpuParticleSlotsWritten;
            one["gpuParticleSlotsSampled"] = e.GpuParticleSlotsSampled;
            one["renderMode"] = e.RenderMode;
            one["distanceToCamera"] = e.DistanceToCamera;
            one["lodMaxDistance"] = e.LODMaxDistance;
            one["beyondLOD"] = e.LODMaxDistance > 0.0f && e.DistanceToCamera > e.LODMaxDistance;
            emittersJson.push_back(std::move(one));
            // Same rule as the verdict: a GPU emitter's CPU pool is empty by
            // design and must not inflate — or deflate — the total.
            if (!e.UseGPU)
            {
                alive += e.AliveCount;
            }
        }

        nlohmann::json j;
        j["emitterCount"] = static_cast<u32>(emitters.size());
        j["aliveTotal"] = alive; // CPU-simulated particles only — see gpuAliveCount per emitter.
        j["submission"] = { { "drawCalls", submission.DrawCalls }, { "instanceCount", submission.InstanceCount } };
        j["emitters"] = std::move(emittersJson);
        j["verdict"] = ExplainParticleFrame(submission, emitters);
        return j;
    }
} // namespace OloEditor::MCP
