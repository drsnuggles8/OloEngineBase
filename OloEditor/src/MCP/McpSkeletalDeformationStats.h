#pragma once

// Pure JSON shaping behind olo_skeletal_deformation_stats (issue #1226).
//
// The shared deformation output is a contract four downstream issues build on
// (#1227 morph targets, #1228 GPU Scene surface identity, #1229 Vulkan RT
// moving surfaces), so its cost and its discontinuities have to be inspectable
// from outside a single debugging session — otherwise each of them re-measures
// the same thing and none of them can see a history reset that happened.
//
// The counters describe the LAST FRAME, not the session: SkeletalDeformation
// clears them at the start of every advance. A frame with skinned entities and
// zero resets is the healthy steady state and reads as exactly that.

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

        out["historyResets"] = Json{
            { "total", snapshot.HistoryResets },
            { "firstUse", snapshot.HistoryResetsFirstUse },
            { "boneCountChanged", snapshot.HistoryResetsBoneCountChanged },
            { "explicit", snapshot.HistoryResetsExplicit },
            { "lastCause", snapshot.LastResetCause },
        };
        return out;
    }
} // namespace OloEngine::MCP::SkeletalDeformationStats
