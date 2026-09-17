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
#include "OloEngine/Renderer/RayTracing/DeformedSurfaceCache.h"
#include "OloEngine/Renderer/RayTracing/VegetationSurfaceCache.h"

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
        // The deformed-vertex producer behind the animated BLASes (#1229).
        // Reported beside them for the same reason GPUScene is: "no deformed
        // surfaces" and "deformed surfaces that could not be produced" give the
        // RT counters the same shape and need opposite fixes.
        OloEngine::RayTracing::DeformedSurfaceStats Deformed;
        OloEngine::RayTracing::VegetationSurfaceStats Vegetation;
        bool VegetationReady = true;
    };

    // The JSON key for each diagnostics category.
    //
    // A switch rather than a table lookup so an unnamed category is at least a
    // -Wswitch warning rather than an out-of-range read. It is NOT a compile
    // error, though an earlier version of this comment claimed so: adding
    // GPUSceneUnsupportedCategory::Groom (#1246) compiled fine and fell through
    // to "unknown", and what actually caught it was
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

            // Foliage rides its own vertex stream, so "Foliage" above only
            // counts SYSTEMS that skip the records. This says what is inside
            // them: how many plants carry canonical identity, how each is
            // represented, and what the raster path cannot draw (issue #1230).
            const auto& foliage = snapshot.GPUScene.m_Foliage;
            out["gpuScene"]["foliage"] = Json{
                { "canonicalInstances", foliage.m_CanonicalInstances },
                { "meshCardInstances", foliage.m_MeshCardInstances },
                { "impostorInstances", foliage.m_ImpostorInstances },
                { "authoredMeshInstances", foliage.m_AuthoredMeshInstances },
                { "unsupportedInstances", foliage.m_UnsupportedInstances },
                { "unsupportedVariants", foliage.m_UnsupportedVariants },
                { "spatialGroups", foliage.m_SpatialGroups },
            };

            // Animated surfaces (issue #1228). "Skinned" in notStagedByCategory
            // above no longer means "every skinned entity" — it means an
            // animated entity that reached submission and produced no record at
            // all. This block is the positive half: what the records DID take,
            // and how many of those carry a previous pose a velocity may
            // legitimately be measured against.
            //
            // canonicalInstances == surfacesWithHistory + surfacesWithoutHistory
            // by construction; a reader seeing that fail is looking at a
            // producer bug, not at a scene.
            const auto& animated = snapshot.GPUScene.m_Animated;
            out["gpuScene"]["animated"] = Json{
                { "canonicalEntities", animated.m_CanonicalEntities },
                { "canonicalInstances", animated.m_CanonicalInstances },
                { "surfacesWithHistory", animated.m_SurfacesWithHistory },
                { "surfacesWithoutHistory", animated.m_SurfacesWithoutHistory },
                { "unsupportedInstances", animated.m_UnsupportedInstances },
                { "unsupportedVariants", animated.m_UnsupportedVariants },
            };
        }

        // BEFORE the readiness early-return, for the same reason as gpuScene:
        // a producer that refuses its work is exactly what makes the RT scene
        // report "no TLAS", so its counters must survive that status.
        const auto& vegetation = snapshot.Vegetation;
        out["vegetation"] = Json{
            { "ready", snapshot.VegetationReady },
            { "complete", vegetation.Complete },
            { "producerFailed", vegetation.ProducerFailed },
            { "requested", vegetation.GroupsRequested },
            { "detailedGroups", vegetation.DetailedGroups },
            { "proxyGroups", vegetation.ProxyGroups },
            { "plantsRepresented", vegetation.PlantsRepresented },
            { "residentBytes", vegetation.ResidentBytes },
            { "dispatched", vegetation.Dispatched },
            { "dispatchBatches", vegetation.DispatchBatches },
            { "verticesDeformed", vegetation.VerticesDeformed },
            { "reused", vegetation.SnapshotsReused },
            { "refused", vegetation.Refused },
            { "historyReset", vegetation.HistoryReset },
        };

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
            // A real, expected population — cloth and particle geometry never
            // reach the canonical GPU Scene — not an error count. Virtualized
            // geometry left this number at #1144 (it arrives as a fixed rigid
            // proxy) and ANIMATED geometry joined it at #1228.
            { "unsupportedInstances", resident.UnsupportedInstances },
            // The animated share of the number above, broken out because it is
            // the one part that means "this should have been traceable". A
            // non-zero value with characters on screen says their deformed
            // vertices are not reaching the RT scene, and they are being kept
            // OUT of the TLAS rather than traced at their rest pose.
            { "animatedInstancesRefused", resident.AnimatedInstancesRefused },
            { "accelerationStructureBytes", resident.AccelerationStructureBytes },
            { "scratchBytes", resident.ScratchBytes },
            { "compactionSavedBytes", resident.CompactionSavedBytes },
        };

        // The deformed-vertex producer (issue #1229). Criterion 4 asks for
        // build/refit timings, memory and rebuild reasons; the structures'
        // half is above, and this is the producer's — because a frame where
        // animated surfaces are expensive is either deforming a lot of vertices
        // or rebuilding a lot of structures, and the RT counters alone cannot
        // say which.
        //
        // `refused` is the number to read first. It is the one value here that
        // means something is wrong rather than something is costly: a surface
        // that should be in the TLAS and is not.
        //
        // `skippedUnchanged` is the opposite — a large number there is the
        // system working, because an idle character costs neither a dispatch
        // nor a refit.
        const auto& deformed = snapshot.Deformed;
        out["deformedSurfaces"] = Json{
            { "residentSurfaces", deformed.ResidentSurfaces },
            { "residentBytes", deformed.ResidentBytes },
            { "paletteBytes", deformed.PaletteBytes },
            { "requested", deformed.SurfacesRequested },
            { "dispatched", deformed.Dispatched },
            { "verticesDeformed", deformed.VerticesDeformed },
            { "allocated", deformed.Allocated },
            { "reallocated", deformed.Reallocated },
            { "retired", deformed.Retired },
            { "skippedUnchanged", deformed.SkippedUnchanged },
            { "refused", deformed.Refused },
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
