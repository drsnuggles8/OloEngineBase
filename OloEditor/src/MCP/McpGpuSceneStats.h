#pragma once

// Pure JSON shaping behind olo_gpu_scene_stats (issue #607).
//
// olo_rt_scene_stats carries a `gpuScene` block, but only the live counts the
// ray tracer needs beside its TLAS. The registry's own accounting — slots
// against GPU capacity, the free list and the retirement queue, upload bytes,
// growth events, draw-link resolution — was visible only in the profiler
// panel. This is that accounting, read from ONE completed profiler frame so
// every number in a reply describes the same frame.
//
// A table's slots partition exactly: live + free + retiring + exhausted, where
// an exhausted slot ran out of generations and is tombstoned for good. The
// report derives `exhaustedSlots` from that identity and lists any identity
// that does not hold under `invariantViolations`, because a registry whose
// counts do not add up is a producer bug, not a scene.

#include "MCP/McpRayTracingStats.h"
#include "MCP/McpStatsSnapshot.h"

#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <span>
#include <string>

namespace OloEngine::MCP::GpuSceneStats
{
    using Json = nlohmann::json;

    struct DrawLinks
    {
        // Draw links resolved / unresolved at the commit step (issue #994).
        u32 Linked = 0;
        u32 Unlinked = 0;
        // Draws that read a record at dispatch, and draws that fell back to
        // their own per-draw data.
        u32 Consumed = 0;
        u32 FellBack = 0;
    };

    // Upload activity summed over the profiler's recent frames. An edit uploads
    // on ONE frame, and a previous-frame sample read over MCP has almost always
    // moved past it, so "did a one-light edit upload one light record" is only
    // answerable over a window.
    struct RecentWindow
    {
        u32 Frames = 0;
        u32 FramesWithUploads = 0;
        u32 BufferGrowthEvents = 0;
        u64 Instances = 0;
        u64 Geometries = 0;
        u64 Materials = 0;
        u64 Lights = 0;
        u64 Environments = 0;
    };

    [[nodiscard]] inline RecentWindow SummarizeWindow(std::span<const OloEngine::GPUSceneFrameStats> frames)
    {
        RecentWindow window;
        window.Frames = static_cast<u32>(frames.size());
        for (const auto& frame : frames)
        {
            if (frame.m_UploadBytes > 0)
                ++window.FramesWithUploads;
            window.BufferGrowthEvents += frame.m_BufferGrowthEvents;
            window.Instances += frame.m_Instances.m_UploadBytes;
            window.Geometries += frame.m_Geometries.m_UploadBytes;
            window.Materials += frame.m_Materials.m_UploadBytes;
            window.Lights += frame.m_Lights.m_UploadBytes;
            window.Environments += frame.m_Environments.m_UploadBytes;
        }
        return window;
    }

    struct Snapshot
    {
        StatsSnapshot::State State;
        OloEngine::GPUSceneFrameStats Stats;
        DrawLinks Draws;
        RecentWindow Recent;
        // The profiler frame the sample came from, so two calls can tell
        // whether they read the same frame.
        u32 RenderFrameNumber = 0;
    };

    // GPU-side bytes per record, per kind. The tables are arrays of these
    // structs, so capacity * stride is the allocation.
    struct RecordStrides
    {
        u64 Instance = sizeof(OloEngine::GPUSceneInstance);
        u64 Geometry = sizeof(OloEngine::GPUSceneGeometry);
        u64 Material = sizeof(OloEngine::GPUSceneMaterial);
        u64 Light = sizeof(OloEngine::GPUSceneLight);
        u64 Environment = sizeof(OloEngine::GPUSceneEnvironment);
    };

    namespace Detail
    {
        [[nodiscard]] inline Json KindReport(const char* name, const OloEngine::GPUSceneKindStats& kind,
                                             u64 recordBytes, Json& violations)
        {
            const u64 accounted = static_cast<u64>(kind.m_Live) + kind.m_FreeSlots + kind.m_RetiredSlots;
            if (accounted > kind.m_SlotCount)
            {
                violations.push_back(std::string(name) + ": live + free + retiring (" + std::to_string(accounted) +
                                     ") exceeds slots (" + std::to_string(kind.m_SlotCount) + ")");
            }
            if (kind.m_SlotCount > kind.m_BufferCapacity && kind.m_BufferCapacity > 0)
            {
                violations.push_back(std::string(name) + ": slots (" + std::to_string(kind.m_SlotCount) +
                                     ") exceed GPU capacity (" + std::to_string(kind.m_BufferCapacity) + ")");
            }
            const u64 exhausted = accounted <= kind.m_SlotCount ? kind.m_SlotCount - accounted : 0u;

            Json out{
                { "live", kind.m_Live },
                { "slots", kind.m_SlotCount },
                { "bufferCapacity", kind.m_BufferCapacity },
                { "freeSlots", kind.m_FreeSlots },
                { "retiringSlots", kind.m_RetiredSlots },
                { "exhaustedSlots", exhausted },
                { "uploadBytes", kind.m_UploadBytes },
                { "recordBytes", recordBytes },
                { "capacityBytes", static_cast<u64>(kind.m_BufferCapacity) * recordBytes },
                { "liveBytes", static_cast<u64>(kind.m_Live) * recordBytes },
            };
            // Null rather than 0 with no buffer: "no table yet" is not "empty table".
            if (kind.m_BufferCapacity > 0)
                out["occupancy"] = static_cast<f64>(kind.m_Live) / static_cast<f64>(kind.m_BufferCapacity);
            else
                out["occupancy"] = nullptr;
            return out;
        }
    } // namespace Detail

