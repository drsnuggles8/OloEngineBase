#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"

#include <yaml-cpp/yaml.h>
#include <filesystem>

namespace OloEngine
{
    std::unordered_map<std::string, QuestDefinition>& QuestDatabase::GetQuests()
    {
        static std::unordered_map<std::string, QuestDefinition> s_Quests;
        return s_Quests;
    }

    void QuestDatabase::LoadFromDirectory(const std::string& path)
    {
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("[QuestDatabase] Directory not found: {}", path);
            return;
        }

        for (auto const& entry : std::filesystem::recursive_directory_iterator(path))
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
                    def.RequiredLevel = root["RequiredLevel"].as<i32>(0);
                    def.CanFail = root["CanFail"].as<bool>(false);
                    def.TimeLimit = root["TimeLimit"].as<f32>(-1.0f);
                    def.IsRepeatable = root["IsRepeatable"].as<bool>(false);
                    def.RepeatCooldownSeconds = root["RepeatCooldownSeconds"].as<f32>(0.0f);

                    if (auto prereqs = root["RequiredCompletedQuests"]; prereqs && prereqs.IsSequence())
                    {
                        for (auto const& node : prereqs)
                        {
                            def.RequiredCompletedQuests.push_back(node.as<std::string>());
                        }
                    }

                    if (auto tags = root["RequiredTags"]; tags && tags.IsSequence())
                    {
                        for (auto const& node : tags)
                        {
                            def.RequiredTags.push_back(node.as<std::string>());
                        }
                    }

                    if (auto failTags = root["FailOnTags"]; failTags && failTags.IsSequence())
                    {
                        for (auto const& node : failTags)
                        {
                            def.FailOnTags.push_back(node.as<std::string>());
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
                                    obj.RequiredCount = objNode["RequiredCount"].as<i32>(1);
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
    }

    void QuestDatabase::Register(const QuestDefinition& definition)
    {
        GetQuests()[definition.QuestID] = definition;
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
