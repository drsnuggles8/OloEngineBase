#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <string>

namespace OloEngine
{
    // Simple event data structs for quest operations.
    // These are POD-style notification payloads, not Event-derived classes.

    struct QuestStartedEvent
    {
        UUID EntityID;
        std::string QuestID;
    };

    struct QuestCompletedEvent
    {
        UUID EntityID;
        std::string QuestID;
        std::string BranchChoice;
    };

    struct QuestFailedEvent
    {
        UUID EntityID;
        std::string QuestID;
    };

    struct QuestAbandonedEvent
    {
        UUID EntityID;
        std::string QuestID;
    };

    struct ObjectiveProgressEvent
    {
        UUID EntityID;
        std::string QuestID;
        std::string ObjectiveID;
        i32 CurrentCount = 0;
        i32 RequiredCount = 0;
    };

    struct ObjectiveCompletedEvent
    {
        UUID EntityID;
        std::string QuestID;
        std::string ObjectiveID;
    };

    struct QuestStageAdvancedEvent
    {
        UUID EntityID;
        std::string QuestID;
        i32 NewStageIndex = 0;
    };

} // namespace OloEngine
