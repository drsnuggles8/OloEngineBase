#pragma once

#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>

namespace OloEngine
{
    struct LootTableEntry
    {
        std::string ItemDefinitionID;
        f32 Weight = 1.0f;
        i32 MinCount = 1;
        i32 MaxCount = 1;
        f32 MinItemLevel = 0.0f;
        f32 MaxItemLevel = 100.0f;

        i32 MinAffixes = 0;
        i32 MaxAffixes = 0;
        std::vector<std::string> PossibleAffixPoolIDs;
    };

    class LootTable
    {
      public:
        std::string TableID;
        std::vector<LootTableEntry> Entries;
        i32 MinDrops = 1;
        i32 MaxDrops = 3;
        f32 NothingWeight = 0.0f;

        [[nodiscard]] std::vector<ItemInstance> Roll(f32 itemLevel = 1.0f) const;
    };

} // namespace OloEngine
