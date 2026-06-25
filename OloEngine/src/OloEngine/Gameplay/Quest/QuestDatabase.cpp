#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <filesystem>

namespace OloEngine
{
    namespace
    {
        // Validate a float read from an .oloquest YAML file (cpp-coding-quality §2b).
        // A corrupt or hand-edited asset can carry NaN/±inf; substitute a safe fallback
        // so it never reaches quest timers / cooldowns.
        [[nodiscard]] f32 SanitizeFinite(f32 value, f32 fallback, const char* field, const std::string& questId)
        {
            if (!std::isfinite(value))
            {
                OLO_CORE_WARN("[QuestDatabase] Quest '{}' has non-finite {} ({}); using {}", questId, field, value, fallback);
                return fallback;
            }
            return value;
        }
    } // namespace

    static std::optional<QuestRequirement> ParseRequirementNode(const YAML::Node& node)
    {
        QuestRequirement req;

        auto typeStr = node["Type"].as<std::string>("");
        auto typeOpt = RequirementTypeFromString(typeStr);
        if (!typeOpt)
        {
            OLO_CORE_ERROR("[QuestDatabase] Invalid requirement type '{}', skipping", typeStr);
            return std::nullopt;
        }
        req.Type = *typeOpt;

        req.Target = node["Target"].as<std::string>("");
        req.Value = node["Value"].as<i32>(0);

        if (auto compStr = node["Comparison"].as<std::string>(""); !compStr.empty())
        {
            auto compOpt = ComparisonOpFromString(compStr);
            if (!compOpt)
            {
                OLO_CORE_WARN("[QuestDatabase] Unknown comparison '{}', defaulting to >=", compStr);
                req.Comparison = ComparisonOp::GreaterThanOrEqual;
            }
            else
            {
                req.Comparison = *compOpt;
            }
        }

        req.Description = node["Description"].as<std::string>("");

        if (auto children = node["Children"]; children && children.IsSequence())
        {
            for (auto const& childNode : children)
            {
                if (auto child = ParseRequirementNode(childNode))
                {
                    req.Children.push_back(std::move(*child));
                }
            }
        }

        return req;
    }

    std::unordered_map<std::string, QuestDefinition>& QuestDatabase::GetQuests()
    {
        static std::unordered_map<std::string, QuestDefinition> s_Quests;
        return s_Quests;
    }

