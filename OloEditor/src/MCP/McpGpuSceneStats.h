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
//
// The not-staged, foliage and animated blocks are shared with
// olo_rt_scene_stats' gpuScene block, so the two tools cannot drift apart.

#include "MCP/McpStatsSnapshot.h"

#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <array>
#include <string>

namespace OloEngine::MCP::GpuSceneStats
{
    using Json = nlohmann::json;

    // The JSON key for each diagnostics category.
    //
    // A switch rather than a table lookup so an unnamed category is at least a
    // -Wswitch warning rather than an out-of-range read. It is NOT a compile
    // error: adding GPUSceneUnsupportedCategory::Groom (#1246) compiled fine and
    // fell through to "unknown", and what caught it was
    // McpRayTracingStats.EveryDiagnosticsCategoryGetsItsOwnKey. That test is the
    // guard; keep it in mind when adding a category, because the build will not
    // stop you.
    [[nodiscard]] constexpr const char* UnsupportedCategoryKey(OloEngine::GPUSceneUnsupportedCategory category)
    {
        using enum OloEngine::GPUSceneUnsupportedCategory;
        switch (category)
        {
            case Virtualized:
                return "virtualized";
            case SoftwareRaster:
                return "softwareRaster";
            case Procedural:
                return "procedural";
            case Terrain:
                return "terrain";
            case Foliage:
                return "foliage";
            case Particles:
                return "particles";
            case Fluids:
                return "fluids";
            case Skinned:
                return "skinned";
            case LegacyModel:
                return "legacyModel";
            case LegacySubmesh:
                return "legacySubmesh";
            case Tiles:
                return "tiles";
            case Cloth:
                return "cloth";
            case Groom:
                return "groom";
            case NotExtractable:
                return "notExtractable";
            case Count:
                break;
        }
        return "unknown";
    }

    [[nodiscard]] inline Json NotStagedByCategoryJson(const OloEngine::GPUSceneFrameStats& stats)
    {
        Json byCategory = Json::object();
        for (sizet i = 0; i < OloEngine::GPUSceneUnsupportedCategoryCount; ++i)
            byCategory[UnsupportedCategoryKey(static_cast<OloEngine::GPUSceneUnsupportedCategory>(i))] =
                stats.m_UnsupportedCounts[i];
        return byCategory;
    }

    // Foliage rides its own vertex stream, so "Foliage" in the not-staged
    // categories only counts SYSTEMS that skip the records. This says what is
    // inside them: how many plants carry canonical identity, how each is
    // represented, and what the raster path cannot draw (issue #1230).
    [[nodiscard]] inline Json FoliageJson(const OloEngine::GPUSceneFoliageStats& foliage)
    {
        return Json{
            { "canonicalInstances", foliage.m_CanonicalInstances },
            { "meshCardInstances", foliage.m_MeshCardInstances },
            { "impostorInstances", foliage.m_ImpostorInstances },
            { "authoredMeshInstances", foliage.m_AuthoredMeshInstances },
            { "unsupportedInstances", foliage.m_UnsupportedInstances },
            { "unsupportedVariants", foliage.m_UnsupportedVariants },
            { "spatialGroups", foliage.m_SpatialGroups },
        };
    }

    // Animated surfaces (issue #1228). "Skinned" in the not-staged categories
    // means an animated entity that reached submission and produced no record
    // at all. This block is the positive half: what the records DID take, and
    // how many of those carry a previous pose a velocity may legitimately be
    // measured against.
    //
    // canonicalInstances == surfacesWithHistory + surfacesWithoutHistory by
    // construction; a reader seeing that fail is looking at a producer bug, not
    // at a scene.
    [[nodiscard]] inline Json AnimatedJson(const OloEngine::GPUSceneAnimatedStats& animated)
    {
        return Json{
            { "canonicalEntities", animated.m_CanonicalEntities },
            { "canonicalInstances", animated.m_CanonicalInstances },
            { "surfacesWithHistory", animated.m_SurfacesWithHistory },
            { "surfacesWithoutHistory", animated.m_SurfacesWithoutHistory },
            { "unsupportedInstances", animated.m_UnsupportedInstances },
            { "unsupportedVariants", animated.m_UnsupportedVariants },
        };
    }

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
    // moved past it, so "did my edit reach the GPU" is only answerable over a
    // window.
    //
    // Count frames per kind, not just bytes: the bytes are backend-shaped. GL
    // writes the dirty records; Vulkan republishes every slot of the table (live,
    // free and retiring) on any edit so no submitted frame reads a half-written
    // one. An upload frame is the same on both: edits applied in one frame share
    // it, and growth and GPU initialisation upload too.
    struct KindWindow
    {
        u32 UploadFrames = 0;
        u64 UploadBytes = 0;
    };