    [[nodiscard("this builds the response; it does not send it")]] inline Json
    BuildReport(const Snapshot& snapshot, const RecordStrides& strides = {})
    {
        Json out = StatsSnapshot::ToJson(snapshot.State);
        out["renderFrameNumber"] = snapshot.RenderFrameNumber;
        if (StatsSnapshot::Status(snapshot.State) != "ready")
            return out;

        const auto& stats = snapshot.Stats;
        Json violations = Json::array();

        out["kinds"] = Json{
            { "instances", Detail::KindReport("instances", stats.m_Instances, strides.Instance, violations) },
            { "geometries", Detail::KindReport("geometries", stats.m_Geometries, strides.Geometry, violations) },
            { "materials", Detail::KindReport("materials", stats.m_Materials, strides.Material, violations) },
            { "lights", Detail::KindReport("lights", stats.m_Lights, strides.Light, violations) },
            { "environments",
              Detail::KindReport("environments", stats.m_Environments, strides.Environment, violations) },
        };

        u64 capacityBytes = 0;
        u64 liveBytes = 0;
        u64 uploadSum = 0;
        for (const auto& [name, kind] : out["kinds"].items())
        {
            capacityBytes += kind.at("capacityBytes").get<u64>();
            liveBytes += kind.at("liveBytes").get<u64>();
            uploadSum += kind.at("uploadBytes").get<u64>();
        }
        if (uploadSum != stats.m_UploadBytes)
        {
            violations.push_back("uploadBytes total (" + std::to_string(stats.m_UploadBytes) +
                                 ") is not the sum of the per-kind figures (" + std::to_string(uploadSum) + ")");
        }

        out["frame"] = Json{
            { "uploadBytes", stats.m_UploadBytes },
            { "bufferGrowthEvents", stats.m_BufferGrowthEvents },
            { "extractionMs", stats.m_ExtractionTimeMs },
        };
        // Records = bytes / stride: uploads are whole records (dirty ranges of
        // record indices), so this is the count a reader compares with the edits
        // they made.
        const auto& recent = snapshot.Recent;
        const auto records = [](u64 bytes, u64 stride) -> u64
        { return stride > 0 ? bytes / stride : 0u; };
        out["recent"] = Json{
            { "frames", recent.Frames },
            { "framesWithUploads", recent.FramesWithUploads },
            { "bufferGrowthEvents", recent.BufferGrowthEvents },
            { "uploadBytes",
              Json{
                  { "instances", recent.Instances },
                  { "geometries", recent.Geometries },
                  { "materials", recent.Materials },
                  { "lights", recent.Lights },
                  { "environments", recent.Environments },
              } },
            { "uploadRecords",
              Json{
                  { "instances", records(recent.Instances, strides.Instance) },
                  { "geometries", records(recent.Geometries, strides.Geometry) },
                  { "materials", records(recent.Materials, strides.Material) },
                  { "lights", records(recent.Lights, strides.Light) },
                  { "environments", records(recent.Environments, strides.Environment) },
              } },
        };
        out["memory"] = Json{
            { "capacityBytes", capacityBytes },
            { "liveBytes", liveBytes },
        };
        out["draws"] = Json{
            { "linked", snapshot.Draws.Linked },
            { "unlinked", snapshot.Draws.Unlinked },
            { "consumed", snapshot.Draws.Consumed },
            { "fellBack", snapshot.Draws.FellBack },
        };

        Json byCategory = Json::object();
        for (sizet i = 0; i < OloEngine::GPUSceneUnsupportedCategoryCount; ++i)
        {
            byCategory[RayTracingStats::UnsupportedCategoryKey(static_cast<OloEngine::GPUSceneUnsupportedCategory>(i))] =
                stats.m_UnsupportedCounts[i];
        }
        out["notStagedTotal"] = stats.m_UnsupportedTotal;
        out["notStagedByCategory"] = std::move(byCategory);

        const auto& foliage = stats.m_Foliage;
        out["foliage"] = Json{
            { "canonicalInstances", foliage.m_CanonicalInstances },
            { "meshCardInstances", foliage.m_MeshCardInstances },
            { "impostorInstances", foliage.m_ImpostorInstances },
            { "authoredMeshInstances", foliage.m_AuthoredMeshInstances },
            { "unsupportedInstances", foliage.m_UnsupportedInstances },
            { "unsupportedVariants", foliage.m_UnsupportedVariants },
            { "spatialGroups", foliage.m_SpatialGroups },
        };

        // canonicalInstances == surfacesWithHistory + surfacesWithoutHistory by
        // construction (GPUSceneAnimatedStats).
        const auto& animated = stats.m_Animated;
        out["animated"] = Json{
            { "canonicalEntities", animated.m_CanonicalEntities },
            { "canonicalInstances", animated.m_CanonicalInstances },
            { "surfacesWithHistory", animated.m_SurfacesWithHistory },
            { "surfacesWithoutHistory", animated.m_SurfacesWithoutHistory },
            { "unsupportedInstances", animated.m_UnsupportedInstances },
            { "unsupportedVariants", animated.m_UnsupportedVariants },
        };
        if (static_cast<u64>(animated.m_SurfacesWithHistory) + animated.m_SurfacesWithoutHistory !=
            animated.m_CanonicalInstances)
        {
            violations.push_back("animated: surfacesWithHistory + surfacesWithoutHistory is not canonicalInstances");
        }

        out["invariantViolations"] = std::move(violations);
        return out;
    }
} // namespace OloEngine::MCP::GpuSceneStats
