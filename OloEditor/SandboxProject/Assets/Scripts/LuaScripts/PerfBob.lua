-- PerfBob.lua — minimal per-frame scripting workload for the scripts_swarm
-- perf stress scenes (generate_perf_scenes.py). Each tick does one
-- get_translation + set_translation round trip, so N entities measure the
-- Lua<->engine interop crossing cost, not script body cost.

local PerfBob = {}

local t = 0.0

function PerfBob.OnUpdate(id, dt)
    t = t + dt
    local pos = entity_utils.get_translation(id)
    pos.y = pos.y + math.sin(t * 2.0) * dt * 0.5
    entity_utils.set_translation(id, pos)
end

return PerfBob
