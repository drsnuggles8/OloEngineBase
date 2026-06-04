#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
    class Scene;
    class Entity;
    struct QuestDefinition;

    // Entity-aware orchestration over the pure QuestJournal value type.
    //
    // The data-structure methods on QuestJournal are intentionally
    // entity-agnostic (they're serialized/undo-tracked value types). This
    // system is the layer that *does* know the owning entity, so it is the
    // single place that translates journal mutations into the entity-stamped
    // QuestEvents.h payloads and publishes them on Scene::GetGameplayEvents().
    //
    // Gameplay code, scripting glue, and UI should call these instead of
    // mutating the journal directly when they want the change to be observable
    // (the raw QuestJournal API stays available for tests / low-level use, but
    // it emits no events).
    class QuestSystem
    {
      public:
        // Per-frame: ticks timed-quest deadlines and publishes QuestFailed for
        // any quest that expires this tick.
        static void OnUpdate(Scene* scene, f32 dt);

        // Accept a quest by definition (publishes QuestStarted on success).
        static bool AcceptQuest(Scene* scene, Entity entity, const std::string& questId, const QuestDefinition& definition);
        // Accept a quest, looking the definition up in the global QuestDatabase.
        static bool AcceptQuest(Scene* scene, Entity entity, const std::string& questId);

        // Abandon an active quest (publishes QuestAbandoned on success).
        static bool AbandonQuest(Scene* scene, Entity entity, const std::string& questId);

        // Complete a quest with an optional branch choice (publishes
        // QuestCompleted on success). Returns true if the quest completed.
        static bool CompleteQuest(Scene* scene, Entity entity, const std::string& questId, const std::string& branchChoice = "");

        // Explicitly fail a quest (publishes QuestFailed on success).
        static bool FailQuest(Scene* scene, Entity entity, const std::string& questId);

        // Objective progress. Each publishes ObjectiveProgress /
        // ObjectiveCompleted and any StageAdvanced / QuestCompleted the change
        // cascades into.
        static void IncrementObjective(Scene* scene, Entity entity, const std::string& questId, const std::string& objectiveId, i32 amount = 1);
        static void SetObjectiveComplete(Scene* scene, Entity entity, const std::string& questId, const std::string& objectiveId);

        // Notify-based progress by objective type (publishes the same cascade
        // across every active quest with a matching objective).
        static void NotifyKill(Scene* scene, Entity entity, const std::string& targetTag);
        static void NotifyCollect(Scene* scene, Entity entity, const std::string& itemId, i32 count = 1);
        static void NotifyInteract(Scene* scene, Entity entity, const std::string& interactableId);
        static void NotifyReachLocation(Scene* scene, Entity entity, const std::string& locationId);
    };

} // namespace OloEngine
