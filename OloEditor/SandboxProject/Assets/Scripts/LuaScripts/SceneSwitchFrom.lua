-- SceneSwitchFrom.lua — issue #642 live verification.
-- Requests a hard-cut switch to SceneSwitchB one second after Play starts.
local M = {}
M.elapsed = 0.0
M.done = false

function M.OnCreate(entityID)
    Log.Info("[SceneSwitch] Scene A live — switching to SceneSwitchB in 1s")
end

function M.OnUpdate(entityID, ts)
    if M.done then return end
    M.elapsed = M.elapsed + ts
    if M.elapsed >= 1.0 then
        M.done = true
        Log.Info("[SceneSwitch] Requesting Scene.LoadScene('SceneSwitchB')")
        Scene.LoadScene("SceneSwitchB")
    end
end

return M