    struct RecentWindow
    {
        u32 Frames = 0;
        u32 FramesWithUploads = 0;
        u32 BufferGrowthEvents = 0;
        KindWindow Instances;
        KindWindow Geometries;
        KindWindow Materials;
        KindWindow Lights;
        KindWindow Environments;

        void Add(const OloEngine::GPUSceneFrameStats& frame)
        {
            const auto add = [](KindWindow& window, const OloEngine::GPUSceneKindStats& kind)
            {
                if (kind.m_UploadBytes > 0)
                    ++window.UploadFrames;
                window.UploadBytes += kind.m_UploadBytes;
            };
            ++Frames;
            if (frame.m_UploadBytes > 0)
                ++FramesWithUploads;
            BufferGrowthEvents += frame.m_BufferGrowthEvents;
            add(Instances, frame.m_Instances);
            add(Geometries, frame.m_Geometries);
            add(Materials, frame.m_Materials);
            add(Lights, frame.m_Lights);
            add(Environments, frame.m_Environments);
        }
    };

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
        struct KindRow
        {
            const char* Name;
            const OloEngine::GPUSceneKindStats& Kind;
            const KindWindow& Recent;
            u64 RecordBytes;

            [[nodiscard]] u64 CapacityBytes() const
            {
                return static_cast<u64>(Kind.m_BufferCapacity) * RecordBytes;
            }
            [[nodiscard]] u64 LiveBytes() const
            {
                return static_cast<u64>(Kind.m_Live) * RecordBytes;
            }
        };

        [[nodiscard]] inline Json KindReport(const KindRow& row, Json& violations)
        {
            const auto& kind = row.Kind;
            const std::string name = row.Name;
            const u64 accounted = static_cast<u64>(kind.m_Live) + kind.m_FreeSlots + kind.m_RetiredSlots;
            if (accounted > kind.m_SlotCount)
            {
                violations.push_back(name + ": live + free + retiring (" + std::to_string(accounted) +
                                     ") exceeds slots (" + std::to_string(kind.m_SlotCount) + ")");
            }
            // Capacity 0 with slots is a table whose records can never reach
            // the GPU, which is the case this list exists for.
            if (kind.m_SlotCount > kind.m_BufferCapacity)
            {
                violations.push_back(name + ": slots (" + std::to_string(kind.m_SlotCount) +
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
                { "recordBytes", row.RecordBytes },
                { "capacityBytes", row.CapacityBytes() },
                { "liveBytes", row.LiveBytes() },
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
        const auto& recent = snapshot.Recent;
        const std::array<Detail::KindRow, 5> rows{ {
            { "instances", stats.m_Instances, recent.Instances, strides.Instance },
            { "geometries", stats.m_Geometries, recent.Geometries, strides.Geometry },
            { "materials", stats.m_Materials, recent.Materials, strides.Material },
            { "lights", stats.m_Lights, recent.Lights, strides.Light },
            { "environments", stats.m_Environments, recent.Environments, strides.Environment },
        } };

        Json violations = Json::array();
        Json kinds = Json::object();
        Json recentFrames = Json::object();
        Json recentBytes = Json::object();
        Json recentRecords = Json::object();
        u64 capacityBytes = 0;
        u64 liveBytes = 0;
        u64 uploadSum = 0;
        for (const auto& row : rows)
        {
            kinds[row.Name] = Detail::KindReport(row, violations);
            capacityBytes += row.CapacityBytes();
            liveBytes += row.LiveBytes();
            uploadSum += row.Kind.m_UploadBytes;
            recentFrames[row.Name] = row.Recent.UploadFrames;
            recentBytes[row.Name] = row.Recent.UploadBytes;
            recentRecords[row.Name] = row.RecordBytes > 0 ? row.Recent.UploadBytes / row.RecordBytes : 0u;
        }
        out["kinds"] = std::move(kinds);
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
        out["recent"] = Json{
            { "frames", recent.Frames },
            { "framesWithUploads", recent.FramesWithUploads },
            { "bufferGrowthEvents", recent.BufferGrowthEvents },
            { "uploadFrames", std::move(recentFrames) },
            { "uploadBytes", std::move(recentBytes) },
            { "uploadRecords", std::move(recentRecords) },
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
        out["notStagedTotal"] = stats.m_UnsupportedTotal;
        out["notStagedByCategory"] = NotStagedByCategoryJson(stats);
        out["foliage"] = FoliageJson(stats.m_Foliage);
        out["animated"] = AnimatedJson(stats.m_Animated);

        const auto& animated = stats.m_Animated;
        if (static_cast<u64>(animated.m_SurfacesWithHistory) + animated.m_SurfacesWithoutHistory !=
            animated.m_CanonicalInstances)
        {
            violations.push_back("animated: surfacesWithHistory + surfacesWithoutHistory is not canonicalInstances");
        }

        out["invariantViolations"] = std::move(violations);
        return out;
    }
} // namespace OloEngine::MCP::GpuSceneStats