    void QuestDatabase::LoadFromDirectory(const std::string& path)
    {
        if (!std::filesystem::is_directory(path))
        {
            OLO_CORE_WARN("[QuestDatabase] Directory not found: {}", path);
            return;
        }

        Clear();

        std::error_code ec;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, ec))
        {
            if (entry.path().extension() == ".oloquest")
            {
                try
                {
                    YAML::Node root = YAML::LoadFile(entry.path().string());
                    QuestDefinition def;

                    def.QuestID = root["QuestID"].as<std::string>("");
                    if (def.QuestID.empty())
                    {
                        OLO_CORE_WARN("[QuestDatabase] Skipping file with no QuestID: {}", entry.path().string());
                        continue;
                    }

                    def.Title = root["Title"].as<std::string>("");
                    def.Description = root["Description"].as<std::string>("");
                    def.Category = root["Category"].as<std::string>("Side");
                    def.CanFail = root["CanFail"].as<bool>(false);
                    // TimeLimit: -1 (or any value <= 0) means "no time limit"; reject NaN/±inf.
                    def.TimeLimit = SanitizeFinite(root["TimeLimit"].as<f32>(-1.0f), -1.0f, "TimeLimit", def.QuestID);
                    def.IsRepeatable = root["IsRepeatable"].as<bool>(false);
                    // RepeatCooldownSeconds: reject NaN/±inf and negative cooldowns; fall back to 0 (no cooldown).
                    f32 cooldown = SanitizeFinite(root["RepeatCooldownSeconds"].as<f32>(0.0f), 0.0f, "RepeatCooldownSeconds", def.QuestID);
                    if (cooldown < 0.0f)
                    {
                        OLO_CORE_WARN("[QuestDatabase] Quest '{}' has negative RepeatCooldownSeconds ({}); using 0", def.QuestID, cooldown);
                        cooldown = 0.0f;
                    }
                    def.RepeatCooldownSeconds = cooldown;

                    if (auto failTags = root["FailOnTags"]; failTags && failTags.IsSequence())
                    {
                        for (auto const& node : failTags)
                        {
                            def.FailOnTags.push_back(node.as<std::string>());
                        }
                    }

                    // Parse Requirements block
                    if (auto requirements = root["Requirements"]; requirements && requirements.IsSequence())
                    {
                        for (auto const& reqNode : requirements)
                        {
                            if (auto req = ParseRequirementNode(reqNode))
                            {
                                def.Requirements.push_back(std::move(*req));
                            }
                        }
                    }

                    if (auto stages = root["Stages"]; stages && stages.IsSequence())
                    {
                        for (auto const& stageNode : stages)
                        {
                            QuestStage stage;
                            stage.StageID = stageNode["StageID"].as<std::string>("");
                            stage.Description = stageNode["Description"].as<std::string>("");
                            stage.RequireAllObjectives = stageNode["RequireAllObjectives"].as<bool>(true);

                            if (auto objectives = stageNode["Objectives"]; objectives && objectives.IsSequence())
                            {
                                for (auto const& objNode : objectives)
                                {
                                    QuestObjective obj;
                                    obj.ObjectiveID = objNode["ObjectiveID"].as<std::string>("");
                                    obj.Description = objNode["Description"].as<std::string>("");
                                    obj.ObjectiveType = ObjectiveTypeFromString(objNode["Type"].as<std::string>("Custom"));
                                    obj.TargetID = objNode["TargetID"].as<std::string>("");
                                    obj.RequiredCount = std::max(objNode["RequiredCount"].as<i32>(1), 1);
                                    obj.IsOptional = objNode["IsOptional"].as<bool>(false);
                                    obj.IsHidden = objNode["IsHidden"].as<bool>(false);
                                    stage.Objectives.push_back(std::move(obj));
                                }
                            }

                            def.Stages.push_back(std::move(stage));
                        }
                    }

                    if (auto choices = root["CompletionChoices"]; choices && choices.IsSequence())
                    {
                        for (auto const& choiceNode : choices)
                        {
                            QuestBranchChoice choice;
                            choice.ChoiceID = choiceNode["ChoiceID"].as<std::string>("");
                            choice.Description = choiceNode["Description"].as<std::string>("");
                            choice.NextQuestID = choiceNode["NextQuestID"].as<std::string>("");
                            if (auto grantedTags = choiceNode["GrantedTags"]; grantedTags && grantedTags.IsSequence())
                            {
                                for (auto const& tag : grantedTags)
                                {
                                    choice.GrantedTags.push_back(tag.as<std::string>());
                                }
                            }
                            def.CompletionChoices.push_back(std::move(choice));
                        }
                    }

                    if (auto rewards = root["CompletionRewards"]; rewards)
                    {
                        def.CompletionRewards.ExperiencePoints = rewards["ExperiencePoints"].as<i32>(0);
                        def.CompletionRewards.Currency = rewards["Currency"].as<i32>(0);
                        if (auto items = rewards["ItemRewards"]; items && items.IsSequence())
                        {
                            for (auto const& item : items)
                            {
                                def.CompletionRewards.ItemRewards.push_back(item.as<std::string>());
                            }
                        }
                        if (auto tags = rewards["GrantedTags"]; tags && tags.IsSequence())
                        {
                            for (auto const& tag : tags)
                            {
                                def.CompletionRewards.GrantedTags.push_back(tag.as<std::string>());
                            }
                        }
                    }

                    Register(def);
                    OLO_CORE_INFO("[QuestDatabase] Loaded quest '{}' from {}", def.QuestID, entry.path().string());
                }
                catch (std::exception const& e)
                {
                    OLO_CORE_ERROR("[QuestDatabase] Failed to load {}: {}", entry.path().string(), e.what());
                }
            }
        }

        if (ec)
        {
            OLO_CORE_ERROR("[QuestDatabase] Directory iteration error: {}", ec.message());
        }
    }

    void QuestDatabase::Register(const QuestDefinition& definition)
    {
        if (definition.QuestID.empty())
        {
            OLO_CORE_WARN("[QuestDatabase] Cannot register quest with empty QuestID");
            return;
        }

        auto& quests = GetQuests();
        if (quests.contains(definition.QuestID))
        {
            OLO_CORE_WARN("[QuestDatabase] Overwriting existing quest '{}'", definition.QuestID);
        }
        quests[definition.QuestID] = definition;
    }

    const QuestDefinition* QuestDatabase::Get(const std::string& questId)
    {
        auto& quests = GetQuests();
        auto it = quests.find(questId);
        return it != quests.end() ? &it->second : nullptr;
    }

    std::vector<const QuestDefinition*> QuestDatabase::GetByCategory(const std::string& category)
    {
        std::vector<const QuestDefinition*> result;
        for (auto const& [id, def] : GetQuests())
        {
            if (def.Category == category)
            {
                result.push_back(&def);
            }
        }
        return result;
    }

    std::vector<const QuestDefinition*> QuestDatabase::GetAll()
    {
        std::vector<const QuestDefinition*> result;
        for (auto const& [id, def] : GetQuests())
        {
            result.push_back(&def);
        }
        return result;
    }

    void QuestDatabase::Clear()
    {
        GetQuests().clear();
    }

} // namespace OloEngine
