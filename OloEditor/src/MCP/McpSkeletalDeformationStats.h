#pragma once

// Pure JSON shaping behind olo_skeletal_deformation_stats (issue #1226).
//
// The shared deformation output is a contract four downstream issues build on
// (#1227 morph targets, #1228 GPU Scene surface identity, #1229 Vulkan RT
// moving surfaces), so its cost and its discontinuities have to be inspectable
// from outside a single debugging session — otherwise each of them re-measures
// the same thing and none of them can see a history reset that happened.
//
// The counters come in two lifetimes, and the difference matters. The advance
// counts describe the LAST FRAME. The reset counts are SESSION TOTALS, because
// a reset is a rare deliberate event and clearing it every frame made it
// unobservable -- an explicit reset at a play-mode transition was wiped by the
// very next frame's advance, so it could never be seen from outside.

#include "MCP/McpStatsSnapshot.h"

#include <string>

namespace OloEngine::MCP::SkeletalDeformationStats
{
    using Json = nlohmann::json;

    struct Snapshot
    {
        StatsSnapshot::State State;
        u32 SkeletonsAdvanced = 0;
        u32 SkeletonsWithHistory = 0;
        u32 BoneMatricesAdvanced = 0;
        u32 HistoryResets = 0;
        u32 HistoryResetsFirstUse = 0;
        u32 HistoryResetsBoneCountChanged = 0;
        u32 HistoryResetsExplicit = 0;
        std::string LastResetCause = "None";
        // The morph half of the same surface (#1227). Same two lifetimes: the
        // advance counts are this frame, the resets and the malformed-input
        // counts are session totals.
        u32 MorphSurfacesAdvanced = 0;
        u32 MorphSurfacesWithHistory = 0;
        u32 MorphSurfacesRejected = 0;
        u32 HistoryResetsMorphSurfaceChanged = 0;
        u32 HistoryResetsMorphSetChanged = 0;
        u32 HistoryResetsMeshTopologyChanged = 0;
        u32 MorphUnknownTargets = 0;
        u32 MorphIncompatibleSets = 0;
        u32 MorphBaseCacheInvalidations = 0;
    };

    [[nodiscard("this builds the response; it does not send it")]] inline Json BuildReport(const Snapshot& snapshot)
    {
        Json out = StatsSnapshot::ToJson(snapshot.State);
        if (StatsSnapshot::Status(snapshot.State) != "ready")
            return out;

        out["skeletonsAdvanced"] = snapshot.SkeletonsAdvanced;
        out["skeletonsWithHistory"] = snapshot.SkeletonsWithHistory;
        out["boneMatricesAdvanced"] = snapshot.BoneMatricesAdvanced;

        // A skeleton that was advanced but carries no history emits zero bone
        // motion this frame. Reporting the two counts separately is what lets a
        // caller tell "nothing moved" from "the previous pose was thrown away".
        out["skeletonsWithoutHistory"] =
            snapshot.SkeletonsAdvanced >= snapshot.SkeletonsWithHistory
                ? snapshot.SkeletonsAdvanced - snapshot.SkeletonsWithHistory
                : 0u;

        // Session totals, not this frame -- see the note at the top.
        out["historyResets"] = Json{
            { "total", snapshot.HistoryResets },
            { "firstUse", snapshot.HistoryResetsFirstUse },
            { "boneCountChanged", snapshot.HistoryResetsBoneCountChanged },
            { "explicit", snapshot.HistoryResetsExplicit },
            { "morphSurfaceChanged", snapshot.HistoryResetsMorphSurfaceChanged },
            { "morphSetChanged", snapshot.HistoryResetsMorphSetChanged },
            { "meshTopologyChanged", snapshot.HistoryResetsMeshTopologyChanged },
            { "lastCause", snapshot.LastResetCause },
        };

        // The morph half. `rejected` is the one to watch: a morphing surface that
        // moved this frame cannot be reprojected at all -- the shaders build the
        // previous position from THIS frame's rest surface -- so its history is
        // thrown away deliberately rather than turned into a wrong velocity. A
        // count that stays at zero while a face is visibly expressing means the
        // rejection is not reaching the entity that is deforming.
        out["morph"] = Json{
            { "surfacesAdvanced", snapshot.MorphSurfacesAdvanced },
            { "surfacesWithHistory", snapshot.MorphSurfacesWithHistory },
            { "surfacesRejected", snapshot.MorphSurfacesRejected },
            { "unknownTargets", snapshot.MorphUnknownTargets },
            { "incompatibleSets", snapshot.MorphIncompatibleSets },
            { "baseCacheInvalidations", snapshot.MorphBaseCacheInvalidations },
        };
        return out;
    }
} // namespace OloEngine::MCP::SkeletalDeformationStats
