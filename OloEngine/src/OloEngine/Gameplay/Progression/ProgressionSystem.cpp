#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Progression/ProgressionSystem.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Progression/CharacterClassDatabase.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Gameplay/Progression/ProgressionEvents.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace OloEngine
{
    namespace
    {
        /// Null-safe asset resolution: a handle of 0, no active project, or no
        /// asset manager (headless unit tests) all degrade to nullptr, which
        /// downstream code treats as "use defaults".
        template<typename T>
        Ref<T> ResolveAsset(AssetHandle handle)
        {
            if (handle == 0 || !Project::GetActive() || !Project::HasAssetManager())
            {
                return nullptr;
            }
            return AssetManager::GetAsset<T>(handle);
        }

        struct ResolvedProgressionData
        {
            Ref<CharacterClassDatabase> ClassDb; // keep-alive for ClassDef
            const CharacterClassDefinition* ClassDef = nullptr;
            Ref<ExperienceCurve> Curve; // null = engine-default curve
        };

        ResolvedProgressionData Resolve(const ProgressionComponent& comp)
        {
            ResolvedProgressionData data;
            data.ClassDb = ResolveAsset<CharacterClassDatabase>(comp.ClassDatabaseHandle);
            if (data.ClassDb && !comp.ClassID.empty())
            {
                data.ClassDef = data.ClassDb->FindClass(comp.ClassID);
            }
            AssetHandle curveHandle = comp.ExperienceCurveHandle;
            if (curveHandle == 0 && data.ClassDef)
            {
                curveHandle = data.ClassDef->ExperienceCurve;
            }
            data.Curve = ResolveAsset<ExperienceCurve>(curveHandle);
            return data;
        }

        i32 XPForLevelUp(const ResolvedProgressionData& data, i32 level)
        {
            return data.Curve ? data.Curve->GetXPForLevelUp(level) : ExperienceCurve::DefaultXPForLevelUp(level);
        }

        i32 EffectiveMaxLevel(const ResolvedProgressionData& data)
        {
            i32 curveMax = data.Curve ? data.Curve->GetMaxLevel() : ExperienceCurve::kDefaultMaxLevel;
            if (data.ClassDef && data.ClassDef->LevelCap > 0)
            {
                return std::min(curveMax, data.ClassDef->LevelCap);
            }
            return curveMax;
        }

        /// Value one attribute point adds to `attribute`. With a class, only
        /// attributes whose spec has ValuePerPoint > 0 are spendable (returns
        /// 0 otherwise); without a class any defined attribute takes points at
        /// 1.0 per point.
        f32 ValuePerPointFor(const CharacterClassDefinition* classDef, std::string_view attribute)
        {
            if (!classDef)
            {
                return 1.0f;
            }
            for (const auto& spec : classDef->Attributes)
            {
                if (spec.Attribute == attribute)
                {
                    return spec.ValuePerPoint;
                }
            }
            return 0.0f;
        }

        void ReapplyGrowthModifiers(const ProgressionComponent& comp, AbilityComponent& ac, const CharacterClassDefinition* classDef)
        {
            if (!classDef)
            {
                return;
            }
            const GameplayTag source{ ProgressionSystem::kLevelGrowthSource };
            constexpr f32 kEpsilon = 1e-6f;
            for (const auto& spec : classDef->Attributes)
            {
                ac.Attributes.RemoveModifiersBySource(spec.Attribute, source);
                f32 growth = spec.GrowthPerLevel * static_cast<f32>(comp.Level - 1);
                if (std::isfinite(growth) && std::abs(growth) > kEpsilon)
                {
                    AttributeModifier mod;
                    mod.Op = AttributeModifier::Operation::Add;
                    mod.Magnitude = growth;
                    mod.Source = source;
                    ac.Attributes.AddModifier(spec.Attribute, mod);
                }
            }
        }

        void ReapplyAllocatedPointModifiers(const ProgressionComponent& comp, AbilityComponent& ac, const CharacterClassDefinition* classDef)
        {
            const GameplayTag source{ ProgressionSystem::kAllocatedPointsSource };
            // Remove from every attribute this source could have touched:
            // the current allocation keys plus every class attribute (covers
            // stale modifiers restored from a save-game).
            for (const auto& [attribute, points] : comp.AllocatedPoints)
            {
                ac.Attributes.RemoveModifiersBySource(attribute, source);
            }
            if (classDef)
            {
                for (const auto& spec : classDef->Attributes)
                {
                    ac.Attributes.RemoveModifiersBySource(spec.Attribute, source);
                }
            }
            constexpr f32 kEpsilon = 1e-6f;
            for (const auto& [attribute, points] : comp.AllocatedPoints)
            {
                if (points <= 0)
                {
                    continue;
                }
                // Honor recorded allocations even if the class no longer marks
                // the attribute spendable (data changed after the fact).
                f32 valuePerPoint = ValuePerPointFor(classDef, attribute);
                if (!(std::isfinite(valuePerPoint) && valuePerPoint > kEpsilon))
                {
                    valuePerPoint = 1.0f;
                }
                AttributeModifier mod;
                mod.Op = AttributeModifier::Operation::Add;
                mod.Magnitude = static_cast<f32>(points) * valuePerPoint;
                mod.Source = source;
                ac.Attributes.AddModifier(attribute, mod);
            }
        }

        std::vector<Ref<SkillTreeDatabase>> ResolveSkillTrees(const ProgressionComponent& comp, const CharacterClassDefinition* classDef)
        {
            std::vector<Ref<SkillTreeDatabase>> trees;
            if (auto tree = ResolveAsset<SkillTreeDatabase>(comp.SkillTreeHandle))
            {
                trees.push_back(std::move(tree));
            }
            if (classDef)
            {
                for (AssetHandle handle : classDef->SkillTrees)
                {
                    if (auto tree = ResolveAsset<SkillTreeDatabase>(handle))
                    {
                        trees.push_back(std::move(tree));
                    }
                }
            }
            return trees;
        }

        const SkillTreeNode* FindNodeInTrees(const std::vector<Ref<SkillTreeDatabase>>& trees, std::string_view nodeId)
        {
            for (const auto& tree : trees)
            {
                if (const auto* node = tree->FindNode(nodeId))
                {
                    return node;
                }
            }
            return nullptr;
        }

        GameplayTag SkillSourceTag(const std::string& nodeId)
        {
            return GameplayTag(std::string(ProgressionSystem::kSkillSourcePrefix) + nodeId);
        }

        /// Idempotent payload application — safe to re-run after scene or
        /// save-game loads (abilities dedupe by tag; passive effects dedupe by
        /// effect name with MaxStacks forced to 1).
        void ApplyNodePayload(AbilityComponent& ac, const SkillTreeNode& node)
        {
            switch (node.Payload)
            {
                case SkillTreeNode::PayloadKind::Ability:
                {
                    for (const auto& active : ac.Abilities)
                    {
                        if (active.Definition.AbilityTag.MatchesExact(node.GrantedAbility.AbilityTag))
                        {
                            return;
                        }
                    }
                    ac.Abilities.push_back(ActiveAbility{ node.GrantedAbility });
                    break;
                }
                case SkillTreeNode::PayloadKind::PassiveEffect:
                {
                    GameplayEffect effect = node.PassiveEffect;
                    effect.Policy.DurationType = GameplayEffectPolicy::Duration::Infinite;
                    effect.MaxStacks = 1;
                    if (effect.Name.empty())
                    {
                        effect.Name = std::string(ProgressionSystem::kSkillSourcePrefix) + node.NodeID;
                    }
                    if (!ac.ActiveEffects.ApplyEffect(effect, ac.OwnedTags, SkillSourceTag(node.NodeID)))
                    {
                        OLO_CORE_WARN("[Progression] Passive effect of skill node '{}' was blocked by tag requirements", node.NodeID);
                    }
                    break;
                }
                case SkillTreeNode::PayloadKind::None:
                default:
                    break;
            }
        }

        void RemoveNodePayload(Scene* scene, Entity entity, AbilityComponent& ac, const SkillTreeNode& node)
        {
            switch (node.Payload)
            {
                case SkillTreeNode::PayloadKind::Ability:
                {
                    for (const auto& active : ac.Abilities)
                    {
                        if (active.Definition.AbilityTag.MatchesExact(node.GrantedAbility.AbilityTag) && active.IsActive)
                        {
                            GameplayAbilitySystem::CancelAbility(scene, entity, node.GrantedAbility.AbilityTag);
                            break;
                        }
                    }
                    std::erase_if(ac.Abilities, [&node](const ActiveAbility& active)
                                  { return active.Definition.AbilityTag.MatchesExact(node.GrantedAbility.AbilityTag); });
                    break;
                }
                case SkillTreeNode::PayloadKind::PassiveEffect:
                {
                    // Cleanup overload: reverts persistent modifiers + granted tags.
                    ac.ActiveEffects.RemoveEffectsBySource(SkillSourceTag(node.NodeID), ac.Attributes, ac.OwnedTags);
                    break;
                }
                case SkillTreeNode::PayloadKind::None:
                default:
                    break;
            }
        }

        /// Once-per-session runtime reconciliation: non-destructive class
        /// ensure, then idempotent reapplication of the progression modifier
        /// sources and unlocked skill payloads. Runs after every scene or
        /// save-game load (RuntimeInitialized is never serialized).
        void EnsureRuntimeState(Entity entity, ProgressionComponent& comp)
        {
            comp.RuntimeInitialized = true;
            if (comp.Level < 1)
            {
                comp.Level = 1;
            }
            const auto data = Resolve(comp);
            if (!entity.HasComponent<AbilityComponent>())
            {
                return;
            }
            auto& ac = entity.GetComponent<AbilityComponent>();
            if (data.ClassDef)
            {
                for (const auto& spec : data.ClassDef->Attributes)
                {
                    if (!ac.Attributes.HasAttribute(spec.Attribute))
                    {
                        ac.Attributes.DefineAttribute(spec.Attribute, spec.BaseValue);
                    }
                }
                for (const auto& def : data.ClassDef->StartingAbilities)
                {
                    bool present = false;
                    for (const auto& active : ac.Abilities)
                    {
                        if (active.Definition.AbilityTag.MatchesExact(def.AbilityTag))
                        {
                            present = true;
                            break;
                        }
                    }
                    if (!present)
                    {
                        ac.Abilities.push_back(ActiveAbility{ def });
                    }
                }
                for (const auto& tagString : data.ClassDef->StartingTags)
                {
                    if (GameplayTag tag{ tagString }; !ac.OwnedTags.HasTagExact(tag))
                    {
                        ac.OwnedTags.AddTag(tag);
                    }
                }
            }
            ReapplyGrowthModifiers(comp, ac, data.ClassDef);
            ReapplyAllocatedPointModifiers(comp, ac, data.ClassDef);
            auto trees = ResolveSkillTrees(comp, data.ClassDef);
            for (const auto& nodeId : comp.UnlockedNodes)
            {
                if (const auto* node = FindNodeInTrees(trees, nodeId))
                {
                    ApplyNodePayload(ac, *node);
                }
                else if (!trees.empty())
                {
                    OLO_CORE_WARN("[Progression] Unlocked node '{}' not found in any skill tree; keeping it recorded", nodeId);
                }
            }
        }

        struct PendingProgressionEvents
        {
            std::vector<ExperienceGainedEvent> XP;
            std::vector<LevelUpEvent> Levels;
        };

        /// Drain PendingXP, resolve level-ups with overflow carry, cap at the
        /// effective max level (discarding beyond-cap XP), grant points, apply
        /// growth + heal-to-full, and queue events for post-iteration publish.
        void ResolveProgression(Entity entity, ProgressionComponent& comp, PendingProgressionEvents& events)
        {
            if (comp.Level < 1)
            {
                comp.Level = 1;
            }
            const auto data = Resolve(comp);
            // The cap only blocks future level-ups; a character above a
            // since-lowered cap keeps its earned levels.
            i32 maxLevel = std::max(EffectiveMaxLevel(data), comp.Level);

            i32 gained = comp.PendingXP;
            comp.PendingXP = 0;
            if (gained > 0)
            {
                i64 sum = static_cast<i64>(comp.CurrentXP) + static_cast<i64>(gained);
                constexpr i64 kMaxXP = 2000000000;
                comp.CurrentXP = static_cast<i32>(std::min(sum, kMaxXP));
            }

            i32 prevLevel = comp.Level;
            i32 apGained = 0;
            i32 spGained = 0;
            i32 apPerLevel = data.ClassDef ? data.ClassDef->AttributePointsPerLevel : ProgressionSystem::kDefaultAttributePointsPerLevel;
            i32 spPerLevel = data.ClassDef ? data.ClassDef->SkillPointsPerLevel : ProgressionSystem::kDefaultSkillPointsPerLevel;
            while (comp.Level < maxLevel)
            {
                i32 needed = XPForLevelUp(data, comp.Level);
                if (comp.CurrentXP < needed)
                {
                    break;
                }
                comp.CurrentXP -= needed;
                ++comp.Level;
                apGained += apPerLevel;
                spGained += spPerLevel;
            }
            if (comp.Level >= maxLevel && comp.CurrentXP > 0)
            {
                comp.CurrentXP = 0;
            }

            if (comp.Level != prevLevel)
            {
                comp.AttributePoints += apGained;
                comp.SkillPoints += spGained;
                if (entity.HasComponent<AbilityComponent>())
                {
                    auto& ac = entity.GetComponent<AbilityComponent>();
                    ReapplyGrowthModifiers(comp, ac, data.ClassDef);
                    if (comp.HealOnLevelUp && ac.Attributes.HasAttribute("Health") && ac.Attributes.HasAttribute("MaxHealth"))
                    {
                        ac.Attributes.SetBaseValue("Health", ac.Attributes.GetCurrentValue("MaxHealth"));
                    }
                }
                events.Levels.push_back({ entity.GetUUID(), prevLevel, comp.Level, apGained, spGained });
            }
            if (gained > 0)
            {
                events.XP.push_back({ entity.GetUUID(), gained, comp.Level, comp.CurrentXP });
            }
        }
    } // namespace

    void ProgressionSystem::OnUpdate(Scene* scene, f32 /*dt*/)
    {
        if (!scene)
        {
            return;
        }
        PendingProgressionEvents events;
        auto view = scene->GetAllEntitiesWith<ProgressionComponent>();
        for (auto entityHandle : view)
        {
            Entity entity{ entityHandle, scene };
            auto& comp = view.get<ProgressionComponent>(entityHandle);
            if (!comp.RuntimeInitialized)
            {
                EnsureRuntimeState(entity, comp);
            }
            ResolveProgression(entity, comp, events);
            // Keep the quest journal's externally-fed player state in sync so
            // Level / IsClass quest requirement gates track earned progression.
            if (entity.HasComponent<QuestJournalComponent>())
            {
                auto& journal = entity.GetComponent<QuestJournalComponent>().Journal;
                if (journal.GetPlayerLevel() != comp.Level)
                {
                    journal.SetPlayerLevel(comp.Level);
                }
                if (!comp.ClassID.empty() && journal.GetPlayerClass() != comp.ClassID)
                {
                    journal.SetPlayerClass(comp.ClassID);
                }
            }
        }
        // Publish outside the view walk (GameplayEventBus contract).
        const auto& bus = scene->GetGameplayEvents();
        for (const auto& event : events.XP)
        {
            bus.Publish(event);
        }
        for (const auto& event : events.Levels)
        {
            bus.Publish(event);
        }
    }

    void ProgressionSystem::GrantExperience([[maybe_unused]] Scene* scene, Entity entity, i32 amount)
    {
        if (amount <= 0 || !entity || !entity.HasComponent<ProgressionComponent>())
        {
            return;
        }
        entity.GetComponent<ProgressionComponent>().AddPendingXP(amount);
    }

    i32 ProgressionSystem::GetXPToNextLevel([[maybe_unused]] Scene* scene, Entity entity)
    {
        if (!entity || !entity.HasComponent<ProgressionComponent>())
        {
            return 0;
        }
        const auto& comp = entity.GetComponent<ProgressionComponent>();
        const auto data = Resolve(comp);
        if (comp.Level >= EffectiveMaxLevel(data))
        {
            return 0;
        }
        return std::max(XPForLevelUp(data, comp.Level) - comp.CurrentXP, 0);
    }

    i32 ProgressionSystem::GetMaxLevel([[maybe_unused]] Scene* scene, Entity entity)
    {
        if (!entity || !entity.HasComponent<ProgressionComponent>())
        {
            return 0;
        }
        const auto data = Resolve(entity.GetComponent<ProgressionComponent>());
        return EffectiveMaxLevel(data);
    }

    bool ProgressionSystem::SpendAttributePoint([[maybe_unused]] Scene* scene, Entity entity, const std::string& attribute, i32 count)
    {
        if (count <= 0 || attribute.empty() || !entity ||
            !entity.HasComponent<ProgressionComponent>() || !entity.HasComponent<AbilityComponent>())
        {
            return false;
        }
        auto& comp = entity.GetComponent<ProgressionComponent>();
        auto& ac = entity.GetComponent<AbilityComponent>();
        if (comp.AttributePoints < count || !ac.Attributes.HasAttribute(attribute))
        {
            return false;
        }
        const auto data = Resolve(comp);
        constexpr f32 kEpsilon = 1e-6f;
        if (data.ClassDef && ValuePerPointFor(data.ClassDef, attribute) <= kEpsilon)
        {
            return false; // class marks this attribute non-spendable
        }
        comp.AttributePoints -= count;
        comp.AllocatedPoints[attribute] += count;
        ReapplyAllocatedPointModifiers(comp, ac, data.ClassDef);
        return true;
    }

    bool ProgressionSystem::RefundAttributePoint([[maybe_unused]] Scene* scene, Entity entity, const std::string& attribute, i32 count)
    {
        if (count <= 0 || attribute.empty() || !entity ||
            !entity.HasComponent<ProgressionComponent>() || !entity.HasComponent<AbilityComponent>())
        {
            return false;
        }
        auto& comp = entity.GetComponent<ProgressionComponent>();
        auto& ac = entity.GetComponent<AbilityComponent>();
        auto it = comp.AllocatedPoints.find(attribute);
        if (it == comp.AllocatedPoints.end() || it->second < count)
        {
            return false; // never refund more than was allocated
        }
        it->second -= count;
        if (it->second <= 0)
        {
            // Remove the (now stale) modifier before the key disappears from
            // the map — ReapplyAllocatedPointModifiers only sweeps current keys
            // plus class attributes.
            ac.Attributes.RemoveModifiersBySource(attribute, GameplayTag{ kAllocatedPointsSource });
            comp.AllocatedPoints.erase(it);
        }
        comp.AttributePoints += count;
        const auto data = Resolve(comp);
        ReapplyAllocatedPointModifiers(comp, ac, data.ClassDef);
        return true;
    }

    i32 ProgressionSystem::RespecAttributes([[maybe_unused]] Scene* scene, Entity entity)
    {
        if (!entity || !entity.HasComponent<ProgressionComponent>())
        {
            return 0;
        }
        auto& comp = entity.GetComponent<ProgressionComponent>();
        i32 refunded = 0;
        if (entity.HasComponent<AbilityComponent>())
        {
            auto& ac = entity.GetComponent<AbilityComponent>();
            const GameplayTag source{ kAllocatedPointsSource };
            for (const auto& [attribute, points] : comp.AllocatedPoints)
            {
                ac.Attributes.RemoveModifiersBySource(attribute, source);
                refunded += std::max(points, 0);
            }
        }
        else
        {
            for (const auto& [attribute, points] : comp.AllocatedPoints)
            {
                refunded += std::max(points, 0);
            }
        }
        comp.AllocatedPoints.clear();
        comp.AttributePoints += refunded;
        return refunded;
    }

    bool ProgressionSystem::CanUnlockSkillNode([[maybe_unused]] Scene* scene, Entity entity, const std::string& nodeId)
    {
        if (nodeId.empty() || !entity || !entity.HasComponent<ProgressionComponent>())
        {
            return false;
        }
        const auto& comp = entity.GetComponent<ProgressionComponent>();
        if (comp.UnlockedNodes.contains(nodeId))
        {
            return false;
        }
        const auto data = Resolve(comp);
        auto trees = ResolveSkillTrees(comp, data.ClassDef);
        const auto* node = FindNodeInTrees(trees, nodeId);
        if (!node || comp.Level < node->LevelRequirement || comp.SkillPoints < node->SkillPointCost)
        {
            return false;
        }
        if (node->Payload != SkillTreeNode::PayloadKind::None && !entity.HasComponent<AbilityComponent>())
        {
            return false;
        }
        for (const auto& prereq : node->Prerequisites)
        {
            if (!comp.UnlockedNodes.contains(prereq))
            {
                return false;
            }
        }
        return true;
    }

    bool ProgressionSystem::UnlockSkillNode(Scene* scene, Entity entity, const std::string& nodeId)
    {
        if (!CanUnlockSkillNode(scene, entity, nodeId))
        {
            return false;
        }
        auto& comp = entity.GetComponent<ProgressionComponent>();
        const auto data = Resolve(comp);
        auto trees = ResolveSkillTrees(comp, data.ClassDef);
        const auto* node = FindNodeInTrees(trees, nodeId);
        if (!node)
        {
            return false;
        }
        comp.SkillPoints -= std::max(node->SkillPointCost, 0);
        comp.UnlockedNodes.insert(nodeId);
        if (node->Payload != SkillTreeNode::PayloadKind::None)
        {
            ApplyNodePayload(entity.GetComponent<AbilityComponent>(), *node);
        }
        if (scene)
        {
            scene->GetGameplayEvents().Publish(SkillNodeUnlockedEvent{ entity.GetUUID(), nodeId });
        }
        return true;
    }

    bool ProgressionSystem::RefundSkillNode(Scene* scene, Entity entity, const std::string& nodeId)
    {
        if (nodeId.empty() || !entity || !entity.HasComponent<ProgressionComponent>())
        {
            return false;
        }
        auto& comp = entity.GetComponent<ProgressionComponent>();
        if (!comp.UnlockedNodes.contains(nodeId))
        {
            return false;
        }
        const auto data = Resolve(comp);
        auto trees = ResolveSkillTrees(comp, data.ClassDef);
        const auto* node = FindNodeInTrees(trees, nodeId);
        if (!node)
        {
            return false; // unknown node: cost unknowable, keep it recorded
        }
        // Blocked while any unlocked node depends on it.
        for (const auto& tree : trees)
        {
            for (const auto& other : tree->m_Nodes)
            {
                if (!comp.UnlockedNodes.contains(other.NodeID))
                {
                    continue;
                }
                for (const auto& prereq : other.Prerequisites)
                {
                    if (prereq == nodeId)
                    {
                        return false;
                    }
                }
            }
        }
        comp.UnlockedNodes.erase(nodeId);
        comp.SkillPoints += std::max(node->SkillPointCost, 0);
        if (node->Payload != SkillTreeNode::PayloadKind::None && entity.HasComponent<AbilityComponent>())
        {
            RemoveNodePayload(scene, entity, entity.GetComponent<AbilityComponent>(), *node);
        }
        return true;
    }

    i32 ProgressionSystem::RespecSkills(Scene* scene, Entity entity)
    {
        if (!entity || !entity.HasComponent<ProgressionComponent>())
        {
            return 0;
        }
        auto& comp = entity.GetComponent<ProgressionComponent>();
        const auto data = Resolve(comp);
        auto trees = ResolveSkillTrees(comp, data.ClassDef);
        i32 refunded = 0;
        for (const auto& nodeId : comp.UnlockedNodes)
        {
            const auto* node = FindNodeInTrees(trees, nodeId);
            if (!node)
            {
                OLO_CORE_WARN("[Progression] RespecSkills: node '{}' not found in any skill tree — no points refunded for it", nodeId);
                continue;
            }
            refunded += std::max(node->SkillPointCost, 0);
            if (node->Payload != SkillTreeNode::PayloadKind::None && entity.HasComponent<AbilityComponent>())
            {
                RemoveNodePayload(scene, entity, entity.GetComponent<AbilityComponent>(), *node);
            }
        }
        comp.UnlockedNodes.clear();
        comp.SkillPoints += refunded;
        return refunded;
    }

    bool ProgressionSystem::InitializeFromClass([[maybe_unused]] Scene* scene, Entity entity, const std::string& classId)
    {
        if (!entity || !entity.HasComponent<ProgressionComponent>())
        {
            return false;
        }
        auto& comp = entity.GetComponent<ProgressionComponent>();
        const std::string resolvedId = classId.empty() ? comp.ClassID : classId;
        if (resolvedId.empty())
        {
            return false;
        }
        auto classDb = ResolveAsset<CharacterClassDatabase>(comp.ClassDatabaseHandle);
        const auto* classDef = classDb ? classDb->FindClass(resolvedId) : nullptr;
        if (!classDef)
        {
            OLO_CORE_WARN("[Progression] InitializeFromClass: class '{}' not resolvable from the class database", resolvedId);
            return false;
        }
        if (!entity.HasComponent<AbilityComponent>())
        {
            entity.AddComponent<AbilityComponent>();
        }
        auto& ac = entity.GetComponent<AbilityComponent>();
        for (const auto& spec : classDef->Attributes)
        {
            // Destructive seed: resets the base value; existing modifiers on
            // the attribute are preserved by DefineAttribute.
            ac.Attributes.DefineAttribute(spec.Attribute, spec.BaseValue);
        }
        for (const auto& def : classDef->StartingAbilities)
        {
            bool present = false;
            for (const auto& active : ac.Abilities)
            {
                if (active.Definition.AbilityTag.MatchesExact(def.AbilityTag))
                {
                    present = true;
                    break;
                }
            }
            if (!present)
            {
                ac.Abilities.push_back(ActiveAbility{ def });
            }
        }
        for (const auto& tagString : classDef->StartingTags)
        {
            if (GameplayTag tag{ tagString }; !ac.OwnedTags.HasTagExact(tag))
            {
                ac.OwnedTags.AddTag(tag);
            }
        }
        comp.ClassID = resolvedId;
        if (comp.ExperienceCurveHandle == 0)
        {
            comp.ExperienceCurveHandle = classDef->ExperienceCurve;
        }
        if (entity.HasComponent<QuestJournalComponent>())
        {
            auto& journal = entity.GetComponent<QuestJournalComponent>().Journal;
            journal.SetPlayerClass(comp.ClassID);
            journal.SetPlayerLevel(comp.Level);
        }
        ReapplyGrowthModifiers(comp, ac, classDef);
        ReapplyAllocatedPointModifiers(comp, ac, classDef);
        comp.RuntimeInitialized = true;
        return true;
    }
} // namespace OloEngine
