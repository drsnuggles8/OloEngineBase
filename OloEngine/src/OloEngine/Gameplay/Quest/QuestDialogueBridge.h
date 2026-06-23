#pragma once

namespace OloEngine
{
    class DialogueSystem;
    class Scene;

    // Glue that lets dialogue nodes drive the quest system without either
    // subsystem including the other. Registers a set of named handlers on the
    // given DialogueSystem:
    //
    //   actions (node Type == "action", `actionName` / `actionArgs` props):
    //     accept_quest    args: "<questId>"  (empty => the speaking NPC's
    //                     first QuestGiverComponent.OfferedQuestIDs entry)
    //     advance_quest   args: "<questId>:<objectiveId>"  (marks the objective
    //                     complete, cascading stage / quest completion)
    //     complete_quest  args: "<questId>"  or  "<questId>:<branchChoice>"
    //     fail_quest      args: "<questId>"
    //
    //   conditions (node Type == "condition", `conditionExpression` /
    //   `conditionArgs` props, and per-choice conditions):
    //     quest_active    args: "<questId>"  -> journal.IsQuestActive
    //     quest_completed args: "<questId>"  -> journal.HasCompletedQuest
    //     has_quest       args: "<questId>"  -> the journal knows the quest in
    //                     any state (active / completed / failed)
    //
    // The handlers mutate / query the journal of the player — resolved as the
    // (single) entity carrying a QuestJournalComponent in `scene`. The accept
    // action additionally reads the speaking NPC's QuestGiverComponent, found
    // via the dialogue-entity UUID the DialogueSystem hands each handler.
    //
    // Quests referenced by id must be registered in the global QuestDatabase
    // (accept looks the definition up there).
    //
    // Called from Scene::InitDialogueSystem so both the runtime
    // (OnRuntimeStart) and headless test harnesses wire the same handlers.
    void RegisterQuestDialogueHandlers(DialogueSystem& dialogue, Scene& scene);

} // namespace OloEngine
