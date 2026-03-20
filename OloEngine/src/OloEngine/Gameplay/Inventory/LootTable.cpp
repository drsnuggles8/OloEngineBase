#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/LootTable.h"
#include "OloEngine/Gameplay/Inventory/AffixDatabase.h"
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
        minDrops = std::max(minDrops, 0);
        maxDrops = std::max(maxDrops, 0);

        // Determine number of drops
        i32 numDrops = RandomUtils::Int32(minDrops, maxDrops);

        // Compute total weight (including NothingWeight), skipping non-positive weights
        f32 clampedNothingWeight = std::max(NothingWeight, 0.0f);
        f32 totalWeight = clampedNothingWeight;
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
            if (roll < clampedNothingWeight)
            {
                continue;
            }

            f32 accumulated = clampedNothingWeight;
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
                    minCount = std::max(minCount, 1);
                    maxCount = std::max(maxCount, 1);

                    ItemInstance instance;
                    instance.InstanceID = UUID();
                    instance.ItemDefinitionID = entry.ItemDefinitionID;
                    instance.StackCount = RandomUtils::Int32(minCount, maxCount);

                    // Roll affixes if the entry specifies them
                    if (entry.MaxAffixes > 0 && !entry.PossibleAffixPoolIDs.empty())
                    {
                        i32 lo = std::max(std::min(entry.MinAffixes, entry.MaxAffixes), 0);
                        i32 hi = std::max(std::max(entry.MinAffixes, entry.MaxAffixes), 0);
                        i32 numAffixes = RandomUtils::Int32(lo, hi);

                        i32 prefixCount = 0;
                        i32 suffixCount = 0;
                        constexpr i32 MaxPrefixes = 3;
                        constexpr i32 MaxSuffixes = 3;

                        for (i32 a = 0; a < numAffixes; ++a)
                        {
                            // Pick a random pool from the entry's list
                            i32 poolIdx = RandomUtils::Int32(0, static_cast<i32>(entry.PossibleAffixPoolIDs.size()) - 1);
                            const auto& poolId = entry.PossibleAffixPoolIDs[static_cast<size_t>(poolIdx)];

                            const AffixPool* pool = AffixDatabase::GetPool(poolId);
                            if (!pool || pool->AffixIDs.empty())
                            {
                                // Fallback: use pool ID as a raw affix (legacy behavior)
                                ItemAffix affix;
                                affix.Name = poolId;
                                affix.Attribute = poolId;
                                affix.Value = RandomUtils::Float32(1.0f, 10.0f);
                                instance.Affixes.push_back(std::move(affix));
                                continue;
                            }

                            // Pick a random affix definition from the pool
                            i32 affixIdx = RandomUtils::Int32(0, static_cast<i32>(pool->AffixIDs.size()) - 1);
                            const AffixDefinition* affixDef = AffixDatabase::Get(pool->AffixIDs[static_cast<size_t>(affixIdx)]);
                            if (!affixDef || affixDef->Tiers.empty())
                            {
                                continue;
                            }

                            // Enforce prefix/suffix limits
                            if (affixDef->Type == AffixType::Prefix && prefixCount >= MaxPrefixes)
                            {
                                continue;
                            }
                            if (affixDef->Type == AffixType::Suffix && suffixCount >= MaxSuffixes)
                            {
                                continue;
                            }

                            // Select the best eligible tier based on item level
                            const AffixTier* selectedTier = nullptr;
                            for (auto const& tier : affixDef->Tiers)
                            {
                                if (itemLevel >= tier.MinItemLevel)
                                {
                                    if (!selectedTier || tier.Tier > selectedTier->Tier)
                                    {
                                        selectedTier = &tier;
                                    }
                                }
                            }
                            if (!selectedTier)
                            {
                                continue;
                            }

                            ItemAffix affix;
                            affix.DefinitionID = affixDef->AffixID;
                            affix.Type = affixDef->Type;
                            affix.Tier = selectedTier->Tier;
                            affix.Name = selectedTier->DisplayName;
                            affix.Attribute = affixDef->Attribute;
                            affix.Value = RandomUtils::Float32(selectedTier->MinValue, selectedTier->MaxValue);
                            instance.Affixes.push_back(std::move(affix));

                            if (affixDef->Type == AffixType::Prefix)
                            {
                                ++prefixCount;
                            }
                            else
                            {
                                ++suffixCount;
                            }
                        }
                    }

                    results.push_back(std::move(instance));
                    break;
                }
            }
        }

        return results;
    }

} // namespace OloEngine
