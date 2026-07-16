#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/ComponentReflection.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    /**
     * @brief RPG character progression state: XP, level, point pools, unlocked
     * skill-tree nodes, and the data assets driving them.
     *
     * All fields are OloHeaderTool-trivial on purpose: the scene YAML
     * (de)serializer, the binary sidecar, and the MCP field registry are fully
     * generated — do NOT add a non-trivial member without hand-writing the
     * SceneSerializer blocks. Save-games use the hand-written Serialize()
     * overload in SaveGameComponentSerializer.cpp (full fidelity incl.
     * PendingXP).
     *
     * XP flows in through PendingXP (via ProgressionSystem::GrantExperience or
     * AddPendingXP); the Progression scheduler system drains it each tick,
     * resolves multi-level-ups with overflow carry against the ExperienceCurve
     * asset, applies class stat growth through the "Progression.LevelGrowth"
     * AttributeSet modifier source, and publishes ExperienceGainedEvent /
     * LevelUpEvent on the GameplayEventBus.
     */
    struct ProgressionComponent
    {
        OLO_SERIALIZE(Clamp, Min = 1)
        i32 Level = 1;

        /// XP accumulated toward the next level (overflow past a level-up is
        /// carried; XP past the max level is discarded).
        OLO_SERIALIZE(Clamp, Min = 0)
        i32 CurrentXP = 0;

        /// Unspent attribute points (granted per level-up).
        OLO_SERIALIZE(Clamp, Min = 0)
        i32 AttributePoints = 0;

        /// Unspent skill points (granted per level-up).
        OLO_SERIALIZE(Clamp, Min = 0)
        i32 SkillPoints = 0;

        /// XP granted to the killer's ProgressionComponent when this entity is
        /// killed through GameplayAbilitySystem::ApplyDamage. 0 = no bounty.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0)
        i32 XPBounty = 0;

        /// On level-up, set Health's base value to MaxHealth's current value.
        OLO_PROPERTY()
        bool HealOnLevelUp = true;

        /// Unlocked skill-tree node ids (across every tree this character uses).
        std::unordered_set<std::string> UnlockedNodes;

        /// Attribute-point allocation: attribute name -> points spent. Applied
        /// through the "Progression.AllocatedPoints" modifier source.
        std::unordered_map<std::string, i32> AllocatedPoints;

        /// ExperienceCurve asset (0 = class default, then engine default).
        AssetHandle ExperienceCurveHandle = 0;

        /// CharacterClassDatabase asset resolved together with ClassID.
        AssetHandle ClassDatabaseHandle = 0;

        /// Additional SkillTreeDatabase asset for class-less characters (the
        /// class's own SkillTrees list is searched as well).
        AssetHandle SkillTreeHandle = 0;

        /// Class id inside ClassDatabaseHandle; empty = no class.
        std::string ClassID;

        /// XP awaiting resolution by the Progression system this tick.
        /// Runtime-only for scene YAML; save-games persist it.
        OLO_SERIALIZE(Skip)
        i32 PendingXP = 0;

        /// Set once per runtime session after class-ensure + modifier/payload
        /// reapplication. Never serialized — a fresh load always re-applies.
        OLO_SERIALIZE(Skip)
        bool RuntimeInitialized = false;

        /// Saturating grant into the pending pool (safe from any game-thread
        /// call site; resolution happens in the Progression system tick).
        void AddPendingXP(i32 amount)
        {
            if (amount <= 0)
            {
                return;
            }
            i64 sum = static_cast<i64>(PendingXP) + static_cast<i64>(amount);
            constexpr i64 kMaxPending = 2000000000;
            PendingXP = static_cast<i32>(sum > kMaxPending ? kMaxPending : sum);
        }

        auto operator==(const ProgressionComponent&) const -> bool = default;
    };
} // namespace OloEngine
