-- LuaGoapHungryNPC.lua — a GOAP-driven NPC authored entirely from Lua.
--
-- Attach via LuaScriptComponent (ScriptFile = "Scripts/LuaScripts/LuaGoapHungryNPC.lua")
-- to an entity that ALSO has a GoapAgentComponent. The scene needs an entity
-- named "Food" for the NPC to walk to. Movement happens on the 3D ground
-- (the X/Z plane); Y is left untouched so the mesh stays on the floor.
--
-- Behaviour: the NPC gets hungry on a timer. The GOAP planner then chains
-- GoToFood -> Eat to satisfy the "NotHungry" goal. Eating teleports the food
-- to the next spot, so the NPC keeps re-planning a fresh path. Press Play and
-- watch the green cube chase the yellow sphere around the arena.
--
-- This file is the reference for how to define a GOAP brain from Lua:
--   goap:AddAction{ name=, cost=, pre={...}, effects={...}, perform=function(dt) ... end }
--   goap:AddGoal{ name=, priority=, desired={...} }
-- where `pre`/`effects`/`desired` are fact tables (string -> bool|integer) and
-- a `perform` callback returns GoapStatus.Running / .Success / .Failure
-- (or nothing, meaning instantaneous success).

local HungryNPC = {}

local EAT_RANGE = 1.1      -- world units (X/Z); how close counts as "at the food"
local MOVE_SPEED = 3.5     -- world units / second
local HUNGER_PERIOD = 2.5  -- seconds between getting hungry

-- Per-entity runtime state (keyed by entity id so one script table can drive
-- several NPCs).
local state = {}

-- The food cycles through these (X, Z) spots each time it is eaten.
local FOOD_SPOTS = {
    { x = 5.0, z = 3.0 },
    { x = -5.0, z = 2.0 },
    { x = 4.0, z = -4.0 },
    { x = -4.0, z = -4.0 },
}

-- Horizontal (X/Z) distance from the NPC to the food entity, plus the food id.
local function distanceToFood(npcID)
    local foodID = entity_utils.find_by_name("Food")
    if not foodID then return nil, nil end
    foodID = math.tointeger(foodID) or 0
    local me = entity_utils.get_translation(npcID)
    local food = entity_utils.get_translation(foodID)
    if not me or not food then return nil, foodID end
    local dx = food.x - me.x
    local dz = food.z - me.z
    return math.sqrt(dx * dx + dz * dz), foodID
end

function HungryNPC.OnCreate(id)
    state[id] = { hungerTimer = 0.0, foodIndex = 0 }

    local goap = entity_utils.get_component(id, "GoapAgentComponent")
    if not goap then
        Log.Warn("[GoapHungryNPC] entity has no GoapAgentComponent — nothing to drive")
        return
    end

    -- Walk toward the food (on the X/Z plane) until within EAT_RANGE.
    goap:AddAction{
        name = "GoToFood",
        cost = 1.0,
        pre = { nearFood = false },
        effects = { nearFood = true },
        perform = function(dt)
            local dist, foodID = distanceToFood(id)
            if not dist then return GoapStatus.Failure end
            if dist <= EAT_RANGE then return GoapStatus.Success end

            local me = entity_utils.get_translation(id)
            local food = entity_utils.get_translation(foodID)
            local step = math.min(MOVE_SPEED * dt, dist) -- never overshoot on a long frame
            local nx = me.x + (food.x - me.x) / dist * step
            local nz = me.z + (food.z - me.z) / dist * step
            entity_utils.set_translation(id, vec3.new(nx, me.y, nz)) -- keep Y on the floor
            if step >= dist then return GoapStatus.Success end -- reached the food this tick
            return GoapStatus.Running
        end,
    }

    -- Eat: instantaneous. Clears hunger and moves the food to the next spot so
    -- the NPC has to travel again next time it gets hungry.
    goap:AddAction{
        name = "Eat",
        cost = 1.0,
        pre = { nearFood = true },
        effects = { hungry = false, nearFood = false },
        perform = function(dt)
            local s = state[id]
            s.foodIndex = (s.foodIndex % #FOOD_SPOTS) + 1
            local spot = FOOD_SPOTS[s.foodIndex]
            local foodID = entity_utils.find_by_name("Food")
            if foodID then
                foodID = math.tointeger(foodID) or 0
                local f = entity_utils.get_translation(foodID)
                entity_utils.set_translation(foodID, vec3.new(spot.x, f.y, spot.z))
            end
            Log.Info("[GoapHungryNPC] nom — fed, food relocated")
            return GoapStatus.Success
        end,
    }

    goap:AddGoal{
        name = "NotHungry",
        priority = 1.0,
        desired = { hungry = false },
    }

    -- Seed the world state. Start hungry so it does something immediately.
    goap:SetWorldFactBool("hungry", true)
    goap:SetWorldFactBool("nearFood", false)

    Log.Info("[GoapHungryNPC] OnCreate — GOAP brain built (2 actions, 1 goal)")
end

function HungryNPC.OnUpdate(id, dt)
    local goap = entity_utils.get_component(id, "GoapAgentComponent")
    if not goap then return end

    local s = state[id]
    if not s then return end

    -- Sensor: refresh world facts BEFORE the AI system ticks the agent this frame.
    local dist = distanceToFood(id)
    if dist then
        goap:SetWorldFactBool("nearFood", dist <= EAT_RANGE)
    end

    -- Hunger timer.
    s.hungerTimer = s.hungerTimer + dt
    if s.hungerTimer >= HUNGER_PERIOD then
        s.hungerTimer = 0.0
        goap:SetWorldFactBool("hungry", true)
        goap:Invalidate() -- a new need appeared; force a fresh plan
    end
end

function HungryNPC.OnDestroy(id)
    -- Release the Lua callbacks captured by the agent before the Lua state is
    -- torn down.
    local goap = entity_utils.get_component(id, "GoapAgentComponent")
    if goap then goap:ClearAgent() end
    state[id] = nil
end

return HungryNPC
