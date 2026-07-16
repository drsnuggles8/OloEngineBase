#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
    class Scene;
    class Entity;

    /**
     * @brief Entity-aware service layer + per-tick system for RPG character
     * progression (issue #635). Mirrors the QuestSystem/InventorySystem
     * layering: ProgressionComponent is the pure value state; this static
     * class is where mutations become entity-stamped ProgressionEvents.h
     * payloads published on Scene::GetGameplayEvents().
     *
     * Ticked from the "Progression" node in Scene::GetGameplayScheduler()
     * (physics-shadow slot, game thread): drains PendingXP, resolves
     * multi-level-ups with overflow carry against the ExperienceCurve asset
     * (capping at the effective max level), grants attribute/skill points,
     * applies per-level stat growth through the dedicated
     * "Progression.LevelGrowth" AttributeSet modifier source, optionally heals
     * to full, pushes the resolved level/class into QuestJournal, and
     * publishes ExperienceGainedEvent / LevelUpEvent after iteration.
     *
     * Attribute-point allocation uses the second dedicated source
     * "Progression.AllocatedPoints"; both sources are recomputed
     * remove-then-add so they compose with equipment/effect modifiers without
     * corrupting them, and reapplication after scene/save loads is idempotent.
     */
    class ProgressionSystem
    {
      public:
        /// Modifier source tags (exact-match removal keys on AttributeSet).
        static constexpr const char* kLevelGrowthSource = "Progression.LevelGrowth";
        static constexpr const char* kAllocatedPointsSource = "Progression.AllocatedPoints";
        /// Prefix for skill passive-effect source tags: "Skill.<NodeID>".
        static constexpr const char* kSkillSourcePrefix = "Skill.";

        /// Point grants per level-up when no character class is resolved.
        static constexpr i32 kDefaultAttributePointsPerLevel = 5;
        static constexpr i32 kDefaultSkillPointsPerLevel = 1;

        /// Per-tick scheduler entry (Scene::UpdateProgression).
        static void OnUpdate(Scene* scene, f32 dt);

        /// Queue XP onto the entity's pending pool (resolved next Progression
        /// tick). No-op for amount <= 0 or a missing ProgressionComponent.
        static void GrantExperience(Scene* scene, Entity entity, i32 amount);

        /// Remaining XP needed to reach the next level (0 at the level cap or
        /// without a ProgressionComponent).
        [[nodiscard]] static i32 GetXPToNextLevel(Scene* scene, Entity entity);

        /// Effective max level: min(class LevelCap if set, curve MaxLevel).
        [[nodiscard]] static i32 GetMaxLevel(Scene* scene, Entity entity);

        /// Spend unspent attribute points into an attribute (validated: pool
        /// balance, attribute defined on the AbilityComponent, and — when a
        /// class is resolved — the attribute is marked spendable). Applied via
        /// the "Progression.AllocatedPoints" modifier source.
        static bool SpendAttributePoint(Scene* scene, Entity entity, const std::string& attribute, i32 count = 1);

        /// Refund previously allocated points back into the pool (validated:
        /// cannot refund more than allocated).
        static bool RefundAttributePoint(Scene* scene, Entity entity, const std::string& attribute, i32 count = 1);

        /// Clear the whole "Progression.AllocatedPoints" source and return
        /// every allocated point to the pool. Returns the number refunded.
        static i32 RespecAttributes(Scene* scene, Entity entity);

        /// True when UnlockSkillNode would succeed right now.
        [[nodiscard]] static bool CanUnlockSkillNode(Scene* scene, Entity entity, const std::string& nodeId);

        /// Unlock a skill-tree node (gates: not yet unlocked, level
        /// requirement, skill-point cost, all prerequisites unlocked). Applies
        /// the node payload (ability grant or infinite passive effect) and
        /// publishes SkillNodeUnlockedEvent.
        static bool UnlockSkillNode(Scene* scene, Entity entity, const std::string& nodeId);

        /// Refund an unlocked node (blocked while any unlocked node lists it
        /// as prerequisite). Returns the skill points and reverts the payload.
        static bool RefundSkillNode(Scene* scene, Entity entity, const std::string& nodeId);

        /// Refund every unlocked node and revert all payloads. Returns the
        /// number of skill points returned.
        static i32 RespecSkills(Scene* scene, Entity entity);

        /// Destructively (re)seed the entity from a class definition: defines
        /// the class attribute set (base values), grants starting abilities and
        /// tags, seeds the experience-curve handle, pushes class/level into
        /// QuestJournal, and reapplies the progression modifier sources.
        /// classId of "" uses the component's stored ClassID. Replaces the
        /// hard-coded AbilityComponent::InitializeDefaultRPGAttributes path.
        static bool InitializeFromClass(Scene* scene, Entity entity, const std::string& classId = "");
    };
} // namespace OloEngine
