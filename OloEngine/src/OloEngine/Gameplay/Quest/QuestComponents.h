#pragma once

#include "OloEngine/Gameplay/Quest/QuestJournal.h"

#include <string>
#include <vector>

namespace OloEngine
{
    struct QuestJournalComponent
    {
        QuestJournal Journal;

        auto operator==(const QuestJournalComponent&) const -> bool = default;
    };

    // Placed on NPCs that give/accept quests
    struct QuestGiverComponent
    {
        std::vector<std::string> OfferedQuestIDs;
        std::vector<std::string> TurnInQuestIDs;
        std::string QuestMarkerIcon; // "!" for available, "?" for turn-in

        auto operator==(const QuestGiverComponent&) const -> bool = default;
    };

} // namespace OloEngine
