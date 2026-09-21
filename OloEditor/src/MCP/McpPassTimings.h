#pragma once

// Pure JSON shaping for the olo_perf_pass_timings MCP tool (issue #316: split
// whole-frame GPU time by render-graph pass — Shadow vs Scene vs GTAO vs Bloom
// vs ToneMap...). The MCP handler in McpTools.cpp gathers, inside a MarshalRead:
//   - per-pass GPU times from GPUPassTimerPool (always-on GL_TIMESTAMP pairs,
//     resolved 1-3 frames after issue),
//   - per-pass CPU times from the live RenderGraph's last execution timings,
//   - frame totals from RendererProfiler,
// pre-resolves them into the plain input structs below, and hands them here.
//
// Keeping the shaping in free functions over engine-free inputs means it
// unit-tests directly against synthetic data — the test binary compiles this
// header but deliberately NOT McpTools.cpp. Mirrors the sibling pattern of
// McpFrameBreakdown.h / McpRenderExplain.h.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GPUTimingStatus.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::MCP::PassTimings
{
    using Json = nlohmann::json;

    // Matches GPUPassTimerPool::kSlotCount — the number of in-flight
    // timestamp slots. An age at or beyond this means a slot was dropped
    // (GPU fell more than a full ring's worth of frames behind) rather than
    // the normal 1-3 frame resolve latency.
    inline constexpr u64 kGpuResultsStaleThreshold = 4;

    // One render-graph pass's GPU time (execution order preserved).
    struct GpuPassEntry
    {
        std::string Name;
        // The measurement AND whether it is one (#1337). An entry whose status
        // is not Valid publishes `gpuMs: null` and a `gpuStatus` saying why —
        // never 0, which a caller reads as a free pass.
        GpuTimingSample Sample{};
        // Stated by the producer, not inferred from a '/' in the name. A pass
        // whose own name contains a slash used to be mistaken for a sub-pass
        // and silently dropped out of passGpuTotalMs.
        bool IsSubPass = false;
        std::string ParentName;
    };

    // One render-graph pass's CPU (submission/dispatch) time.
    struct CpuPassEntry
    {
        std::string Name;
        f64 CpuMs = 0.0;
    };

    // The parallel command recorder's frame telemetry (issue #806, ADR 0011
    // amendment (92)) - a plain mirror of RendererAPI::ParallelRecordingFrameStats
    // so this header stays engine-free. The handler copies the profiler's
    // last-completed-frame snapshot in. All zero on OpenGL, whose facade
    // default reports nothing; on Vulkan with OLO_VK_PARALLEL_RECORDING off
    // only InlineRegions counts (every RecordParallel call ran inline).
    struct ParallelRegionStats
    {
        std::string PassName;
        bool Parallel = false;
        f64 WorkerRecordMs = 0.0;
        f64 RegionWallMs = 0.0;
        f64 JoinWaitMs = 0.0;
        std::vector<f64> ItemRecordMs;
        std::vector<std::string> ItemPassNames;
        // Optional micro-cost probe: OLO_VK_RECORDING_COSTS=1 at process start.
        f64 SelectionSeedMs = 0.0;
        f64 AttachmentPrepareMs = 0.0;
        f64 SampledImagePrepareMs = 0.0;
        f64 PipelineLookupMs = 0.0;
        // Caller-side seeding of every item's frontend context (dispatcher
        // caches plus the per-item copy of each shared upload object).
        f64 FrontendPrepareMs = 0.0;
    };

    struct ParallelRecordingStats
    {
        u32 Regions = 0;             // RecordParallel calls that forked onto task workers
        u32 InlineRegions = 0;       // RecordParallel calls that ran inline on the render thread
        u32 SecondariesExecuted = 0; // secondary command buffers executed into the primary
        u32 MergeConflicts = 0;      // subresources two items transitioned differently (rule 5): a bug in the forking pass
        u32 DeclinedGroups = 0;      // whole-pass groups prepared, then recorded sequentially anyway (members prepared twice)
        f64 WorkerRecordMs = 0.0;    // sum of per-item recording time across workers
        f64 RegionWallMs = 0.0;      // sum of fork-to-join wall time on the render thread
        f64 JoinWaitMs = 0.0;
        std::vector<ParallelRegionStats> RegionTimings;
    };

    // The async-compute queue's telemetry (issue #808) — a plain mirror of
    // RendererAPI::AsyncComputeFrameStats so this header stays engine-free.
    // All zero on OpenGL. On Vulkan, BatchesOnComputeQueue == 0 with a
    // non-empty DeclineReason is the degrade path saying which reason applied,
    // which is the whole point of surfacing it: a session that expected
    // overlap and got none gets its answer here rather than from a debugger.
    struct AsyncComputeStats
    {
        u32 BatchesOnComputeQueue = 0;
        u32 BatchesDeclined = 0;
        u32 OwnershipTransfers = 0;
        u32 ComputeSubmits = 0;
        std::string DeclineReason;
    };

    struct FrameTotals
    {
        f64 FrameTimeMs = 0.0;
        f64 CpuMs = 0.0;
        // Whole-frame GPU time (timestamp span) AND whether it is a
        // measurement (#1337).
        GpuTimingSample Gpu{};
        // CPU time blocked on GPU/present sync, split by cause: a fence wait
        // means the GPU is behind, a present wait means the display is pacing
        // you. GpuWaitMs stays as their sum for callers that want the single
        // "GPU-bound" signal.
        f64 GpuWaitMs = 0.0;
        f64 FenceWaitMs = 0.0;
        f64 PresentWaitMs = 0.0;
        // How many frames old the resolved GPU numbers are (0 = nothing
        // resolved yet). GPU results always lag 1-3 frames behind the CPU
        // numbers; transient name mismatches between the two lists are normal.
        u64 GpuResultsAgeFrames = 0;
        // The frame the GPU numbers describe, and the frame the CPU numbers
        // describe. Published so a reader can confirm the pairing rather than
        // trust the age arithmetic (#1337 criterion 4: measurement frame IDs).
        u64 GpuMeasurementFrameId = 0;
        u64 CurrentFrameId = 0;
        // Timestamp slots discarded since the pool started, each one a frame
        // that was never measured at all, and separately the frames the backend
        // declined to stamp. Two counters because they are two faults: the GPU
        // is behind, versus the instrument is not working.
        u32 GpuDroppedSlots = 0;
        u32 GpuUnstampedFrames = 0;
        ParallelRecordingStats ParallelRecording;
        AsyncComputeStats AsyncCompute;
    };

    // Millisecond values are sub-ms for many passes — keep 3 decimals.
    [[nodiscard]] inline f64 Round3(f64 v)
    {
        return std::round(v * 1000.0) / 1000.0;
    }

    // Emit one measurement as the PAIR it is (#1337): `<key>` carries the number
    // when the sample is valid and JSON `null` when it is not, and
    // `<key>Status` always says which. Null rather than a missing key so a
    // caller can rely on the field existing; null rather than 0 because 0 is a
    // legal pass time and was how this whole class of defect stayed invisible.
    inline void EmitSample(Json& target, const char* msKey, const char* statusKey, const GpuTimingSample& sample)
    {
        if (sample.IsValid())
            target[msKey] = Round3(sample.GpuMs);
        else
            target[msKey] = nullptr;
        target[statusKey] = std::string(ToString(sample.Status));
    }

    // Join the GPU list (primary, execution order) with CPU times by pass name.
    //
    // CPU-only passes — ones the graph executed that the GPU list does not
    // carry, because the timer pool overflowed or the topology changed between
    // the resolved GPU frame and the current CPU frame — are appended with
    // `gpuMs: null` and status `notTimed`. They used to be appended with
    // `gpuMs: 0`, which is the same claim as "this pass is free" (#1337).
    //
    // A sub-pass entry is a bracket stamped INSIDE its parent
    // (GPUPassTimerPool::BeginSubPass — e.g. the ScenePass DepthPrepass/Color
    // split, #316): it is attached to the most recent top-level entry with the
    // parent's name as subPasses[{name, gpuMs, gpuStatus}] and does NOT count
    // toward passGpuTotalMs, because its time is already inside the parent's
    // bracket. An orphan sub-entry (parent not GPU-timed this frame) is kept as
    // a top-level entry under its full name rather than dropped.
    //
    // Which entries are sub-passes comes from `IsSubPass`, set by the producer.
    // It used to be re-derived here by looking for a '/' in the name, so a pass
    // whose own name contained a slash was misfiled as somebody's sub-pass and
    // its GPU time silently left out of the frame total.
    [[nodiscard]] inline Json BuildPassTimings(const std::vector<GpuPassEntry>& gpuPasses,
                                               const std::vector<CpuPassEntry>& cpuPasses,
                                               const FrameTotals& totals)
    {
        Json passes = Json::array();
        f64 passGpuTotal = 0.0;
        // A total assembled out of a list with holes is a LOWER BOUND, and says
        // so rather than presenting itself as the frame's pass time.
        u32 unmeasuredPasses = 0;

        std::vector<bool> cpuUsed(cpuPasses.size(), false);
        const auto findCpuMs = [&cpuPasses, &cpuUsed](const std::string& name) -> f64
        {
            for (sizet i = 0; i < cpuPasses.size(); ++i)
            {
                if (!cpuUsed[i] && cpuPasses[i].Name == name)
                {
                    cpuUsed[i] = true;
                    return cpuPasses[i].CpuMs;
                }
            }
            return 0.0;
        };

        // Index of the last top-level entry per pass name, for sub-pass
        // attachment (the pool allocates parent-before-sub, so a forward walk
        // always sees the parent first).
        const auto findParentIndex = [&passes](const std::string& parentName) -> std::optional<sizet>
        {
            for (sizet i = passes.size(); i > 0; --i)
            {
                if (passes[i - 1]["pass"].get<std::string>() == parentName)
                    return i - 1;
            }
            return std::nullopt;
        };

        for (const auto& gpuPass : gpuPasses)
        {
            if (gpuPass.IsSubPass)
            {
                if (const auto parentIdx = findParentIndex(gpuPass.ParentName))
                {
                    Json& parent = passes[*parentIdx];
                    if (!parent.contains("subPasses"))
                        parent["subPasses"] = Json::array();
                    // The leaf name: the producer publishes "<Parent>/<leaf>",
                    // and the parent is already named by the entry this hangs
                    // off, so repeating it here would be noise.
                    const auto slash = gpuPass.Name.rfind('/');
                    Json sub{ { "name", slash == std::string::npos ? gpuPass.Name : gpuPass.Name.substr(slash + 1) } };
                    EmitSample(sub, "gpuMs", "gpuStatus", gpuPass.Sample);
                    parent["subPasses"].push_back(std::move(sub));
                    continue;
                }
                // Orphan sub-entry: its parent was not GPU-timed this frame, so
                // there is no bracket for its time to be double-counted inside.
                // Publish it top-level under its full name and DO count it.
            }
            if (gpuPass.Sample.IsValid())
                passGpuTotal += gpuPass.Sample.GpuMs;
            else
                ++unmeasuredPasses;

            Json entry{ { "pass", gpuPass.Name }, { "cpuMs", Round3(findCpuMs(gpuPass.Name)) } };
            EmitSample(entry, "gpuMs", "gpuStatus", gpuPass.Sample);
            passes.push_back(std::move(entry));
        }

        for (sizet i = 0; i < cpuPasses.size(); ++i)
        {
            if (cpuUsed[i])
                continue;
            // Executed on the CPU this frame, absent from the GPU list.
            // `notTimed`, not 0 — but deliberately NOT counted in
            // unmeasuredPasses.
            //
            // The GPU numbers describe a frame 1-3 older than the CPU ones, so
            // a name present in one list and not the other is the NORMAL
            // consequence of that lag (a pass that started or stopped running
            // in between), not a hole in the resolved frame's accounting. Its
            // GPU time is not missing from passGpuTotalMs; it belongs to a
            // different frame. Counting it here would degrade
            // passGpuTotalIsComplete and null out unattributedGpuMs on
            // perfectly healthy frames, which would make both fields useless
            // — the same overshoot as classifying an empty bracket a fault.
            //
            // A pass that really was in the resolved frame and went untimed
            // does reach unmeasuredPasses: the pool publishes it in the GPU
            // list as a NotTimed entry.
            Json entry{ { "pass", cpuPasses[i].Name }, { "cpuMs", Round3(cpuPasses[i].CpuMs) } };
            EmitSample(entry, "gpuMs", "gpuStatus", GpuTimingSample::Absent(GpuTimingStatus::NotTimed));
            passes.push_back(std::move(entry));
        }

        Json o;
        Json frame{ { "frameTimeMs", Round3(totals.FrameTimeMs) },
                    { "cpuMs", Round3(totals.CpuMs) },
                    { "gpuWaitMs", Round3(totals.GpuWaitMs) },
                    // Fence and present waits say opposite things about where
                    // the frame went, so they are answerable apart (#1337
                    // criterion 2). gpuWaitMs stays their sum.
                    { "fenceWaitMs", Round3(totals.FenceWaitMs) },
                    { "presentWaitMs", Round3(totals.PresentWaitMs) },
                    // Which frame each half of this report describes. The GPU
                    // numbers resolve 1-3 frames behind the CPU ones, so the
                    // two IDs are normally different — stated rather than
                    // inferred (#1337 criterion 4).
                    { "gpuMeasurementFrameId", totals.GpuMeasurementFrameId },
                    { "currentFrameId", totals.CurrentFrameId } };
        EmitSample(frame, "gpuMs", "gpuStatus", totals.Gpu);
        o["frame"] = std::move(frame);
        // Parallel command recorder telemetry (#806). Counters go out as
        // integers; the two times get the same 3-decimal rounding as every
        // other ms value here. The block is always present (zeros on a
        // backend that never forks) so a caller can rely on the key.
        const ParallelRecordingStats& pr = totals.ParallelRecording;
        o["parallelRecording"] = Json{ { "regions", pr.Regions },
                                       { "inlineRegions", pr.InlineRegions },
                                       { "secondariesExecuted", pr.SecondariesExecuted },
                                       { "mergeConflicts", pr.MergeConflicts },
                                       { "workerRecordMs", Round3(pr.WorkerRecordMs) },
                                       { "regionWallMs", Round3(pr.RegionWallMs) },
                                       { "joinWaitMs", Round3(pr.JoinWaitMs) },
                                       { "declinedGroups", pr.DeclinedGroups },
                                       { "regionTimings", Json::array() } };
        for (const auto& region : pr.RegionTimings)
        {
            Json items = Json::array();
            for (const f64 itemMs : region.ItemRecordMs)
                items.push_back(Round3(itemMs));
            o["parallelRecording"]["regionTimings"].push_back(
                Json{ { "pass", region.PassName }, { "parallel", region.Parallel }, { "workerRecordMs", Round3(region.WorkerRecordMs) }, { "regionWallMs", Round3(region.RegionWallMs) }, { "joinWaitMs", Round3(region.JoinWaitMs) }, { "itemRecordMs", std::move(items) }, { "itemPassNames", region.ItemPassNames }, { "selectionSeedMs", Round3(region.SelectionSeedMs) }, { "attachmentPrepareMs", Round3(region.AttachmentPrepareMs) }, { "sampledImagePrepareMs", Round3(region.SampledImagePrepareMs) }, { "pipelineLookupMs", Round3(region.PipelineLookupMs) }, { "frontendPrepareMs", Round3(region.FrontendPrepareMs) } });
        }
        // Async compute queue telemetry (#808). Always present, zeros on a
        // backend or device that never crosses queues, so a caller can rely on
        // the key — and `declineReason` is what a zero means.
        const AsyncComputeStats& ac = totals.AsyncCompute;
        o["asyncCompute"] = Json{ { "batchesOnComputeQueue", ac.BatchesOnComputeQueue },
                                  { "batchesDeclined", ac.BatchesDeclined },
                                  { "ownershipTransfers", ac.OwnershipTransfers },
                                  { "computeSubmits", ac.ComputeSubmits },
                                  { "declineReason", ac.DeclineReason } };
        // ---- The seven numbers, named for what each one IS (#1337 criterion 2).
        //
        // They were all already published, under keys that do not say which
        // kind of quantity they are. `workerRecordMs` in particular is a SUM
        // ACROSS WORKERS and routinely exceeds elapsed time — the checked-in
        // parallel-recording study measured 25.3-27.8 ms of worker CPU inside a
        // 2.4 ms wall — so a reader who takes it for elapsed frame time
        // concludes the change made things ten times slower. This block states
        // the kind next to the number so that reading is not available.
        //
        // Duplicated rather than renamed: the existing keys have consumers.
        f64 cpuPrepareMs = 0.0;
        for (const auto& region : pr.RegionTimings)
        {
            cpuPrepareMs += region.FrontendPrepareMs + region.SelectionSeedMs + region.AttachmentPrepareMs +
                            region.SampledImagePrepareMs;
        }
        o["recordingBreakdown"] =
            Json{ // ELAPSED. Real time on the render thread, fork to join.
                  { "elapsedRecordingWallMs", Round3(pr.RegionWallMs) },
                  // A SUM ACROSS WORKERS. Not elapsed, and legitimately larger
                  // than elapsedRecordingWallMs when work ran concurrently.
                  { "summedWorkerCpuMs", Round3(pr.WorkerRecordMs) },
                  // ELAPSED, inside the above: waiting for the last worker.
                  { "joinWaitMs", Round3(pr.JoinWaitMs) },
                  // A SUM. Caller-side setup before any item records, from the
                  // optional cost probe; 0 unless OLO_VK_RECORDING_COSTS=1.
                  { "summedCpuPrepareMs", Round3(cpuPrepareMs) },
                  // ELAPSED. CPU blocked on the frame fence: the GPU is behind.
                  { "fenceWaitMs", Round3(totals.FenceWaitMs) },
                  // ELAPSED. CPU blocked in SwapBuffers/vsync: display pacing.
                  { "presentWaitMs", Round3(totals.PresentWaitMs) },
                  // ELAPSED on the GPU timeline, or null when unmeasured.
                  { "gpuExecutionMs", totals.Gpu.IsValid() ? Json(Round3(totals.Gpu.GpuMs)) : Json(nullptr) },
                  { "gpuExecutionStatus", std::string(ToString(totals.Gpu.Status)) },
                  { "note",
                    "summedWorkerCpuMs and summedCpuPrepareMs are SUMS across workers/regions, not elapsed "
                    "time, and may exceed elapsedRecordingWallMs when work ran concurrently. Never add a "
                    "summed figure to an elapsed one." }
            };

        o["passes"] = std::move(passes);
        o["passGpuTotalMs"] = Round3(passGpuTotal);
        // How many passes could not contribute to that total. Non-zero makes
        // passGpuTotalMs a floor, and `passGpuTotalIsComplete` says so in one
        // field so a caller does not have to walk the list to find out.
        o["unmeasuredPasses"] = unmeasuredPasses;
        o["passGpuTotalIsComplete"] = unmeasuredPasses == 0;
        // GPU time inside the frame span but between/outside timed passes
        // (barriers, transient materialization, HZB rebuild, capture readbacks).
        //
        // Only derivable when the frame span itself is a measurement AND every
        // pass contributed: subtracting a partial pass total from a real frame
        // total attributes the missing passes' time to "unattributed", which
        // invents a finding. Null when it cannot be derived (#1337).
        if (totals.Gpu.IsValid() && unmeasuredPasses == 0)
        {
            // Negative values are clamped: pass spans can overlap the frame
            // span's edges by a timestamp tick.
            o["unattributedGpuMs"] = Round3(totals.Gpu.GpuMs > passGpuTotal ? totals.Gpu.GpuMs - passGpuTotal : 0.0);
        }
        else
        {
            o["unattributedGpuMs"] = nullptr;
        }
        o["gpuResultsAgeFrames"] = totals.GpuResultsAgeFrames;
        // Timestamp slots the pool discarded because the GPU fell more than a
        // ring behind. Each one is a frame that was never measured at all, so a
        // rising count means the published series has gaps in it.
        o["gpuDroppedSlots"] = totals.GpuDroppedSlots;
        o["gpuUnstampedFrames"] = totals.GpuUnstampedFrames;
        // One word for the whole report's trustworthiness, so a caller has
        // something to branch on without interpreting age, status and the
        // per-pass list together.
        o["gpuResultsStatus"] = std::string(ToString(totals.Gpu.Status));
        // GPUPassTimerPool has kGpuResultsStaleThreshold in-flight timestamp
        // slots; normal steady-state results lag 1-3 frames behind. An age at
        // or beyond that means the GPU fell far enough behind that a slot was
        // dropped rather than resolved (or nothing has resolved recently at
        // all) — the numbers above are from an old, possibly no-longer-
        // representative frame. Surfaced explicitly (issue #519) so a caller
        // doesn't have to know to compare gpuResultsAgeFrames against the
        // pool's slot count themselves.
        // Matches GPUPassTimerPool::FrameTimings::IsStale() exactly, including
        // the "nothing has ever resolved" arm. Age alone reported `false` for a
        // pool that had never produced a frame (its age is 0), so the MCP tool
        // and the benchmark export answered the same question differently.
        o["gpuResultsStale"] =
            totals.GpuMeasurementFrameId == 0 || totals.GpuResultsAgeFrames >= kGpuResultsStaleThreshold;
        return o;
    }
} // namespace OloEngine::MCP::PassTimings
