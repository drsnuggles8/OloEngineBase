-- GameplayEventsDemo.lua
-- Drives every Quest & Inventory gameplay event so you can watch them stream
-- into the editor Console panel. Filter the Console by "[GameplayEvents]".
--
-- Attach (via LuaScriptComponent) to an entity that has BOTH a
-- QuestJournalComponent and an InventoryComponent. Enter Play, click the
-- viewport so it has focus, then press:
--
--   QUESTS
--     1  Accept "wolf_hunt"               -> QuestStarted
--     2  NotifyKill "wolf"                -> ObjectiveProgress
--                                            (3rd press also: ObjectiveCompleted + QuestStageAdvanced)
--     3  NotifyInteract "village_elder"   -> ObjectiveCompleted + QuestCompleted
--     4  Abandon "wolf_hunt"              -> QuestAbandoned
--     5  Accept "demo_timed"              -> QuestStarted
--                                            (do nothing ~8s -> QuestFailed via the timer path)
--     6  Accept "herb_gathering"          -> QuestStarted
--     7  NotifyCollect "healing_herb" x5  -> ObjectiveProgress... + ObjectiveCompleted + QuestCompleted
--
--   INVENTORY
--     I  AddItem "health_potion"          -> ItemAdded
--     K  RemoveItem "health_potion"       -> ItemRemoved
--     O  AddItem + Equip "iron_sword"     -> ItemAdded + ItemEquipped (MainHand)
--     P  Unequip MainHand                 -> ItemUnequipped
--
-- A nearby auto-pickup also fires ItemAdded the instant you press Play.

local M = {}

local function quest(id)
    return entity_utils.get_component(id, "QuestJournalComponent")
end

local function inv(id)
    return entity_utils.get_component(id, "InventoryComponent")
end

function M.OnCreate(id)
    Log.Info("[GameplayEventsDemo] Ready. Keys 1-7 = quests, I/K/O/P = inventory. Watch the Console for [GameplayEvents].")
end

function M.OnUpdate(id, dt)
    -- ---- Quests ----
    if Input.IsKeyJustPressed(KeyCode.D1) then
        local q = quest(id); if q then q:AcceptQuest("wolf_hunt") end
    elseif Input.IsKeyJustPressed(KeyCode.D2) then
        local q = quest(id); if q then q:NotifyKill("wolf") end
    elseif Input.IsKeyJustPressed(KeyCode.D3) then
        local q = quest(id); if q then q:NotifyInteract("village_elder") end
    elseif Input.IsKeyJustPressed(KeyCode.D4) then
        local q = quest(id); if q then q:AbandonQuest("wolf_hunt") end
    elseif Input.IsKeyJustPressed(KeyCode.D5) then
        local q = quest(id); if q then q:AcceptQuest("demo_timed") end
    elseif Input.IsKeyJustPressed(KeyCode.D6) then
        local q = quest(id); if q then q:AcceptQuest("herb_gathering") end
    elseif Input.IsKeyJustPressed(KeyCode.D7) then
        local q = quest(id); if q then q:NotifyCollect("healing_herb", 5) end
    -- ---- Inventory ----
    elseif Input.IsKeyJustPressed(KeyCode.I) then
        local i = inv(id); if i then i:AddItem("health_potion", 1) end
    elseif Input.IsKeyJustPressed(KeyCode.K) then
        local i = inv(id); if i then i:RemoveItem("health_potion", 1) end
    elseif Input.IsKeyJustPressed(KeyCode.O) then
        local i = inv(id)
        if i then
            i:AddItem("iron_sword", 1)
            i:EquipItem("iron_sword", "MainHand")
        end
    elseif Input.IsKeyJustPressed(KeyCode.P) then
        local i = inv(id); if i then i:UnequipItem("MainHand") end
    end
end

function M.OnDestroy(id)
end

return M
