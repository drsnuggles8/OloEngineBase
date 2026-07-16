#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <string>

namespace OloEngine
{
    // POD-style notification payloads published on the per-Scene
    // GameplayEventBus, not Event-derived classes (mirrors QuestEvents.h /
    // InventoryEvents.h). Entity-stamped with UUIDs; subscribers resolve via
    // Scene::GetEntityByUUID.

    /// Pending XP was drained into a character this tick (after any level-ups
    /// were resolved — Level/CurrentXP are the post-resolution values).
    struct ExperienceGainedEvent
    {
        UUID EntityID;
        i32 Amount = 0;
        i32 Level = 0;
        i32 CurrentXP = 0;
    };

    /// A character's level rose (possibly by several levels from one grant —
    /// PreviousLevel -> NewLevel covers the whole jump).
    struct LevelUpEvent
    {
        UUID EntityID;
        i32 PreviousLevel = 0;
        i32 NewLevel = 0;
        i32 AttributePointsGained = 0;
        i32 SkillPointsGained = 0;
    };

    /// A skill-tree node was unlocked and its payload applied.
    struct SkillNodeUnlockedEvent
    {
        UUID EntityID;
        std::string NodeID;
    };
} // namespace OloEngine
