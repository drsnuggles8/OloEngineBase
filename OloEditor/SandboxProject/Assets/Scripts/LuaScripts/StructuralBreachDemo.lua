-- StructuralBreachDemo.lua — issue #786 progressive-collapse demo.
--
-- Blows this block out of the wall a couple of seconds after Play, so
-- StructuralCollapseDemo.olo breaches itself with no external caller: press Play
-- and watch the wall come down.
--
-- Attached to the five middle blocks of the wall's bottom course. Depleting
-- `health` is the same trigger DestructibleSystem::ApplyDamage / a combat kill /
-- a joint break use: the block shatters into debris on the next tick, and
-- because it is also a StructuralNodeComponent, its removal re-runs the support
-- solver over the wall. Everything that can no longer reach an anchor within its
-- MaxLateralSpan detaches, falls, and shatters where it lands.
--
-- Nothing here knows about the collapse — that is the point. The breach uses the
-- #459 damage seam and the structural system reacts to it.

local StructuralBreachDemo = {}

local delay = 2.0 -- seconds after Play before this block blows out
local timer = 0.0
local triggered = false

function StructuralBreachDemo.OnCreate(id)
    Log.Info("[StructuralBreachDemo] Breach block armed; blowing out in " .. tostring(delay) .. "s")
end

function StructuralBreachDemo.OnUpdate(id, dt)
    if triggered then return end
    timer = timer + dt
    if timer < delay then return end

    local dc = entity_utils.get_component(id, "DestructibleComponent")
    if dc then
        dc.health = 0.0 -- DestructibleSystem shatters it on the next tick
        triggered = true
        Log.Info("[StructuralBreachDemo] Base block blown out — the wall loses an anchor.")
    end
end

return StructuralBreachDemo
