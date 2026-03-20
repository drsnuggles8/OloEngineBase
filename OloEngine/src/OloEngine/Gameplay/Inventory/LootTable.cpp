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

        // Clamp inverted drop bounds
        i32 minDrops = MinDrops;
        i32 maxDrops = MaxDrops;
        if (minDrops > maxDrops)
        {
            std::swap(minDrops, maxDrops);
        }

        // Determine number of drops
        i32 numDrops = RandomUtils::Int32(minDrops, maxDrops);

        // Compute total weight (including NothingWeight), skipping non-positive weights
        f32 totalWeight = NothingWeight;
        for (auto const& entry : Entries)
        {
            if (entry.Weight <= 0.0f)
            {
                continue;
            }
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
                if (entry.Weight <= 0.0f)
                {
                    continue;
                }
                if (itemLevel < entry.MinItemLevel || itemLevel > entry.MaxItemLevel)
                {
                    continue;
                }

                accumulated += entry.Weight;
                if (roll < accumulated)
                {
                    // Clamp inverted count bounds
                    i32 minCount = entry.MinCount;
                    i32 maxCount = entry.MaxCount;
                    if (minCount > maxCount)
                    {
                        std::swap(minCount, maxCount);
                    }

                    ItemInstance instance;
                    instance.InstanceID = UUID();
                    instance.ItemDefinitionID = entry.ItemDefinitionID;
                    instance.StackCount = RandomUtils::Int32(minCount, maxCount);

                    results.push_back(std::move(instance));
                    break;
                }
            }
        }

        return results;
    }

} // namespace OloEngine
