#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Gameplay/Progression/CharacterClassDatabase.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"
#include "OloEngine/Project/Project.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    // Serializers for the three progression data assets (issue #635):
    // ExperienceCurve (.oloxpcurve), SkillTreeDatabase (.oloskilltree),
    // CharacterClassDatabase (.olocharclass). Effect / ability-def YAML keys
    // deliberately mirror SceneSerializer's SerializeEffectList format so
    // authored gameplay content reads the same everywhere.

    namespace
    {
        // Validate a float read from a content YAML file (cpp-coding-quality
        // §2b): hand-edited ".nan"/".inf" parses cleanly but must not enter
        // gameplay math.
        [[nodiscard]] f32 SanitizeFinite(f32 value, f32 fallback, const char* context)
        {
            if (!std::isfinite(value))
            {
                OLO_CORE_WARN("[ProgressionAssets] Non-finite {} (NaN/inf); using {}", context, fallback);
                return fallback;
            }
            return value;
        }

        void EmitTagList(YAML::Emitter& out, const char* key, const GameplayTagContainer& tags)
        {
            out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
            for (auto const& tag : tags.GetTags())
            {
                out << tag.GetTagString();
            }
            out << YAML::EndSeq;
        }

        void ReadTagList(const YAML::Node& node, const char* key, GameplayTagContainer& tags)
        {
            if (auto listNode = node[key]; listNode && listNode.IsSequence())
            {
                for (auto const& t : listNode)
                {
                    if (std::string tag = t.as<std::string>(""); !tag.empty())
                    {
                        tags.AddTag(GameplayTag(tag));
                    }
                }
            }
        }

        void EmitEffectList(YAML::Emitter& out, const char* key, const std::vector<GameplayEffect>& effects)
        {
            out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
            for (auto const& effect : effects)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << effect.Name;

                std::string durType = "Instant";
                if (effect.Policy.DurationType == GameplayEffectPolicy::Duration::HasDuration)
                    durType = "HasDuration";
                else if (effect.Policy.DurationType == GameplayEffectPolicy::Duration::Infinite)
                    durType = "Infinite";
                out << YAML::Key << "DurationType" << YAML::Value << durType;
                out << YAML::Key << "DurationSeconds" << YAML::Value << effect.Policy.DurationSeconds;
                out << YAML::Key << "IsPeriodic" << YAML::Value << effect.Policy.IsPeriodic;
                out << YAML::Key << "PeriodSeconds" << YAML::Value << effect.Policy.PeriodSeconds;

                out << YAML::Key << "Modifiers" << YAML::Value << YAML::BeginSeq;
                for (auto const& mod : effect.Modifiers)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Attribute" << YAML::Value << mod.AttributeName;
                    std::string op = "Add";
                    if (mod.Op == AttributeModifier::Operation::Multiply)
                        op = "Multiply";
                    else if (mod.Op == AttributeModifier::Operation::Override)
                        op = "Override";
                    out << YAML::Key << "Operation" << YAML::Value << op;
                    out << YAML::Key << "Magnitude" << YAML::Value << mod.Magnitude;
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;

                out << YAML::Key << "MaxStacks" << YAML::Value << effect.MaxStacks;
                out << YAML::Key << "RefreshDurationOnStack" << YAML::Value << effect.RefreshDurationOnStack;

                EmitTagList(out, "GrantedTags", effect.GrantedTags);
                EmitTagList(out, "RequiredTags", effect.RequiredTags);
                EmitTagList(out, "BlockedTags", effect.BlockedTags);

                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        void ReadEffectList(const YAML::Node& node, const char* key, std::vector<GameplayEffect>& effects)
        {
            auto effectsNode = node[key];
            if (!effectsNode || !effectsNode.IsSequence())
            {
                return;
            }
            for (auto const& effectNode : effectsNode)
            {
                GameplayEffect ge;
                ge.Name = effectNode["Name"].as<std::string>("");

                if (std::string durType = effectNode["DurationType"].as<std::string>("Instant"); durType == "HasDuration")
                    ge.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
                else if (durType == "Infinite")
                    ge.Policy.DurationType = GameplayEffectPolicy::Duration::Infinite;

                ge.Policy.DurationSeconds = effectNode["DurationSeconds"].as<f32>(0.0f);
                ge.Policy.IsPeriodic = effectNode["IsPeriodic"].as<bool>(false);
                ge.Policy.PeriodSeconds = effectNode["PeriodSeconds"].as<f32>(1.0f);
                ge.Policy.DurationSeconds = std::clamp(SanitizeFinite(ge.Policy.DurationSeconds, 0.0f, "effect DurationSeconds"), 0.0f, 600.0f);
                ge.Policy.PeriodSeconds = std::clamp(SanitizeFinite(ge.Policy.PeriodSeconds, 1.0f, "effect PeriodSeconds"), 0.01f, 60.0f);

                if (auto mods = effectNode["Modifiers"]; mods && mods.IsSequence())
                {
                    for (auto const& modNode : mods)
                    {
                        GameplayEffect::AttributeMod mod;
                        mod.AttributeName = modNode["Attribute"].as<std::string>("");
                        if (std::string op = modNode["Operation"].as<std::string>("Add"); op == "Multiply")
                            mod.Op = AttributeModifier::Operation::Multiply;
                        else if (op == "Override")
                            mod.Op = AttributeModifier::Operation::Override;
                        mod.Magnitude = modNode["Magnitude"].as<f32>(0.0f);
                        mod.Magnitude = std::clamp(SanitizeFinite(mod.Magnitude, 0.0f, "modifier Magnitude"), -1.0e6f, 1.0e6f);
                        ge.Modifiers.push_back(mod);
                    }
                }

                ge.MaxStacks = std::max(effectNode["MaxStacks"].as<i32>(1), 1);
                ge.RefreshDurationOnStack = effectNode["RefreshDurationOnStack"].as<bool>(true);

                ReadTagList(effectNode, "GrantedTags", ge.GrantedTags);
                ReadTagList(effectNode, "RequiredTags", ge.RequiredTags);
                ReadTagList(effectNode, "BlockedTags", ge.BlockedTags);

                effects.push_back(std::move(ge));
            }
        }

        void EmitAbilityDef(YAML::Emitter& out, const GameplayAbilityDef& def)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << def.Name;
            out << YAML::Key << "AbilityTag" << YAML::Value << def.AbilityTag.GetTagString();
            out << YAML::Key << "CooldownDuration" << YAML::Value << def.CooldownDuration;
            out << YAML::Key << "ResourceCost" << YAML::Value << def.ResourceCost;
            out << YAML::Key << "CostAttribute" << YAML::Value << def.CostAttribute;
            out << YAML::Key << "IsChanneled" << YAML::Value << def.IsChanneled;
            out << YAML::Key << "IsToggled" << YAML::Value << def.IsToggled;
            out << YAML::Key << "ChannelDuration" << YAML::Value << def.ChannelDuration;
            EmitTagList(out, "RequiredTags", def.RequiredTags);
            EmitTagList(out, "BlockedTags", def.BlockedTags);
            EmitTagList(out, "ActivationGrantedTags", def.ActivationGrantedTags);
            EmitEffectList(out, "ActivationEffects", def.ActivationEffects);
            EmitEffectList(out, "TargetActivationEffects", def.TargetActivationEffects);
            out << YAML::EndMap;
        }

        void ReadAbilityDef(const YAML::Node& node, GameplayAbilityDef& def)
        {
            def.Name = node["Name"].as<std::string>("");
            def.AbilityTag = GameplayTag(node["AbilityTag"].as<std::string>(""));
            def.CooldownDuration = std::clamp(SanitizeFinite(node["CooldownDuration"].as<f32>(0.0f), 0.0f, "ability CooldownDuration"), 0.0f, 600.0f);
            def.ResourceCost = std::clamp(SanitizeFinite(node["ResourceCost"].as<f32>(0.0f), 0.0f, "ability ResourceCost"), 0.0f, 10000.0f);
            def.CostAttribute = node["CostAttribute"].as<std::string>("Mana");
            def.IsChanneled = node["IsChanneled"].as<bool>(false);
            def.IsToggled = node["IsToggled"].as<bool>(false);
            def.ChannelDuration = std::clamp(SanitizeFinite(node["ChannelDuration"].as<f32>(0.0f), 0.0f, "ability ChannelDuration"), 0.0f, 60.0f);
            ReadTagList(node, "RequiredTags", def.RequiredTags);
            ReadTagList(node, "BlockedTags", def.BlockedTags);
            ReadTagList(node, "ActivationGrantedTags", def.ActivationGrantedTags);
            ReadEffectList(node, "ActivationEffects", def.ActivationEffects);
            ReadEffectList(node, "TargetActivationEffects", def.TargetActivationEffects);
        }
    } // namespace

    // ------------------------------------------------------------------
    // ExperienceCurveSerializer
    // ------------------------------------------------------------------

    void ExperienceCurveSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto curve = asset.As<ExperienceCurve>();
        if (!curve)
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer::Serialize - Failed to cast asset to ExperienceCurve ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(curve);
        auto fullPath = Project::GetProjectDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool ExperienceCurveSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("ExperienceCurveSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto curve = Ref<ExperienceCurve>::Create();
        if (!DeserializeFromYAML(ss.str(), curve))
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }

        curve->SetHandle(metadata.Handle);
        asset = curve;
        return true;
    }

    bool ExperienceCurveSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto curve = AssetManager::GetAsset<ExperienceCurve>(handle);
        if (!curve)
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }

        std::string yamlString = SerializeToYAML(curve);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> ExperienceCurveSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto curve = Ref<ExperienceCurve>::Create();
        if (!DeserializeFromYAML(yamlString, curve))
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        curve->SetHandle(assetInfo.Handle);
        return curve;
    }

    std::string ExperienceCurveSerializer::SerializeToYAML(const Ref<ExperienceCurve>& curve) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "ExperienceCurve" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Mode" << YAML::Value << (curve->m_Mode == ExperienceCurve::CurveMode::Table ? "Table" : "Formula");
        out << YAML::Key << "MaxLevel" << YAML::Value << curve->m_MaxLevel;
        out << YAML::Key << "BaseXP" << YAML::Value << curve->m_BaseXP;
        out << YAML::Key << "Exponent" << YAML::Value << curve->m_Exponent;
        if (!curve->m_Table.empty())
        {
            out << YAML::Key << "Table" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (i32 entry : curve->m_Table)
            {
                out << entry;
            }
            out << YAML::EndSeq;
        }
        out << YAML::EndMap;
        out << YAML::EndMap;
        return out.c_str();
    }

    bool ExperienceCurveSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<ExperienceCurve>& curve) const
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer - YAML parse error: {}", e.what());
            return false;
        }

        auto root = data["ExperienceCurve"];
        if (!root || !root.IsMap())
        {
            OLO_CORE_ERROR("ExperienceCurveSerializer - Missing 'ExperienceCurve' root key");
            return false;
        }

        curve->m_Mode = root["Mode"].as<std::string>("Formula") == "Table" ? ExperienceCurve::CurveMode::Table
                                                                           : ExperienceCurve::CurveMode::Formula;
        curve->m_MaxLevel = root["MaxLevel"].as<i32>(50);
        curve->m_BaseXP = SanitizeFinite(root["BaseXP"].as<f32>(100.0f), 100.0f, "curve BaseXP");
        curve->m_Exponent = SanitizeFinite(root["Exponent"].as<f32>(1.5f), 1.5f, "curve Exponent");
        curve->m_Table.clear();
        if (auto table = root["Table"]; table && table.IsSequence())
        {
            curve->m_Table.reserve(table.size());
            for (auto const& entry : table)
            {
                curve->m_Table.push_back(entry.as<i32>(1));
            }
        }
        curve->Sanitize();

        if (curve->m_Mode == ExperienceCurve::CurveMode::Table && curve->m_Table.empty())
        {
            OLO_CORE_WARN("ExperienceCurveSerializer - Table mode with empty table; falling back to Formula");
            curve->m_Mode = ExperienceCurve::CurveMode::Formula;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // SkillTreeDatabaseSerializer
    // ------------------------------------------------------------------

    void SkillTreeDatabaseSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto tree = asset.As<SkillTreeDatabase>();
        if (!tree)
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer::Serialize - Failed to cast asset to SkillTreeDatabase ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(tree);
        auto fullPath = Project::GetProjectDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool SkillTreeDatabaseSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("SkillTreeDatabaseSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto tree = Ref<SkillTreeDatabase>::Create();
        if (!DeserializeFromYAML(ss.str(), tree))
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }

        tree->SetHandle(metadata.Handle);
        asset = tree;
        return true;
    }

    bool SkillTreeDatabaseSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto tree = AssetManager::GetAsset<SkillTreeDatabase>(handle);
        if (!tree)
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }

        std::string yamlString = SerializeToYAML(tree);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> SkillTreeDatabaseSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto tree = Ref<SkillTreeDatabase>::Create();
        if (!DeserializeFromYAML(yamlString, tree))
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        tree->SetHandle(assetInfo.Handle);
        return tree;
    }

    std::string SkillTreeDatabaseSerializer::SerializeToYAML(const Ref<SkillTreeDatabase>& tree) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "SkillTree" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "TreeID" << YAML::Value << tree->m_TreeID;
        out << YAML::Key << "DisplayName" << YAML::Value << tree->m_DisplayName;

        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const auto& node : tree->m_Nodes)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "NodeID" << YAML::Value << node.NodeID;
            out << YAML::Key << "DisplayName" << YAML::Value << node.DisplayName;
            out << YAML::Key << "Description" << YAML::Value << node.Description;
            out << YAML::Key << "LevelRequirement" << YAML::Value << node.LevelRequirement;
            out << YAML::Key << "SkillPointCost" << YAML::Value << node.SkillPointCost;
            out << YAML::Key << "Prerequisites" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const auto& prereq : node.Prerequisites)
            {
                out << prereq;
            }
            out << YAML::EndSeq;
            out << YAML::Key << "EditorPosition" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << node.EditorPosition.x << node.EditorPosition.y << YAML::EndSeq;

            std::string payload = "None";
            if (node.Payload == SkillTreeNode::PayloadKind::Ability)
                payload = "Ability";
            else if (node.Payload == SkillTreeNode::PayloadKind::PassiveEffect)
                payload = "PassiveEffect";
            out << YAML::Key << "Payload" << YAML::Value << payload;

            if (node.Payload == SkillTreeNode::PayloadKind::Ability)
            {
                out << YAML::Key << "Ability" << YAML::Value;
                EmitAbilityDef(out, node.GrantedAbility);
            }
            else if (node.Payload == SkillTreeNode::PayloadKind::PassiveEffect)
            {
                std::vector<GameplayEffect> single{ node.PassiveEffect };
                EmitEffectList(out, "PassiveEffect", single);
            }
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;
        out << YAML::EndMap;
        return out.c_str();
    }

    bool SkillTreeDatabaseSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<SkillTreeDatabase>& tree) const
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer - YAML parse error: {}", e.what());
            return false;
        }

        auto root = data["SkillTree"];
        if (!root || !root.IsMap())
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer - Missing 'SkillTree' root key");
            return false;
        }

        // Parse into locals; commit to the asset only after validation.
        std::string treeId = root["TreeID"].as<std::string>("");
        std::string displayName = root["DisplayName"].as<std::string>("");
        std::vector<SkillTreeNode> nodes;

        if (auto nodesNode = root["Nodes"]; nodesNode && nodesNode.IsSequence())
        {
            nodes.reserve(nodesNode.size());
            for (auto const& nodeNode : nodesNode)
            {
                SkillTreeNode node;
                node.NodeID = nodeNode["NodeID"].as<std::string>("");
                if (node.NodeID.empty())
                {
                    OLO_CORE_ERROR("SkillTreeDatabaseSerializer - Node with empty NodeID");
                    return false;
                }
                node.DisplayName = nodeNode["DisplayName"].as<std::string>("");
                node.Description = nodeNode["Description"].as<std::string>("");
                node.LevelRequirement = std::max(nodeNode["LevelRequirement"].as<i32>(1), 1);
                node.SkillPointCost = std::max(nodeNode["SkillPointCost"].as<i32>(1), 0);
                if (auto prereqs = nodeNode["Prerequisites"]; prereqs && prereqs.IsSequence())
                {
                    for (auto const& prereq : prereqs)
                    {
                        if (std::string id = prereq.as<std::string>(""); !id.empty())
                        {
                            node.Prerequisites.push_back(std::move(id));
                        }
                    }
                }
                if (auto pos = nodeNode["EditorPosition"]; pos && pos.IsSequence() && pos.size() == 2)
                {
                    node.EditorPosition.x = SanitizeFinite(pos[0].as<f32>(0.0f), 0.0f, "node EditorPosition.x");
                    node.EditorPosition.y = SanitizeFinite(pos[1].as<f32>(0.0f), 0.0f, "node EditorPosition.y");
                }

                std::string payload = nodeNode["Payload"].as<std::string>("None");
                if (payload == "Ability")
                {
                    node.Payload = SkillTreeNode::PayloadKind::Ability;
                    if (auto abilityNode = nodeNode["Ability"]; abilityNode && abilityNode.IsMap())
                    {
                        ReadAbilityDef(abilityNode, node.GrantedAbility);
                    }
                    if (node.GrantedAbility.AbilityTag.GetTagString().empty())
                    {
                        OLO_CORE_ERROR("SkillTreeDatabaseSerializer - Ability node '{}' has no AbilityTag", node.NodeID);
                        return false;
                    }
                }
                else if (payload == "PassiveEffect")
                {
                    node.Payload = SkillTreeNode::PayloadKind::PassiveEffect;
                    std::vector<GameplayEffect> effects;
                    ReadEffectList(nodeNode, "PassiveEffect", effects);
                    if (!effects.empty())
                    {
                        node.PassiveEffect = std::move(effects.front());
                    }
                    // Skill passives are always infinite while unlocked.
                    node.PassiveEffect.Policy.DurationType = GameplayEffectPolicy::Duration::Infinite;
                }

                nodes.push_back(std::move(node));
            }
        }

        // Validate on a scratch asset before committing (duplicate ids,
        // dangling prerequisites, prerequisite cycles all reject the asset).
        auto scratch = Ref<SkillTreeDatabase>::Create();
        scratch->m_TreeID = std::move(treeId);
        scratch->m_DisplayName = std::move(displayName);
        scratch->m_Nodes = std::move(nodes);
        if (std::string error; !scratch->Validate(&error))
        {
            OLO_CORE_ERROR("SkillTreeDatabaseSerializer - Validation failed: {}", error);
            return false;
        }

        tree->m_TreeID = std::move(scratch->m_TreeID);
        tree->m_DisplayName = std::move(scratch->m_DisplayName);
        tree->m_Nodes = std::move(scratch->m_Nodes);
        tree->RebuildIndex();
        return true;
    }

    // ------------------------------------------------------------------
    // CharacterClassDatabaseSerializer
    // ------------------------------------------------------------------

    void CharacterClassDatabaseSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto classDb = asset.As<CharacterClassDatabase>();
        if (!classDb)
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer::Serialize - Failed to cast asset to CharacterClassDatabase ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(classDb);
        auto fullPath = Project::GetProjectDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool CharacterClassDatabaseSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("CharacterClassDatabaseSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto classDb = Ref<CharacterClassDatabase>::Create();
        if (!DeserializeFromYAML(ss.str(), classDb))
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }

        classDb->SetHandle(metadata.Handle);
        asset = classDb;
        return true;
    }

    bool CharacterClassDatabaseSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto classDb = AssetManager::GetAsset<CharacterClassDatabase>(handle);
        if (!classDb)
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }

        std::string yamlString = SerializeToYAML(classDb);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> CharacterClassDatabaseSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto classDb = Ref<CharacterClassDatabase>::Create();
        if (!DeserializeFromYAML(yamlString, classDb))
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        classDb->SetHandle(assetInfo.Handle);
        return classDb;
    }

    std::string CharacterClassDatabaseSerializer::SerializeToYAML(const Ref<CharacterClassDatabase>& classDb) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "CharacterClasses" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Classes" << YAML::Value << YAML::BeginSeq;
        for (const auto& classDef : classDb->m_Classes)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ClassID" << YAML::Value << classDef.ClassID;
            out << YAML::Key << "DisplayName" << YAML::Value << classDef.DisplayName;
            out << YAML::Key << "Description" << YAML::Value << classDef.Description;
            out << YAML::Key << "AttributePointsPerLevel" << YAML::Value << classDef.AttributePointsPerLevel;
            out << YAML::Key << "SkillPointsPerLevel" << YAML::Value << classDef.SkillPointsPerLevel;
            out << YAML::Key << "LevelCap" << YAML::Value << classDef.LevelCap;
            out << YAML::Key << "ExperienceCurve" << YAML::Value << static_cast<u64>(classDef.ExperienceCurve);
            out << YAML::Key << "SkillTrees" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (AssetHandle handle : classDef.SkillTrees)
            {
                out << static_cast<u64>(handle);
            }
            out << YAML::EndSeq;
            out << YAML::Key << "StartingTags" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const auto& tag : classDef.StartingTags)
            {
                out << tag;
            }
            out << YAML::EndSeq;

            out << YAML::Key << "Attributes" << YAML::Value << YAML::BeginSeq;
            for (const auto& spec : classDef.Attributes)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Attribute" << YAML::Value << spec.Attribute;
                out << YAML::Key << "BaseValue" << YAML::Value << spec.BaseValue;
                out << YAML::Key << "GrowthPerLevel" << YAML::Value << spec.GrowthPerLevel;
                out << YAML::Key << "ValuePerPoint" << YAML::Value << spec.ValuePerPoint;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::Key << "StartingAbilities" << YAML::Value << YAML::BeginSeq;
            for (const auto& def : classDef.StartingAbilities)
            {
                EmitAbilityDef(out, def);
            }
            out << YAML::EndSeq;

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
        out << YAML::EndMap;
        return out.c_str();
    }

    bool CharacterClassDatabaseSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<CharacterClassDatabase>& classDb) const
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer - YAML parse error: {}", e.what());
            return false;
        }

        auto root = data["CharacterClasses"];
        if (!root || !root.IsMap())
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer - Missing 'CharacterClasses' root key");
            return false;
        }

        std::vector<CharacterClassDefinition> classes;
        if (auto classesNode = root["Classes"]; classesNode && classesNode.IsSequence())
        {
            classes.reserve(classesNode.size());
            for (auto const& classNode : classesNode)
            {
                CharacterClassDefinition classDef;
                classDef.ClassID = classNode["ClassID"].as<std::string>("");
                if (classDef.ClassID.empty())
                {
                    OLO_CORE_ERROR("CharacterClassDatabaseSerializer - Class with empty ClassID");
                    return false;
                }
                classDef.DisplayName = classNode["DisplayName"].as<std::string>("");
                classDef.Description = classNode["Description"].as<std::string>("");
                classDef.AttributePointsPerLevel = std::max(classNode["AttributePointsPerLevel"].as<i32>(5), 0);
                classDef.SkillPointsPerLevel = std::max(classNode["SkillPointsPerLevel"].as<i32>(1), 0);
                classDef.LevelCap = std::max(classNode["LevelCap"].as<i32>(0), 0);
                classDef.ExperienceCurve = AssetHandle(classNode["ExperienceCurve"].as<u64>(0));
                if (auto trees = classNode["SkillTrees"]; trees && trees.IsSequence())
                {
                    for (auto const& handle : trees)
                    {
                        classDef.SkillTrees.push_back(AssetHandle(handle.as<u64>(0)));
                    }
                }
                if (auto tags = classNode["StartingTags"]; tags && tags.IsSequence())
                {
                    for (auto const& tag : tags)
                    {
                        if (std::string tagString = tag.as<std::string>(""); !tagString.empty())
                        {
                            classDef.StartingTags.push_back(std::move(tagString));
                        }
                    }
                }
                if (auto attributes = classNode["Attributes"]; attributes && attributes.IsSequence())
                {
                    for (auto const& attrNode : attributes)
                    {
                        ClassAttributeSpec spec;
                        spec.Attribute = attrNode["Attribute"].as<std::string>("");
                        if (spec.Attribute.empty())
                        {
                            OLO_CORE_WARN("CharacterClassDatabaseSerializer - Class '{}' has attribute spec with empty name; skipping", classDef.ClassID);
                            continue;
                        }
                        spec.BaseValue = std::clamp(SanitizeFinite(attrNode["BaseValue"].as<f32>(0.0f), 0.0f, "class BaseValue"), -1.0e6f, 1.0e6f);
                        spec.GrowthPerLevel = std::clamp(SanitizeFinite(attrNode["GrowthPerLevel"].as<f32>(0.0f), 0.0f, "class GrowthPerLevel"), -1.0e6f, 1.0e6f);
                        spec.ValuePerPoint = std::clamp(SanitizeFinite(attrNode["ValuePerPoint"].as<f32>(0.0f), 0.0f, "class ValuePerPoint"), 0.0f, 1.0e6f);
                        classDef.Attributes.push_back(std::move(spec));
                    }
                }
                if (auto abilities = classNode["StartingAbilities"]; abilities && abilities.IsSequence())
                {
                    for (auto const& abilityNode : abilities)
                    {
                        GameplayAbilityDef def;
                        ReadAbilityDef(abilityNode, def);
                        classDef.StartingAbilities.push_back(std::move(def));
                    }
                }
                classes.push_back(std::move(classDef));
            }
        }

        auto scratch = Ref<CharacterClassDatabase>::Create();
        scratch->m_Classes = std::move(classes);
        if (std::string error; !scratch->Validate(&error))
        {
            OLO_CORE_ERROR("CharacterClassDatabaseSerializer - Validation failed: {}", error);
            return false;
        }

        classDb->m_Classes = std::move(scratch->m_Classes);
        classDb->RebuildIndex();
        return true;
    }
} // namespace OloEngine
