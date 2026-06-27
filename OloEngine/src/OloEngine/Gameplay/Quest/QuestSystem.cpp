#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestEvents.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <utility>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Translate the journal's change records into the public, entity-stamped
        // QuestEvents payloads and publish them on the scene's bus.
        void PublishQuestChanges(Scene* scene, UUID entityId, const QuestEventSink& changes)
        {
            if (changes.empty())
            {
                return;
            }

            const GameplayEventBus& bus = scene->GetGameplayEvents();
            using enum QuestJournalChange::Type;
            for (auto const& c : changes)
            {
                switch (c.Kind)
                {
                    case QuestStarted:
                        bus.Publish(QuestStartedEvent{ entityId, c.QuestID });
                        break;
                    case QuestAbandoned:
                        bus.Publish(QuestAbandonedEvent{ entityId, c.QuestID });
                        break;
                    case QuestFailed:
                        bus.Publish(QuestFailedEvent{ entityId, c.QuestID });
                        break;
                    case QuestCompleted:
                        bus.Publish(QuestCompletedEvent{ entityId, c.QuestID, c.BranchChoice });
                        break;
                    case ObjectiveProgress:
                        bus.Publish(ObjectiveProgressEvent{ entityId, c.QuestID, c.ObjectiveID, c.CurrentCount, c.RequiredCount });
                        break;
                    case ObjectiveCompleted:
                        bus.Publish(ObjectiveCompletedEvent{ entityId, c.QuestID, c.ObjectiveID });
                        break;
                    case StageAdvanced:
                        bus.Publish(QuestStageAdvancedEvent{ entityId, c.QuestID, c.NewStageIndex });
                        break;
                    default:
                        break;
                }
            }
        }

        // Common guard: returns the QuestJournalComponent pointer or nullptr.
        QuestJournalComponent* ResolveJournal(const Scene* scene, Entity entity)
        {
            if (!scene || !entity || !entity.HasComponent<QuestJournalComponent>())
            {
                return nullptr;
            }
            return &entity.GetComponent<QuestJournalComponent>();
        }
    } // namespace

    void QuestSystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
        {
            return;
        }

        // Accumulate (entity, changes) during the view walk and publish after,
        // so a subscriber that mutates the registry can't invalidate the view.
        std::vector<std::pair<UUID, QuestEventSink>> pending;

        auto journalView = scene->GetAllEntitiesWith<QuestJournalComponent>();
        for (auto e : journalView)
        {
            Entity entity = { e, scene };
            auto& jc = entity.GetComponent<QuestJournalComponent>();

            QuestEventSink changes;
            jc.Journal.UpdateTimers(dt, &changes);
            if (!changes.empty())
            {
                pending.emplace_back(entity.GetUUID(), std::move(changes));
            }
        }

        for (auto const& [entityId, changes] : pending)
        {
            PublishQuestChanges(scene, entityId, changes);
        }
    }

    bool QuestSystem::AcceptQuest(Scene* scene, Entity entity, const std::string& questId, const QuestDefinition& definition)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return false;
        }

        QuestEventSink changes;
        bool accepted = jc->Journal.AcceptQuest(questId, definition, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
        return accepted;
    }

    bool QuestSystem::AcceptQuest(Scene* scene, Entity entity, const std::string& questId)
    {
        const QuestDefinition* def = QuestDatabase::Get(questId);
        if (!def)
        {
            return false;
        }
        return AcceptQuest(scene, entity, questId, *def);
    }

    bool QuestSystem::AbandonQuest(Scene* scene, Entity entity, const std::string& questId)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return false;
        }

        QuestEventSink changes;
        bool abandoned = jc->Journal.AbandonQuest(questId, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
        return abandoned;
    }

    bool QuestSystem::CompleteQuest(Scene* scene, Entity entity, const std::string& questId, const std::string& branchChoice)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return false;
        }

        QuestEventSink changes;
        bool completed = jc->Journal.CompleteQuest(questId, branchChoice, &changes).has_value();
        PublishQuestChanges(scene, entity.GetUUID(), changes);
        return completed;
    }

    bool QuestSystem::FailQuest(Scene* scene, Entity entity, const std::string& questId)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return false;
        }

        QuestEventSink changes;
        bool failed = jc->Journal.FailQuest(questId, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
        return failed;
    }

    void QuestSystem::IncrementObjective(Scene* scene, Entity entity, const std::string& questId, const std::string& objectiveId, i32 amount)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return;
        }

        QuestEventSink changes;
        jc->Journal.IncrementObjective(questId, objectiveId, amount, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
    }

    void QuestSystem::SetObjectiveComplete(Scene* scene, Entity entity, const std::string& questId, const std::string& objectiveId)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return;
        }

        QuestEventSink changes;
        jc->Journal.SetObjectiveComplete(questId, objectiveId, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
    }

    void QuestSystem::NotifyKill(Scene* scene, Entity entity, const std::string& targetTag)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return;
        }

        QuestEventSink changes;
        jc->Journal.NotifyKill(targetTag, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
    }

    void QuestSystem::NotifyCollect(Scene* scene, Entity entity, const std::string& itemId, i32 count)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return;
        }

        QuestEventSink changes;
        jc->Journal.NotifyCollect(itemId, count, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
    }

    void QuestSystem::NotifyInteract(Scene* scene, Entity entity, const std::string& interactableId)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return;
        }

        QuestEventSink changes;
        jc->Journal.NotifyInteract(interactableId, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
    }

    void QuestSystem::NotifyReachLocation(Scene* scene, Entity entity, const std::string& locationId)
    {
        QuestJournalComponent* jc = ResolveJournal(scene, entity);
        if (!jc)
        {
            return;
        }

        QuestEventSink changes;
        jc->Journal.NotifyReachLocation(locationId, &changes);
        PublishQuestChanges(scene, entity.GetUUID(), changes);
    }

} // namespace OloEngine
