#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TransparentStringHash.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    /**
     * @brief One node of a skill/talent tree.
     *
     * Unlocking is gated on character level, skill-point cost, and the
     * prerequisite DAG (ALL prerequisites must already be unlocked). The
     * payload either grants a GameplayAbilityDef into the owner's
     * AbilityComponent or applies an infinite passive GameplayEffect
     * (source tag "Skill.<NodeID>").
     */
    struct SkillTreeNode
    {
        enum class PayloadKind : u8
        {
            None = 0,         ///< Pure prerequisite / flavour node
            Ability = 1,      ///< Grants GrantedAbility into AbilityComponent
            PassiveEffect = 2 ///< Applies PassiveEffect as an Infinite effect
        };

        std::string NodeID;
        std::string DisplayName;
        std::string Description;
        i32 LevelRequirement = 1;
        i32 SkillPointCost = 1;
        std::vector<std::string> Prerequisites; ///< NodeIDs, all required

        PayloadKind Payload = PayloadKind::None;
        GameplayAbilityDef GrantedAbility;
        GameplayEffect PassiveEffect; ///< Policy forced to Infinite on load

        glm::vec2 EditorPosition{ 0.0f, 0.0f }; ///< Skill Tree editor canvas layout

        auto operator==(const SkillTreeNode&) const -> bool = default;
    };

    /**
     * @brief Skill/talent tree asset (`.oloskilltree`).
     *
     * A set of SkillTreeNodes forming a prerequisite DAG (a forest — nodes
     * without prerequisites are roots, so one asset can hold several visual
     * trees). Referenced by ProgressionComponent::SkillTreeHandle and by
     * CharacterClassDefinition::SkillTrees. Loaded via the AssetManager
     * (serializer: SkillTreeDatabaseSerializer), which rejects the asset when
     * Validate() fails (duplicate/empty node IDs, dangling prerequisites, or
     * a prerequisite cycle).
     */
    class SkillTreeDatabase : public Asset
    {
      public:
        SkillTreeDatabase() = default;

        static AssetType GetStaticType()
        {
            return AssetType::SkillTreeDatabase;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        /// O(1) node lookup by id; nullptr when absent. Requires RebuildIndex()
        /// after any mutation of m_Nodes (the serializer does this on load).
        [[nodiscard]] const SkillTreeNode* FindNode(std::string_view nodeId) const;

        /// Rebuild the NodeID -> index map from m_Nodes.
        void RebuildIndex();

        /// Structural validation: non-empty unique NodeIDs, every prerequisite
        /// references an existing node, no self-prerequisites, and the
        /// prerequisite graph is acyclic. On failure returns false and (when
        /// outError is non-null) a human-readable reason.
        [[nodiscard]] bool Validate(std::string* outError = nullptr) const;

        std::string m_TreeID;      ///< Stable string id, e.g. "warrior_arms"
        std::string m_DisplayName; ///< Editor-facing name
        std::vector<SkillTreeNode> m_Nodes;

      private:
        std::unordered_map<std::string, sizet, StringHash, StringEqual> m_NodeIndex;
    };
} // namespace OloEngine
