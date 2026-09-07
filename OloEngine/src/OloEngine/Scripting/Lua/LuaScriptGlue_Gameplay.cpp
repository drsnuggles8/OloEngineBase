#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_Gameplay.cpp — save games, the AI behaviour components, inventory, quests, progression, abilities and damage.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    namespace
    {
        GoapWorldState LuaTableToWorldState(const sol::table& table)
        {
            GoapWorldState ws;
            if (!table.valid())
                return ws;
            for (auto& [keyObj, valObj] : table)
            {
                if (!keyObj.is<std::string>())
                    continue;
                const std::string key = keyObj.as<std::string>();
                if (valObj.is<bool>())
                {
                    ws.Set(key, valObj.as<bool>());
                }
                else if (valObj.is<double>()) // Lua numbers; bool is handled above
                {
                    // Facts are discrete (bool|i32). Accept only integral, in-range
                    // numbers; a fractional or out-of-range value is a scripting
                    // mistake — warn and skip rather than silently truncate or wrap.
                    const double d = valObj.as<double>();
                    constexpr double kI32Min = -2147483648.0;
                    constexpr double kI32Max = 2147483647.0;
                    if (d >= kI32Min && d <= kI32Max && d == std::floor(d)) // exact integrality check
                        ws.Set(key, static_cast<i32>(d));
                    else
                        OLO_CORE_WARN("[Lua GOAP] fact '{}' = {} is not a 32-bit integer — skipped (GOAP facts must be bool or integer)", key, d);
                }
            }
            return ws;
        }

        void LuaGoapAddAction(GoapAgentComponent& comp, sol::table def)
        {
            if (!comp.RuntimeAgent)
                comp.RuntimeAgent = Ref<GoapAgent>::Create();

            GoapAction action;
            action.Name = def.get_or<std::string>("name", "");
            if (const f32 cost = def.get_or("cost", 1.0f); std::isfinite(cost) && cost >= 0.0f)
            {
                action.Cost = cost;
            }
            else
            {
                OLO_CORE_WARN("[Lua GOAP] action '{}' has invalid cost {} — using 1.0 (cost must be finite and >= 0)", action.Name, cost);
                action.Cost = 1.0f;
            }
            if (sol::optional<sol::table> pre = def["pre"]; pre)
                action.Preconditions = LuaTableToWorldState(*pre);
            if (sol::optional<sol::table> eff = def["effects"]; eff)
                action.Effects = LuaTableToWorldState(*eff);

            if (sol::optional<sol::protected_function> fn = def["perform"]; fn && fn->valid())
            {
                action.Perform = [callback = *fn](f32 dt) -> GoapActionStatus
                {
                    sol::protected_function_result result = callback(dt);
                    if (!result.valid())
                    {
                        const sol::error err = result;
                        OLO_CORE_ERROR("[Lua GOAP] action 'perform' error: {}", err.what());
                        return GoapActionStatus::Failure;
                    }
                    const sol::object out = result;
                    const sol::type t = out.get_type();
                    if (t == sol::type::lua_nil || t == sol::type::none)
                        return GoapActionStatus::Success; // no/nil return → instantaneous success
                    if (out.is<double>())
                    {
                        // Must be an exact, in-range status code — reject fractional
                        // values rather than truncating them.
                        if (const double d = out.as<double>(); d == std::floor(d) && d >= 0.0 && d <= 2.0)
                            return static_cast<GoapActionStatus>(static_cast<i32>(d));
                    }
                    OLO_CORE_ERROR("[Lua GOAP] action 'perform' returned an invalid value — expected GoapStatus.{{Running,Success,Failure}} or nil");
                    return GoapActionStatus::Failure;
                };
            }
            if (sol::optional<sol::protected_function> fn = def["onEnter"]; fn && fn->valid())
            {
                action.OnEnter = [callback = *fn]()
                {
                    sol::protected_function_result result = callback();
                    if (!result.valid())
                    {
                        const sol::error err = result;
                        OLO_CORE_ERROR("[Lua GOAP] action 'onEnter' error: {}", err.what());
                    }
                };
            }
            if (sol::optional<sol::protected_function> fn = def["isUsable"]; fn && fn->valid())
            {
                action.IsUsable = [callback = *fn]() -> bool
                {
                    sol::protected_function_result result = callback();
                    if (!result.valid())
                    {
                        const sol::error err = result;
                        OLO_CORE_ERROR("[Lua GOAP] action 'isUsable' error: {}", err.what());
                        return false;
                    }
                    const sol::object out = result;
                    if (out.is<bool>())
                        return out.as<bool>();
                    if (const sol::type t = out.get_type(); t == sol::type::lua_nil || t == sol::type::none)
                        return true; // no/nil return → usable by default
                    OLO_CORE_ERROR("[Lua GOAP] action 'isUsable' returned a non-boolean value — treating as not usable");
                    return false;
                };
            }
            comp.RuntimeAgent->AddAction(std::move(action));
        }

        void LuaGoapAddGoal(GoapAgentComponent& comp, sol::table def)
        {
            if (!comp.RuntimeAgent)
                comp.RuntimeAgent = Ref<GoapAgent>::Create();

            GoapGoal goal;
            goal.Name = def.get_or<std::string>("name", "");
            goal.Priority = def.get_or("priority", 1.0f);
            if (sol::optional<sol::table> desired = def["desired"]; desired)
                goal.DesiredState = LuaTableToWorldState(*desired);
            if (sol::optional<sol::protected_function> fn = def["isValid"]; fn && fn->valid())
            {
                goal.IsValid = [callback = *fn](const GoapWorldState& ws) -> bool
                {
                    // Hand the current world state to the script as a { key = value }
                    // table so relevance gates can read facts, e.g.
                    // isValid = function(ws) return ws.underThreat end.
                    sol::state_view lua(callback.lua_state());
                    sol::table state = lua.create_table();
                    for (const auto& fact : ws.GetFacts())
                        std::visit([&](auto&& v)
                                   { state[fact.Key] = v; }, fact.Val);

                    sol::protected_function_result result = callback(state);
                    if (!result.valid())
                    {
                        const sol::error err = result;
                        OLO_CORE_ERROR("[Lua GOAP] goal 'isValid' error: {}", err.what());
                        return false;
                    }
                    const sol::object out = result;
                    if (out.is<bool>())
                        return out.as<bool>();
                    if (const sol::type t = out.get_type(); t == sol::type::lua_nil || t == sol::type::none)
                        return true; // no/nil return → relevant by default
                    OLO_CORE_ERROR("[Lua GOAP] goal 'isValid' returned a non-boolean value — treating as not relevant");
                    return false;
                };
            }
            comp.RuntimeAgent->AddGoal(std::move(goal));
        }
    } // namespace

    void LuaScriptGlue::RegisterGameplayTypes(sol::state& lua)
    {
        // --- SaveGame ---
        auto saveGameTable = lua.create_named_table("SaveGame");
        saveGameTable["Save"] = [](const std::string& slotName, const std::string& displayName) -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::Save");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(std::to_underlying(SaveLoadResult::NoActiveScene));
            return static_cast<i32>(std::to_underlying(SaveGameManager::Save(*scene, slotName, displayName)));
        };
        saveGameTable["Load"] = [](const std::string& slotName) -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::Load");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(std::to_underlying(SaveLoadResult::NoActiveScene));
            return static_cast<i32>(std::to_underlying(SaveGameManager::Load(*scene, slotName)));
        };
        saveGameTable["QuickSave"] = []() -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::QuickSave");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(std::to_underlying(SaveLoadResult::NoActiveScene));
            return static_cast<i32>(std::to_underlying(SaveGameManager::QuickSave(*scene)));
        };
        saveGameTable["QuickLoad"] = []() -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::QuickLoad");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(std::to_underlying(SaveLoadResult::NoActiveScene));
            return static_cast<i32>(std::to_underlying(SaveGameManager::QuickLoad(*scene)));
        };
        saveGameTable["EnumerateSaves"] = [](sol::this_state s) -> sol::table
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::EnumerateSaves");
            sol::state_view luaState(s);
            auto saves = SaveGameManager::EnumerateSaves();
            sol::table result = luaState.create_table(static_cast<int>(saves.size()), 0);
            int index = 1;
            for (const auto& info : saves)
            {
                sol::table entry = luaState.create_table(0, 3);
                entry["SlotName"] = info.FilePath.stem().string();
                entry["DisplayName"] = info.Metadata.DisplayName;
                entry["TimestampUTC"] = info.Metadata.TimestampUTC;
                result[index++] = entry;
            }
            return result;
        };
        saveGameTable["DeleteSave"] = [](const std::string& slotName)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::DeleteSave");
            return SaveGameManager::DeleteSave(slotName);
        };
        saveGameTable["ValidateSave"] = [](const std::string& slotName)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::ValidateSave");
            return SaveGameManager::ValidateSave(slotName);
        };
        saveGameTable["GetAutoSaveInterval"] = []()
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::GetAutoSaveInterval");
            return SaveGameManager::GetAutoSaveInterval();
        };
        saveGameTable["SetAutoSaveInterval"] = [](f32 interval)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::SetAutoSaveInterval");
            SaveGameManager::SetAutoSaveInterval(interval);
        };

        // --- BehaviorTreeComponent ---
        OLO_PROFILE_SCOPE("Lua::RegisterAITypes");
        lua.new_usertype<BehaviorTreeComponent>("BehaviorTreeComponent", "SetBlackboardBool", [](BehaviorTreeComponent& comp, const std::string& key, bool value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardBool", [](const BehaviorTreeComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Get<bool>(key); }, "SetBlackboardInt", [](BehaviorTreeComponent& comp, const std::string& key, i32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardInt", [](const BehaviorTreeComponent& comp, const std::string& key) -> i32
                                                { return comp.Blackboard.Get<i32>(key); }, "SetBlackboardFloat", [](BehaviorTreeComponent& comp, const std::string& key, f32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardFloat", [](const BehaviorTreeComponent& comp, const std::string& key) -> f32
                                                { return comp.Blackboard.Get<f32>(key); }, "SetBlackboardString", [](BehaviorTreeComponent& comp, const std::string& key, const std::string& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardString", [](const BehaviorTreeComponent& comp, const std::string& key) -> std::string
                                                { return comp.Blackboard.Get<std::string>(key); }, "SetBlackboardVec3", [](BehaviorTreeComponent& comp, const std::string& key, const glm::vec3& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardVec3", [](const BehaviorTreeComponent& comp, const std::string& key) -> glm::vec3
                                                { return comp.Blackboard.Get<glm::vec3>(key); }, "SetBlackboardUUID", [](BehaviorTreeComponent& comp, const std::string& key, u64 value)
                                                { comp.Blackboard.Set(key, UUID(value)); }, "GetBlackboardUUID", [](const BehaviorTreeComponent& comp, const std::string& key) -> u64
                                                { return static_cast<u64>(comp.Blackboard.Get<UUID>(key)); }, "RemoveBlackboardKey", [](BehaviorTreeComponent& comp, const std::string& key)
                                                { comp.Blackboard.Remove(key); }, "HasBlackboardKey", [](const BehaviorTreeComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Has(key); }, "IsRunning", sol::readonly(&BehaviorTreeComponent::IsRunning));

        // --- StateMachineComponent ---
        lua.new_usertype<StateMachineComponent>("StateMachineComponent", "SetBlackboardBool", [](StateMachineComponent& comp, const std::string& key, bool value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardBool", [](const StateMachineComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Get<bool>(key); }, "SetBlackboardInt", [](StateMachineComponent& comp, const std::string& key, i32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardInt", [](const StateMachineComponent& comp, const std::string& key) -> i32
                                                { return comp.Blackboard.Get<i32>(key); }, "SetBlackboardFloat", [](StateMachineComponent& comp, const std::string& key, f32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardFloat", [](const StateMachineComponent& comp, const std::string& key) -> f32
                                                { return comp.Blackboard.Get<f32>(key); }, "SetBlackboardString", [](StateMachineComponent& comp, const std::string& key, const std::string& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardString", [](const StateMachineComponent& comp, const std::string& key) -> std::string
                                                { return comp.Blackboard.Get<std::string>(key); }, "SetBlackboardVec3", [](StateMachineComponent& comp, const std::string& key, const glm::vec3& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardVec3", [](const StateMachineComponent& comp, const std::string& key) -> glm::vec3
                                                { return comp.Blackboard.Get<glm::vec3>(key); }, "SetBlackboardUUID", [](StateMachineComponent& comp, const std::string& key, u64 value)
                                                { comp.Blackboard.Set(key, UUID(value)); }, "GetBlackboardUUID", [](const StateMachineComponent& comp, const std::string& key) -> u64
                                                { return static_cast<u64>(comp.Blackboard.Get<UUID>(key)); }, "RemoveBlackboardKey", [](StateMachineComponent& comp, const std::string& key)
                                                { comp.Blackboard.Remove(key); }, "HasBlackboardKey", [](const StateMachineComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Has(key); }, "GetCurrentState", [](const StateMachineComponent& comp) -> std::string
                                                {
                if (comp.RuntimeFSM && comp.RuntimeFSM->IsStarted())
                    return comp.RuntimeFSM->GetCurrentStateID();
                return ""; }, "ForceTransition", [](StateMachineComponent& comp, Entity entity, const std::string& stateId)
                                                {
                if (comp.RuntimeFSM)
                    comp.RuntimeFSM->ForceTransition(stateId, entity, comp.Blackboard); });

        // --- GoapAgentComponent ---
        // Scripts steer the GOAP brain by pushing observations into its world
        // state (SetWorldFact*) and reading back which goal/plan it committed to.
        lua.new_usertype<GoapAgentComponent>("GoapAgentComponent", "enabled", &GoapAgentComponent::Enabled, "SetBlackboardBool", [](GoapAgentComponent& comp, const std::string& key, bool value)
                                             { comp.Blackboard.Set(key, value); }, "GetBlackboardBool", [](const GoapAgentComponent& comp, const std::string& key) -> bool
                                             { return comp.Blackboard.Get<bool>(key); }, "SetBlackboardInt", [](GoapAgentComponent& comp, const std::string& key, i32 value)
                                             { comp.Blackboard.Set(key, value); }, "GetBlackboardInt", [](const GoapAgentComponent& comp, const std::string& key) -> i32
                                             { return comp.Blackboard.Get<i32>(key); }, "SetBlackboardFloat", [](GoapAgentComponent& comp, const std::string& key, f32 value)
                                             { comp.Blackboard.Set(key, value); }, "GetBlackboardFloat", [](const GoapAgentComponent& comp, const std::string& key) -> f32
                                             { return comp.Blackboard.Get<f32>(key); }, "SetWorldFactBool", [](GoapAgentComponent& comp, const std::string& key, bool value)
                                             { if (!comp.RuntimeAgent) comp.RuntimeAgent = Ref<GoapAgent>::Create(); comp.RuntimeAgent->SetFact(key, value); }, "SetWorldFactInt", [](GoapAgentComponent& comp, const std::string& key, i32 value)
                                             { if (!comp.RuntimeAgent) comp.RuntimeAgent = Ref<GoapAgent>::Create(); comp.RuntimeAgent->SetFact(key, value); }, "Invalidate", [](GoapAgentComponent& comp)
                                             { if (comp.RuntimeAgent) comp.RuntimeAgent->Invalidate(); }, "CurrentGoal", [](const GoapAgentComponent& comp) -> std::string
                                             { return comp.RuntimeAgent ? comp.RuntimeAgent->CurrentGoalName() : std::string{}; }, "HasPlan", [](const GoapAgentComponent& comp) -> bool
                                             { return comp.RuntimeAgent && comp.RuntimeAgent->HasPlan(); }, "GoalsAchieved", [](const GoapAgentComponent& comp) -> u32
                                             { return comp.RuntimeAgent ? comp.RuntimeAgent->GoalsAchieved() : 0u; },
                                             // Authoring: build the agent's brain from Lua tables.
                                             "AddAction", &LuaGoapAddAction, "AddGoal", &LuaGoapAddGoal, "ClearAgent", [](GoapAgentComponent& comp)
                                             { comp.RuntimeAgent = nullptr; });

        // Status codes a GOAP action's `perform` callback returns.
        lua["GoapStatus"] = lua.create_table_with(
            "Running", static_cast<i32>(GoapActionStatus::Running),
            "Success", static_cast<i32>(GoapActionStatus::Success),
            "Failure", static_cast<i32>(GoapActionStatus::Failure));

        // --- PerceptibleComponent ---
        // Marks the entity as something AI sight can sense. Scripts flip
        // isPerceptible for stealth or swap team at runtime.
        lua.new_usertype<PerceptibleComponent>("PerceptibleComponent",
                                               "team", &PerceptibleComponent::Team,
                                               "isPerceptible", &PerceptibleComponent::IsPerceptible);

        // --- PerceptionComponent ---
        // Authored sight config is read/write (float/vec3 setters validate
        // finiteness); the per-tick sensor result is read-only — PerceptionSystem
        // owns it. `visibleTarget` is surfaced as a u64 UUID (0 = nothing seen).
        lua.new_usertype<PerceptionComponent>("PerceptionComponent",
                                              "sightRange", sol::property([](const PerceptionComponent& c)
                                                                          { return c.SightRange; }, [](PerceptionComponent& c, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) c.SightRange = v; }),
                                              "fovDegrees", sol::property([](const PerceptionComponent& c)
                                                                          { return c.FovDegrees; }, [](PerceptionComponent& c, f32 v)
                                                                          { if (std::isfinite(v)) c.FovDegrees = glm::clamp(v, 0.0f, 360.0f); }),
                                              "eyeOffset", sol::property([](const PerceptionComponent& c)
                                                                         { return c.EyeOffset; }, [](PerceptionComponent& c, const glm::vec3& v)
                                                                         { if (IsFiniteVec3(v)) c.EyeOffset = v; }),
                                              "requireLineOfSight", &PerceptionComponent::RequireLineOfSight,
                                              "perceiverTeam", &PerceptionComponent::PerceiverTeam,
                                              "detectSameTeam", &PerceptionComponent::DetectSameTeam,
                                              "hasVisibleTarget", sol::readonly(&PerceptionComponent::HasVisibleTarget),
                                              "visibleTarget", sol::property([](const PerceptionComponent& c)
                                                                             { return static_cast<u64>(c.VisibleTarget); }),
                                              "lastKnownPosition", sol::readonly(&PerceptionComponent::LastKnownPosition),
                                              "hasLastKnownPosition", sol::readonly(&PerceptionComponent::HasLastKnownPosition),
                                              "timeSinceLastSeen", sol::readonly(&PerceptionComponent::TimeSinceLastSeen));

        // --- InventoryComponent ---
        lua.new_usertype<InventoryComponent>("InventoryComponent", "currency", &InventoryComponent::Currency, "AddItem", [](InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                             {
                                                 i32 total = count.value_or(1);
                                                 if (total <= 0)
                                                     return false;
                                                 const auto* def = ItemDatabase::Get(itemId);
                                                 if (!def)
                                                     return false;
                                                 i32 maxStack = std::max(def->MaxStackSize, 1);
                                                 // Route through InventorySystem so each add publishes ItemAdded.
                                                 auto [scene, entity] = LuaOwnerContext(comp);
                                                 i32 remaining = total;
                                                 while (remaining > 0)
                                                 {
                                                     ItemInstance instance;
                                                     instance.InstanceID = UUID();
                                                     instance.ItemDefinitionID = itemId;
                                                     instance.StackCount = std::min(remaining, maxStack);
                                                     bool added = entity ? InventorySystem::AddItem(scene, entity, instance)
                                                                         : comp.PlayerInventory.AddItem(instance);
                                                     if (!added)
                                                         return false;
                                                     remaining -= instance.StackCount;
                                                 }
                                                 return true; }, "RemoveItem", [](InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                             { auto [scene, entity] = LuaOwnerContext(comp); i32 cnt = count.value_or(1); return entity ? InventorySystem::RemoveItemByDefinition(scene, entity, itemId, cnt) : comp.PlayerInventory.RemoveItemByDefinition(itemId, cnt); }, "HasItem", [](const InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count) -> bool
                                             { return comp.PlayerInventory.HasItem(itemId, count.value_or(1)); }, "CountItem", [](const InventoryComponent& comp, const std::string& itemId) -> i32
                                             { return comp.PlayerInventory.CountItem(itemId); }, "GetUsedSlots", [](const InventoryComponent& comp) -> i32
                                             { return comp.PlayerInventory.GetUsedSlots(); }, "GetCapacity", [](const InventoryComponent& comp) -> i32
                                             { return comp.PlayerInventory.GetCapacity(); }, "GetTotalWeight", [](const InventoryComponent& comp) -> f32
                                             { return comp.PlayerInventory.GetTotalWeight(); }, "EquipItem", [](InventoryComponent& comp, const std::string& definitionId, const std::string& slotName) -> bool
                                             {
                                                 auto [scene, entity] = LuaOwnerContext(comp);
                                                 if (!entity)
                                                     return false;
                                                 EquipmentSlots::Slot slot = EquipmentSlots::SlotFromString(slotName);
                                                 if (slot == EquipmentSlots::Slot::Count)
                                                     return false;
                                                 // Find the item in the inventory by definition; copy it (Equip
                                                 // removes it from the inventory, invalidating the slot pointer).
                                                 i32 idx = comp.PlayerInventory.FindItem(definitionId);
                                                 const ItemInstance* found = comp.PlayerInventory.GetItemAtSlot(idx);
                                                 if (!found)
                                                     return false;
                                                 ItemInstance copy = *found;
                                                 return InventorySystem::EquipItem(scene, entity, slot, copy); }, "UnequipItem", [](InventoryComponent& comp, const std::string& slotName) -> bool
                                             {
                                                 auto [scene, entity] = LuaOwnerContext(comp);
                                                 if (!entity)
                                                     return false;
                                                 EquipmentSlots::Slot slot = EquipmentSlots::SlotFromString(slotName);
                                                 if (slot == EquipmentSlots::Slot::Count)
                                                     return false;
                                                 return InventorySystem::UnequipItem(scene, entity, slot); });

        // --- ItemPickupComponent ---
        lua.new_usertype<ItemPickupComponent>("ItemPickupComponent",
                                              "pickupRadius", sol::property([](const ItemPickupComponent& c)
                                                                            { return c.PickupRadius; }, [](ItemPickupComponent& c, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f) c.PickupRadius = v; }),
                                              "autoPickup", &ItemPickupComponent::AutoPickup,
                                              "despawnTimer", sol::property([](const ItemPickupComponent& c)
                                                                            { return c.DespawnTimer; }, [](ItemPickupComponent& c, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f) c.DespawnTimer = v; }));

        // --- ItemContainerComponent ---
        lua.new_usertype<ItemContainerComponent>("ItemContainerComponent",
                                                 "isShop", &ItemContainerComponent::IsShop,
                                                 "lootTableID", &ItemContainerComponent::LootTableID,
                                                 "hasBeenLooted", &ItemContainerComponent::HasBeenLooted);

        // --- QuestJournalComponent ---
        lua.new_usertype<QuestJournalComponent>("QuestJournalComponent", "AcceptQuest", [](QuestJournalComponent& comp, const std::string& questId) -> bool
                                                {
                auto [scene, entity] = LuaOwnerContext(comp);
                if (entity)
                    return QuestSystem::AcceptQuest(scene, entity, questId);
                const auto* def = QuestDatabase::Get(questId);
                return def && comp.Journal.AcceptQuest(questId, *def); }, "AbandonQuest", [](QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { auto [scene, entity] = LuaOwnerContext(comp); return entity ? QuestSystem::AbandonQuest(scene, entity, questId) : comp.Journal.AbandonQuest(questId); }, "CompleteQuest", [](QuestJournalComponent& comp, const std::string& questId, sol::optional<std::string> branch) -> bool
                                                { auto [scene, entity] = LuaOwnerContext(comp); return entity ? QuestSystem::CompleteQuest(scene, entity, questId, branch.value_or("")) : comp.Journal.CompleteQuest(questId, branch.value_or("")).has_value(); }, "IsQuestActive", [](const QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.IsQuestActive(questId); }, "HasCompletedQuest", [](const QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.HasCompletedQuest(questId); }, "IncrementObjective", [](QuestJournalComponent& comp, const std::string& questId, const std::string& objId, sol::optional<i32> amount)
                                                { i32 amt = amount.value_or(1); if (amt <= 0) return; auto [scene, entity] = LuaOwnerContext(comp); if (entity) QuestSystem::IncrementObjective(scene, entity, questId, objId, amt); else comp.Journal.IncrementObjective(questId, objId, amt); }, "NotifyKill", [](QuestJournalComponent& comp, const std::string& targetTag)
                                                { auto [scene, entity] = LuaOwnerContext(comp); if (entity) QuestSystem::NotifyKill(scene, entity, targetTag); else comp.Journal.NotifyKill(targetTag); }, "NotifyCollect", [](QuestJournalComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                                { i32 cnt = count.value_or(1); if (cnt <= 0) return; auto [scene, entity] = LuaOwnerContext(comp); if (entity) QuestSystem::NotifyCollect(scene, entity, itemId, cnt); else comp.Journal.NotifyCollect(itemId, cnt); }, "NotifyInteract", [](QuestJournalComponent& comp, const std::string& id)
                                                { auto [scene, entity] = LuaOwnerContext(comp); if (entity) QuestSystem::NotifyInteract(scene, entity, id); else comp.Journal.NotifyInteract(id); }, "NotifyReachLocation", [](QuestJournalComponent& comp, const std::string& locId)
                                                { auto [scene, entity] = LuaOwnerContext(comp); if (entity) QuestSystem::NotifyReachLocation(scene, entity, locId); else comp.Journal.NotifyReachLocation(locId); }, "HasTag", [](const QuestJournalComponent& comp, const std::string& tag) -> bool
                                                { return comp.Journal.HasTag(tag); }, "AddTag", [](QuestJournalComponent& comp, const std::string& tag)
                                                { comp.Journal.AddTag(tag); }, "SetPlayerLevel", [](QuestJournalComponent& comp, i32 level)
                                                { if (level < 0) return; comp.Journal.SetPlayerLevel(level); }, "GetPlayerLevel", [](const QuestJournalComponent& comp) -> i32
                                                { return comp.Journal.GetPlayerLevel(); }, "SetReputation", [](QuestJournalComponent& comp, const std::string& factionId, i32 value)
                                                { comp.Journal.SetReputation(factionId, value); }, "GetReputation", [](const QuestJournalComponent& comp, const std::string& factionId) -> i32
                                                { return comp.Journal.GetReputation(factionId); }, "SetItemCount", [](QuestJournalComponent& comp, const std::string& itemId, i32 count)
                                                { if (count < 0) return; comp.Journal.SetItemCount(itemId, count); }, "GetItemCount", [](const QuestJournalComponent& comp, const std::string& itemId) -> i32
                                                { return comp.Journal.GetItemCount(itemId); }, "SetStat", [](QuestJournalComponent& comp, const std::string& statName, i32 value)
                                                { comp.Journal.SetStat(statName, value); }, "GetStat", [](const QuestJournalComponent& comp, const std::string& statName) -> i32
                                                { return comp.Journal.GetStat(statName); }, "SetPlayerClass", [](QuestJournalComponent& comp, const std::string& className)
                                                { comp.Journal.SetPlayerClass(className); }, "GetPlayerClass", [](const QuestJournalComponent& comp) -> std::string
                                                { return comp.Journal.GetPlayerClass(); }, "SetPlayerFaction", [](QuestJournalComponent& comp, const std::string& factionName)
                                                { comp.Journal.SetPlayerFaction(factionName); }, "GetPlayerFaction", [](const QuestJournalComponent& comp) -> std::string
                                                { return comp.Journal.GetPlayerFaction(); });

        // --- QuestGiverComponent ---
        lua.new_usertype<QuestGiverComponent>("QuestGiverComponent",
                                              "questMarkerIcon", &QuestGiverComponent::QuestMarkerIcon,
                                              "offeredQuestIDs", &QuestGiverComponent::OfferedQuestIDs,
                                              "turnInQuestIDs", &QuestGiverComponent::TurnInQuestIDs);

        // --- ProgressionComponent ---
        // Mutations route through ProgressionSystem when an owning entity can
        // be resolved (entity-stamped gameplay events); without a scene
        // context they fall back to the raw component (or a safe default) so
        // the bindings stay usable in headless tests.
        lua.new_usertype<ProgressionComponent>("ProgressionComponent", "GrantExperience", [](ProgressionComponent& comp, i32 amount)
                                               {
                auto [scene, entity] = LuaOwnerContext(comp);
                if (entity)
                    ProgressionSystem::GrantExperience(scene, entity, amount);
                else
                    comp.AddPendingXP(amount); }, "GetLevel", [](const ProgressionComponent& comp) -> i32
                                               { return comp.Level; }, "GetXP", [](const ProgressionComponent& comp) -> i32
                                               { return comp.CurrentXP; }, "GetPendingXP", [](const ProgressionComponent& comp) -> i32
                                               { return comp.PendingXP; }, "GetXPToNextLevel", [](ProgressionComponent& comp) -> i32
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::GetXPToNextLevel(scene, entity) : std::max(ExperienceCurve::DefaultXPForLevelUp(comp.Level) - comp.CurrentXP, 0); }, "GetMaxLevel", [](ProgressionComponent& comp) -> i32
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::GetMaxLevel(scene, entity) : ExperienceCurve::kDefaultMaxLevel; }, "GetAttributePoints", [](const ProgressionComponent& comp) -> i32
                                               { return comp.AttributePoints; }, "GetSkillPoints", [](const ProgressionComponent& comp) -> i32
                                               { return comp.SkillPoints; }, "SpendAttributePoint", [](ProgressionComponent& comp, const std::string& attribute, sol::optional<i32> count) -> bool
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::SpendAttributePoint(scene, entity, attribute, count.value_or(1)) : false; }, "RefundAttributePoint", [](ProgressionComponent& comp, const std::string& attribute, sol::optional<i32> count) -> bool
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::RefundAttributePoint(scene, entity, attribute, count.value_or(1)) : false; }, "Respec", [](ProgressionComponent& comp) -> i32
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::RespecAttributes(scene, entity) : 0; }, "UnlockSkillNode", [](ProgressionComponent& comp, const std::string& nodeId) -> bool
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::UnlockSkillNode(scene, entity, nodeId) : false; }, "RefundSkillNode", [](ProgressionComponent& comp, const std::string& nodeId) -> bool
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::RefundSkillNode(scene, entity, nodeId) : false; }, "RespecSkills", [](ProgressionComponent& comp) -> i32
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::RespecSkills(scene, entity) : 0; }, "CanUnlockSkillNode", [](ProgressionComponent& comp, const std::string& nodeId) -> bool
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::CanUnlockSkillNode(scene, entity, nodeId) : false; }, "IsNodeUnlocked", [](const ProgressionComponent& comp, const std::string& nodeId) -> bool
                                               { return comp.UnlockedNodes.contains(nodeId); }, "InitializeFromClass", [](ProgressionComponent& comp, sol::optional<std::string> classId) -> bool
                                               { auto [scene, entity] = LuaOwnerContext(comp); return entity ? ProgressionSystem::InitializeFromClass(scene, entity, classId.value_or("")) : false; }, "GetClassID", [](const ProgressionComponent& comp) -> std::string
                                               { return comp.ClassID; }, "xpBounty", &ProgressionComponent::XPBounty, "healOnLevelUp", &ProgressionComponent::HealOnLevelUp);

        // --- Log (global table) ---
        auto logTable = lua.create_named_table("Log");
        logTable["Trace"] = [](const std::string& msg)
        { OLO_TRACE("{}", msg); };
        logTable["Info"] = [](const std::string& msg)
        { OLO_INFO("{}", msg); };
        logTable["Warn"] = [](const std::string& msg)
        { OLO_WARN("{}", msg); };
        logTable["Error"] = [](const std::string& msg)
        { OLO_ERROR("{}", msg); };

        // --- Entity utilities ---
        auto entityUtilsTable = lua.create_named_table("entity_utils");
        // Overloaded so scripts can pass either the Entity userdata or the raw
        // u64 id they actually receive in OnCreate/OnUpdate. Both report the
        // script-facing (logical) liveness: an id from a same-tick
        // Scene.Instantiate is already valid even though the entity
        // materialises at the next drain, and an id already passed to
        // entity_utils.destroy is already invalid (issue #643).
        entityUtilsTable["is_valid"] = sol::overload(
            [](Entity* entity) -> bool
            {
                if (!entity)
                    return false;
                const Scene* scene = ScriptEngine::GetSceneContext();
                return scene && scene->IsEntityLiveForScripts(entity->GetUUID());
            },
            [](u64 entityID) -> bool
            {
                const Scene* scene = ScriptEngine::GetSceneContext();
                return scene && scene->IsEntityLiveForScripts(UUID(entityID));
            });

        // Deferred destroy — the entity (and its children) goes away once the
        // engine drains its command queue, after every script's OnUpdate has
        // returned this tick. Safe to call on the entity the script is running
        // on, and safe to call twice. Mirrors C#'s entity.Destroy().
        entityUtilsTable["destroy"] = [](u64 entityID)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
            {
                OLO_CORE_WARN("[Lua] entity_utils.destroy called with no active scene context.");
                return;
            }
            scene->ScriptDestroyEntity(UUID(entityID));
        };

        // --- Physics (raycast) ---
        auto physicsTable = lua.create_named_table("Physics");
        physicsTable["Raycast"] = [](const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance, sol::this_state s) -> sol::object
        {
            const Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);

            JoltScene* joltScene = scene->GetPhysicsScene();
            if (!joltScene)
                return sol::make_object(s, sol::nil);

            RayCastInfo rayInfo(origin, direction, maxDistance);
            SceneQueryHit hit;
            if (!joltScene->CastRay(rayInfo, hit))
                return sol::make_object(s, sol::nil);

            sol::state_view lua(s);
            sol::table result = lua.create_table(0, 4);
            result["position"] = hit.m_Position;
            result["normal"] = hit.m_Normal;
            result["distance"] = hit.m_Distance;
            result["entityID"] = static_cast<u64>(hit.m_HitEntity);
            return result;
        };

        // --- Camera (screen-to-world) ---
        auto cameraTable = lua.create_named_table("Camera");
        cameraTable["ScreenToWorldRay"] = [](u64 cameraEntityID, const glm::vec2& screenPos, sol::this_state s) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);

            auto cameraOpt = scene->TryGetEntityWithUUID(UUID(cameraEntityID));
            if (!cameraOpt)
                return sol::make_object(s, sol::nil);
            Entity cameraEntity{ static_cast<entt::entity>(*cameraOpt), scene };
            if (!cameraEntity.HasComponent<CameraComponent>() || !cameraEntity.HasComponent<TransformComponent>())
                return sol::make_object(s, sol::nil);

            auto const& cameraComp = cameraEntity.GetComponent<CameraComponent>();
            auto const& transform = cameraEntity.GetComponent<TransformComponent>();

            glm::mat4 viewMatrix = glm::inverse(transform.GetTransform());
            glm::mat4 projMatrix = cameraComp.Camera.GetProjection();
            glm::mat4 invVP = glm::inverse(projMatrix * viewMatrix);

            // screenPos is in pixels (from Input.GetMousePosition); normalise to [0,1]
            const auto& window = Application::Get().GetWindow();
            const f32 winW = static_cast<f32>(window.GetWidth());
            const f32 winH = static_cast<f32>(window.GetHeight());
            if (winW <= 0.0f || winH <= 0.0f)
                return sol::make_object(s, sol::nil);

            const f32 normX = screenPos.x / winW;
            const f32 normY = screenPos.y / winH;

            f32 ndcX = normX * 2.0f - 1.0f;
            f32 ndcY = 1.0f - normY * 2.0f; // Flip Y: screen top-left origin → NDC bottom-left origin

            glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
            glm::vec4 farPoint = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
            nearPoint /= nearPoint.w;
            farPoint /= farPoint.w;

            sol::state_view lua(s);
            sol::table result = lua.create_table(0, 2);
            result["origin"] = glm::vec3(nearPoint);
            result["direction"] = glm::normalize(glm::vec3(farPoint - nearPoint));
            return result;
        };

        // --- AbilityComponent ---
        lua.new_usertype<AbilityComponent>("AbilityComponent", "GetAttribute", [](const AbilityComponent& comp, const std::string& name) -> f32
                                           { return comp.Attributes.GetBaseValue(name); }, "SetAttribute", [](AbilityComponent& comp, const std::string& name, f32 value)
                                           { comp.Attributes.SetBaseValue(name, value); }, "GetCurrentAttribute", [](const AbilityComponent& comp, const std::string& name) -> f32
                                           { return comp.Attributes.GetCurrentValue(name); }, "HasTag", [](const AbilityComponent& comp, const std::string& tag) -> bool
                                           { return comp.OwnedTags.HasTagExact(GameplayTag(tag)); }, "AddTag", [](AbilityComponent& comp, const std::string& tag)
                                           { comp.OwnedTags.AddTag(GameplayTag(tag)); }, "RemoveTag", [](AbilityComponent& comp, const std::string& tag)
                                           { comp.OwnedTags.RemoveTag(GameplayTag(tag)); }, "DefineAttribute", [](AbilityComponent& comp, const std::string& name, f32 baseValue)
                                           { comp.Attributes.DefineAttribute(name, baseValue); }, "InitDefaultRPG", [](AbilityComponent& comp, f32 maxHP, f32 maxMana, f32 atk, f32 def)
                                           { comp.InitializeDefaultRPGAttributes(maxHP, maxMana, atk, def); });

        // --- Damage routing (cross-entity, uses scene context) ---
        auto damageTable = lua.create_named_table("Damage");
        damageTable["ApplyToTarget"] = [](u64 sourceID, u64 targetID, f32 rawDamage, sol::optional<std::string> damageType, sol::optional<bool> isCritical) -> f32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return 0.0f;

            auto sourceOpt = scene->TryGetEntityWithUUID(UUID(sourceID));
            auto targetOpt = scene->TryGetEntityWithUUID(UUID(targetID));
            if (!sourceOpt || !targetOpt)
                return 0.0f;
            Entity source{ static_cast<entt::entity>(*sourceOpt), scene };
            Entity target{ static_cast<entt::entity>(*targetOpt), scene };

            if (!source.HasComponent<AbilityComponent>() || !target.HasComponent<AbilityComponent>())
                return 0.0f;

            auto const& sourceAC = source.GetComponent<AbilityComponent>();

            DamageEvent event;
            event.Source = source;
            event.Target = target;
            event.RawDamage = rawDamage;
            event.IsCritical = isCritical.value_or(false);
            event.CritMultiplier = sourceAC.Attributes.GetCurrentValue("CritMultiplier");
            const std::string dt = damageType.value_or("Physical");
            event.DamageType = GameplayTag(dt);

            // Single damage choke point: calculation, Health base-value write,
            // death-tag flip, EntityKilledEvent, and the kill-XP bounty (issue
            // #635) all live in GameplayAbilitySystem::ApplyDamage.
            return GameplayAbilitySystem::ApplyDamage(scene, event);
        };

        damageTable["TryActivateAbility"] = [](u64 casterID, const std::string& abilityTag) -> bool
        {
            if (abilityTag.empty())
                return false;

            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;

            auto casterOpt = scene->TryGetEntityWithUUID(UUID(casterID));
            if (!casterOpt)
                return false;
            Entity caster{ static_cast<entt::entity>(*casterOpt), scene };

            if (!caster.HasComponent<AbilityComponent>())
                return false;

            return GameplayAbilitySystem::TryActivateAbility(scene, caster, GameplayTag(abilityTag));
        };

        damageTable["TryActivateAbilityOnTarget"] = [](u64 casterID, const std::string& abilityTag, u64 targetID) -> bool
        {
            if (abilityTag.empty())
                return false;

            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;

            auto casterOpt = scene->TryGetEntityWithUUID(UUID(casterID));
            auto targetOpt = scene->TryGetEntityWithUUID(UUID(targetID));
            if (!casterOpt || !targetOpt)
                return false;
            Entity caster{ static_cast<entt::entity>(*casterOpt), scene };
            Entity target{ static_cast<entt::entity>(*targetOpt), scene };

            if (!caster.HasComponent<AbilityComponent>() || !target.HasComponent<AbilityComponent>())
                return false;

            GameplayTag tag(abilityTag);
            // Activate on the caster (checks cooldowns, costs, tags).
            // TryActivateAbility also applies ActivationEffects to the caster;
            // for targeted abilities we additionally redirect effects to the
            // target below (intentional self+target duplication — see C# mirror).
            if (!GameplayAbilitySystem::TryActivateAbility(scene, caster, tag))
                return false;

            const auto& casterAC = caster.GetComponent<AbilityComponent>();
            for (const auto& ability : casterAC.Abilities)
            {
                if (ability.Definition.AbilityTag == tag)
                {
                    if (!ability.Definition.TargetActivationEffects.empty())
                    {
                        auto& targetAC = target.GetComponent<AbilityComponent>();
                        for (auto const& effect : ability.Definition.TargetActivationEffects)
                        {
                            targetAC.ActiveEffects.ApplyEffect(effect, targetAC.OwnedTags, tag);
                        }
                    }
                    break;
                }
            }

            return true;
        };
    }
} // namespace OloEngine
