#pragma once

// Pure JSON shaping behind olo_rt_scene_stats (issue #978).
//
// The RT scene has three distinct "nothing to report" states and they need
// different fixes, so the envelope keeps them apart rather than flattening
// them into a page of zeros:
//
//   unavailable — this device/backend has no ray tracing, and `reason` says
//                 which of the four ways that can happen it was;
//   noData      — ray tracing is live but no TLAS has been built yet (a scene
//                 with no traceable geometry is exactly this);
//   ready       — the counters below are a real sample.
//
// A counter without the state beside it is the "a broken stats channel returns
// 1,847" failure: an all-zero payload from an unsupported GPU and an all-zero
// payload from an empty scene are the same bytes.

#include "MCP/McpStatsSnapshot.h"

#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/RayTracing/RayTracingStats.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"

#include <string>

namespace OloEngine::MCP::RayTracingStats
{
    using Json = nlohmann::json;

    struct Snapshot
    {
        StatsSnapshot::State State;
        OloEngine::RayTracing::Capabilities Capabilities;
        OloEngine::RayTracing::SceneStats Stats;
        // The canonical scene the acceleration structures are BUILT FROM
        // (issue #1065). Reported beside them because the two numbers are only
        // meaningful together: a TLAS with 49 instances is either the whole
        // scene or a rounding error on it, and the RT counters alone cannot
        // tell those apart. `GPUSceneAvailable` false means the renderer is not
        // up, not that the scene is empty.
        bool GPUSceneAvailable = false;
        OloEngine::GPUSceneFrameStats GPUScene;
    };

    // The JSON key for each diagnostics category. A switch, not a table lookup,
    // so a category added to the enum without a name here fails to compile
    // instead of silently reporting as "unknown".
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
            case NotExtractable:
                return "notExtractable";
            case Count:
                break;
        }
        return "unknown";
    }

    [[nodiscard("this builds the response; it does not send it")]] inline Json BuildReport(const Snapshot& snapshot)
    {
        namespace RT = OloEngine::RayTracing;

        Json out = StatsSnapshot::ToJson(snapshot.State);

        // The capability block is emitted even when the status is not "ready",
        // because "why is ray tracing unavailable" is the single most useful
        // thing this tool can answer and it is exactly the case where the
        // counters are absent.
        out["capability"] = Json{
            { "supported", snapshot.Capabilities.Supported },
            { "rayTracingPipeline", snapshot.Capabilities.RayTracingPipeline },
            { "reason", std::string(snapshot.Capabilities.ReasonText()) },
        };
        if (snapshot.Capabilities.Supported)
        {
            out["capability"]["properties"] = Json{
                { "minScratchOffsetAlignment", snapshot.Capabilities.Properties.MinScratchOffsetAlignment },
                { "maxInstanceCount", snapshot.Capabilities.Properties.MaxInstanceCount },
                { "maxGeometryCount", snapshot.Capabilities.Properties.MaxGeometryCount },
                { "maxPrimitiveCount", snapshot.Capabilities.Properties.MaxPrimitiveCount },
            };
        }

        // BEFORE the readiness early-return, on purpose. "No TLAS was built"
        // and "nothing was ever offered to build one from" are different bugs
        // with the same RT payload, and the second one is only visible here:
        // the block below says how much renderable geometry reached the
        // canonical scene and how much was counted as not reaching it.
        out["gpuScene"] = Json{ { "available", snapshot.GPUSceneAvailable } };
        if (snapshot.GPUSceneAvailable)
        {
            Json byCategory = Json::object();
            for (sizet i = 0; i < OloEngine::GPUSceneUnsupportedCategoryCount; ++i)
            {
                byCategory[UnsupportedCategoryKey(static_cast<OloEngine::GPUSceneUnsupportedCategory>(i))] =
                    snapshot.GPUScene.m_UnsupportedCounts[i];
            }
            out["gpuScene"]["instances"] = snapshot.GPUScene.m_Instances.m_Live;
            out["gpuScene"]["geometries"] = snapshot.GPUScene.m_Geometries.m_Live;
            out["gpuScene"]["materials"] = snapshot.GPUScene.m_Materials.m_Live;
            out["gpuScene"]["lights"] = snapshot.GPUScene.m_Lights.m_Live;
            out["gpuScene"]["notStagedTotal"] = snapshot.GPUScene.m_UnsupportedTotal;
            out["gpuScene"]["notStagedByCategory"] = std::move(byCategory);
        }

        if (StatsSnapshot::Status(snapshot.State) != "ready")
        {
            return out;
        }

        const auto& resident = snapshot.Stats.Resident;
        const auto& frame = snapshot.Stats.Frame;

        out["resident"] = Json{
            { "blasByClass",
              Json{
                  { "static", resident.BlasByClass[static_cast<sizet>(RT::GeometryClass::Static)] },
                  { "rigidDynamic", resident.BlasByClass[static_cast<sizet>(RT::GeometryClass::RigidDynamic)] },
                  { "deformed", resident.BlasByClass[static_cast<sizet>(RT::GeometryClass::Deformed)] },
                  { "masked", resident.BlasByClass[static_cast<sizet>(RT::GeometryClass::Masked)] },
                  // Always zero: an unsupported record produces no structure.
                  // The real count is unsupportedInstances below, which is a
                  // per-INSTANCE number because rejection is a per-instance
                  // verdict.
                  { "unsupported", resident.BlasByClass[static_cast<sizet>(RT::GeometryClass::Unsupported)] },
              } },
            { "tlasInstances", resident.TlasInstances },
            // A real, expected population — skinned, cloth, virtualized and
            // particle geometry never reach the canonical GPU Scene — not an
            // error count.
            { "unsupportedInstances", resident.UnsupportedInstances },
            { "accelerationStructureBytes", resident.AccelerationStructureBytes },
            { "scratchBytes", resident.ScratchBytes },
            { "compactionSavedBytes", resident.CompactionSavedBytes },
        };

        out["frame"] = Json{
            { "blasBuilds", frame.BlasBuilds },
            { "blasRefits", frame.BlasRefits },
            { "blasCompactions", frame.BlasCompactions },
            { "blasRetired", frame.BlasRetired },
            { "tlasBuilds", frame.TlasBuilds },
            { "tlasUpdates", frame.TlasUpdates },
            { "instancesTraced", frame.InstancesTraced },
            { "instancesSkipped", frame.InstancesSkipped },
            // Nanoseconds, resolved a frame or more late. Zero means "no
            // sample resolved yet", which is normal for the frames right after
            // a build — not "it was free".
            { "blasBuildGpuNs", frame.BlasBuildGpuNs },
            { "tlasBuildGpuNs", frame.TlasBuildGpuNs },
        };
        out["lastTlasReason"] = std::string(RT::ToString(snapshot.Stats.LastTlasReason));
        return out;
    }
} // namespace OloEngine::MCP::RayTracingStats
