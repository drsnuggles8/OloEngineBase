-- BreakableCrateDemo.lua — issue #459 destructible demo.
--
-- Deals fatal structural damage to this entity's DestructibleComponent a moment
-- after Play starts, so DestructibleDebrisDemo.olo shatters on its own with no
-- external caller: press Play and watch it burst into debris.
--
-- Attach via LuaScriptComponent (ScriptFile =
-- "Scripts/LuaScripts/BreakableCrateDemo.lua") to an entity that has a
-- DestructibleComponent. Depleting `health` to 0 is the same trigger
-- DestructibleSystem::ApplyDamage / a combat kill / a joint break use — the
-- system shatters the object on its next tick when health reaches 0.

local BreakableCrateDemo = {}

local delay = 1.5 -- seconds after Play before the crate takes fatal damage
local timer = 0.0
local triggered = false

function BreakableCrateDemo.OnCreate(id)
    Log.Info("[BreakableCrateDemo] Crate will shatter in " .. tostring(delay) .. "s")
end

function BreakableCrateDemo.OnUpdate(id, dt)
    if triggered then return end
    timer = timer + dt
    if timer < delay then return end

    local dc = entity_utils.get_component(id, "DestructibleComponent")
    if dc then
        dc.health = 0.0 -- DestructibleSystem shatters it on the next tick
        triggered = true
        Log.Info("[BreakableCrateDemo] Structural integrity depleted — shattering.")
    end
end

return BreakableCrateDemo
