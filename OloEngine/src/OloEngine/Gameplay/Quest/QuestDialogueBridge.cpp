#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Quest/QuestDialogueBridge.h"

#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <string>
#include <string_view>
#include <utility>

namespace OloEngine
{
    namespace
    {
        std::string Trim(std::string_view s)
        {
            constexpr std::string_view kWhitespace = " \t\r\n";
            const auto begin = s.find_first_not_of(kWhitespace);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            const auto end = s.find_last_not_of(kWhitespace);
            return std::string(s.substr(begin, end - begin + 1));
        }

        // Split "head:tail" on the first ':'. No ':' => {whole, ""}. Both parts
        // are trimmed, so "Q1 : spare" yields {"Q1", "spare"}.
        std::pair<std::string, std::string> SplitFirst(const std::string& in, char delim)
        {
            const auto pos = in.find(delim);
            if (pos == std::string::npos)
            {
                return { Trim(in), std::string{} };
            }
            return { Trim(std::string_view(in).substr(0, pos)), Trim(std::string_view(in).substr(pos + 1)) };
        }

        // The player whose journal a dialogue mutates / queries. The dialogue
        // callbacks carry only the speaking entity, not the player, so the
        // bridge recovers the journal owner by scanning the scene. The
        // foundational slice assumes a single QuestJournalComponent (the local
        // player); multi-journal / co-op routing is a follow-up.
        Entity ResolveJournalEntity(Scene& scene)
        {
            auto view = scene.GetAllEntitiesWith<QuestJournalComponent>();
            if (auto it = view.begin(); it != view.end())
            {
                return Entity{ *it, &scene };
            }
            return {};
        }

        // The speaking NPC the dialogue is running on, by the UUID the
        // DialogueSystem hands each handler. Invalid Entity if it can't be
        // resolved (e.g. destroyed mid-frame).
        Entity ResolveDialogueEntity(const Scene& scene, UUID dialogueEntity)
        {
            return scene.TryGetEntityWithUUID(dialogueEntity).value_or(Entity{});
        }

        // Quest id for an accept action: an explicit arg wins; an empty arg
        // falls back to the speaking NPC's first offered quest. This is what
        // ties a bare `accept_quest` node to the NPC's QuestGiverComponent.
        std::string ResolveAcceptQuestId(const std::string& arg, Entity npc)
        {
            if (std::string explicitId = Trim(arg); !explicitId.empty())
            {
                return explicitId;
            }
            if (npc && npc.HasComponent<QuestGiverComponent>())
            {
                if (const auto& giver = npc.GetComponent<QuestGiverComponent>(); !giver.OfferedQuestIDs.empty())
                {
                    return giver.OfferedQuestIDs.front();
                }
            }
            return {};
        }
    } // namespace

    void RegisterQuestDialogueHandlers(DialogueSystem& dialogue, Scene& scene)
    {
        Scene* scenePtr = &scene;

        // ----------------------------- actions -------------------------------

        dialogue.RegisterActionHandler("accept_quest", [scenePtr](UUID dialogueEntity, const std::string&, const std::string& args)
                                       {
            const Entity npc = ResolveDialogueEntity(*scenePtr, dialogueEntity);
            const std::string questId = ResolveAcceptQuestId(args, npc);
            if (questId.empty())
            {
                OLO_CORE_WARN("[QuestDialogue] accept_quest: no quest id (empty args and no QuestGiverComponent offer on the speaking entity)");
                return;
            }
            const Entity player = ResolveJournalEntity(*scenePtr);
            if (!player)
            {
                OLO_CORE_WARN("[QuestDialogue] accept_quest '{}': no QuestJournalComponent in scene", questId);
                return;
            }
            QuestSystem::AcceptQuest(scenePtr, player, questId); });

        dialogue.RegisterActionHandler("advance_quest", [scenePtr](UUID, const std::string&, const std::string& args)
                                       {
            auto [questId, objectiveId] = SplitFirst(args, ':');
            if (questId.empty() || objectiveId.empty())
            {
                OLO_CORE_WARN("[QuestDialogue] advance_quest expects 'questId:objectiveId', got '{}'", args);
                return;
            }
            const Entity player = ResolveJournalEntity(*scenePtr);
            if (!player)
            {
                return;
            }
            QuestSystem::SetObjectiveComplete(scenePtr, player, questId, objectiveId); });

        dialogue.RegisterActionHandler("complete_quest", [scenePtr](UUID, const std::string&, const std::string& args)
                                       {
            auto [questId, branchChoice] = SplitFirst(args, ':');
            if (questId.empty())
            {
                OLO_CORE_WARN("[QuestDialogue] complete_quest expects 'questId' or 'questId:branch', got '{}'", args);
                return;
            }
            const Entity player = ResolveJournalEntity(*scenePtr);
            if (!player)
            {
                return;
            }
            QuestSystem::CompleteQuest(scenePtr, player, questId, branchChoice); });

        dialogue.RegisterActionHandler("fail_quest", [scenePtr](UUID, const std::string&, const std::string& args)
                                       {
            const std::string questId = Trim(args);
            if (questId.empty())
            {
                OLO_CORE_WARN("[QuestDialogue] fail_quest expects 'questId', got empty args");
                return;
            }
            const Entity player = ResolveJournalEntity(*scenePtr);
            if (!player)
            {
                return;
            }
            QuestSystem::FailQuest(scenePtr, player, questId); });

        // --------------------------- conditions ------------------------------

        dialogue.RegisterConditionHandler("quest_active", [scenePtr](UUID, const std::string&, const std::string& args)
                                          {
            const std::string questId = Trim(args);
            const Entity player = ResolveJournalEntity(*scenePtr);
            if (!player || questId.empty())
            {
                return false;
            }
            return player.GetComponent<QuestJournalComponent>().Journal.IsQuestActive(questId); });

        dialogue.RegisterConditionHandler("quest_completed", [scenePtr](UUID, const std::string&, const std::string& args)
                                          {
            const std::string questId = Trim(args);
            const Entity player = ResolveJournalEntity(*scenePtr);
            if (!player || questId.empty())
            {
                return false;
            }
            return player.GetComponent<QuestJournalComponent>().Journal.HasCompletedQuest(questId); });

        dialogue.RegisterConditionHandler("has_quest", [scenePtr](UUID, const std::string&, const std::string& args)
                                          {
            const std::string questId = Trim(args);
            const Entity player = ResolveJournalEntity(*scenePtr);
            if (!player || questId.empty())
            {
                return false;
            }
            // "Has" = the journal knows the quest in any tracked state. A quest
            // never accepted reports Unavailable.
            return player.GetComponent<QuestJournalComponent>().Journal.GetQuestStatus(questId) != QuestStatus::Unavailable; });
    }

} // namespace OloEngine
