#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/LootTable.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Core/UUID.h"

namespace OloEngine
{
    std::vector<ItemInstance> LootTable::Roll(f32 itemLevel) const
    {
        OLO_PROFILE_FUNCTION();
        std::vector<ItemInstance> results;

        if (Entries.empty())
        {
            return results;
        }

        // Determine number of drops
        i32 numDrops = RandomUtils::Int32(MinDrops, MaxDrops);

        // Compute total weight (including NothingWeight)
        f32 totalWeight = NothingWeight;
        for (auto const& entry : Entries)
        {
            if (itemLevel >= entry.MinItemLevel && itemLevel <= entry.MaxItemLevel)
            {
                totalWeight += entry.Weight;
            }
        }

        if (totalWeight <= 0.0f)
        {
            return results;
        }

        for (i32 drop = 0; drop < numDrops; ++drop)
        {
            f32 roll = RandomUtils::Float32(0.0f, totalWeight);

            // Check for nothing drop
            if (roll < NothingWeight)
            {
                continue;
            }

            f32 accumulated = NothingWeight;
            for (auto const& entry : Entries)
            {
                if (itemLevel < entry.MinItemLevel || itemLevel > entry.MaxItemLevel)
                {
                    continue;
                }

                accumulated += entry.Weight;
                if (roll < accumulated)
                {
                    ItemInstance instance;
                    instance.InstanceID = UUID();
                    instance.ItemDefinitionID = entry.ItemDefinitionID;
                    instance.StackCount = RandomUtils::Int32(entry.MinCount, entry.MaxCount);

                    results.push_back(std::move(instance));
                    break;
                }
            }
        }

        return results;
    }

} // namespace OloEngine
