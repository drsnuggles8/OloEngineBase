#include "OloEnginePCH.h"
#include "GameplayEventLogger.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Quest/QuestEvents.h"
#include "OloEngine/Gameplay/Inventory/InventoryEvents.h"

namespace OloEngine
{
    void AttachGameplayEventLogger(Scene& scene)
    {
        GameplayEventBus& bus = scene.GetGameplayEvents();

        // --- Quest events -----------------------------------------------------
        bus.Subscribe<QuestStartedEvent>([](const QuestStartedEvent& e)
                                         { OLO_CORE_INFO("[GameplayEvents] QuestStarted    entity={} quest='{}'", static_cast<u64>(e.EntityID), e.QuestID); });

        bus.Subscribe<ObjectiveProgressEvent>([](const ObjectiveProgressEvent& e)
                                              { OLO_CORE_INFO("[GameplayEvents] ObjectiveProgress entity={} quest='{}' objective='{}' {}/{}", static_cast<u64>(e.EntityID), e.QuestID, e.ObjectiveID, e.CurrentCount, e.RequiredCount); });

        bus.Subscribe<ObjectiveCompletedEvent>([](const ObjectiveCompletedEvent& e)
                                               { OLO_CORE_INFO("[GameplayEvents] ObjectiveCompleted entity={} quest='{}' objective='{}'", static_cast<u64>(e.EntityID), e.QuestID, e.ObjectiveID); });

        bus.Subscribe<QuestStageAdvancedEvent>([](const QuestStageAdvancedEvent& e)
                                               { OLO_CORE_INFO("[GameplayEvents] QuestStageAdvanced entity={} quest='{}' newStage={}", static_cast<u64>(e.EntityID), e.QuestID, e.NewStageIndex); });

        bus.Subscribe<QuestCompletedEvent>([](const QuestCompletedEvent& e)
                                           { OLO_CORE_INFO("[GameplayEvents] QuestCompleted  entity={} quest='{}' branch='{}'", static_cast<u64>(e.EntityID), e.QuestID, e.BranchChoice); });

        bus.Subscribe<QuestFailedEvent>([](const QuestFailedEvent& e)
                                        { OLO_CORE_INFO("[GameplayEvents] QuestFailed     entity={} quest='{}'", static_cast<u64>(e.EntityID), e.QuestID); });

        bus.Subscribe<QuestAbandonedEvent>([](const QuestAbandonedEvent& e)
                                           { OLO_CORE_INFO("[GameplayEvents] QuestAbandoned  entity={} quest='{}'", static_cast<u64>(e.EntityID), e.QuestID); });

        // --- Inventory events -------------------------------------------------
        bus.Subscribe<ItemAddedEvent>([](const ItemAddedEvent& e)
                                      { OLO_CORE_INFO("[GameplayEvents] ItemAdded       entity={} item='{}' instance={} slot={}", static_cast<u64>(e.EntityID), e.ItemDefinitionID, static_cast<u64>(e.ItemInstanceID), e.SlotIndex); });

        bus.Subscribe<ItemRemovedEvent>([](const ItemRemovedEvent& e)
                                        { OLO_CORE_INFO("[GameplayEvents] ItemRemoved     entity={} item='{}' instance={} slot={}", static_cast<u64>(e.EntityID), e.ItemDefinitionID, static_cast<u64>(e.ItemInstanceID), e.SlotIndex); });

        bus.Subscribe<ItemEquippedEvent>([](const ItemEquippedEvent& e)
                                         { OLO_CORE_INFO("[GameplayEvents] ItemEquipped    entity={} instance={} slot='{}'", static_cast<u64>(e.EntityID), static_cast<u64>(e.ItemInstanceID), e.SlotName); });

        bus.Subscribe<ItemUnequippedEvent>([](const ItemUnequippedEvent& e)
                                           { OLO_CORE_INFO("[GameplayEvents] ItemUnequipped  entity={} instance={} slot='{}'", static_cast<u64>(e.EntityID), static_cast<u64>(e.ItemInstanceID), e.SlotName); });

        OLO_CORE_INFO("[GameplayEvents] Logger attached — quest/inventory events will stream to the Console panel.");
    }

} // namespace OloEngine
