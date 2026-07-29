-- SceneSwitchTo.lua — issue #642 live verification.
-- Lives in the scene we switch INTO. Its OnCreate/OnUpdate only run if the host
-- re-initialized scripting for the incoming scene.
local M = {}
M.logged = false

function M.OnCreate(entityID)
    Log.Info("[SceneSwitch] Scene B OnCreate fired — scripting came up for the incoming scene")
end

function M.OnUpdate(entityID, ts)
    if M.logged then return end
    M.logged = true
    Log.Info("[SceneSwitch] Scene B OnUpdate running — the switched-to scene is ticking")
end

return M
