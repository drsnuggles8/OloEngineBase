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
    };

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
