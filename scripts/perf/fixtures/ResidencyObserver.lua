-- Uses production Lua APIs. No io/os, private userdata, renderer mutation or fake counters.
local cell = "authored-forward"
local packedHandleFromScene = "15952688685437936336"
local elapsed = 0
local fired = {}
local reloadPass = 0

local function marker(kind)
    local coat = entity_utils.find_by_name("PackedCoat")
    local plants = entity_utils.find_by_name("StagedPlants")
    assert(coat == 1257001 and plants == 1257002, "Runtime fixture identities changed")
    local b = Scene.GetRepresentationStreamingBudgets()
    assert(b ~= nil, "Missing active scene")
    Log.Info("[ResidencyRuntime] " .. kind .. " cell=" .. cell ..
             " pass=" .. tostring(reloadPass) .. " simulationSeconds=" .. tostring(elapsed) ..
             " groomUuid=" .. tostring(coat) .. " plantUuid=" .. tostring(plants) ..
             " groomHandleFromLooseScene=" .. packedHandleFromScene ..
             " residentMiB=" .. tostring(b.residentMegabytes) ..
             " uploadMiB=" .. tostring(b.uploadMegabytesPerFrame) ..
             " stagingMiB=" .. tostring(b.stagingMegabytes))
end

local function once(time, name, action)
    if elapsed >= time and not fired[name] then
        fired[name] = true
        action()
    end
end

return {
    OnCreate = function(id)
        assert(id == 1257099, "Observer UUID changed")
        assert(type(Scene.SetRepresentationStreamingBudgets) == "function", "Rebuild runtime with budget Lua API")
        assert(type(Scene.GetRepresentationStreamingBudgets) == "function", "Rebuild runtime with budget Lua API")
        reloadPass = ResidencyObserverReloadCount or 0
        Scene.SetRepresentationStreamingBudgets(256, 160, 4096)
        marker("PHASE=high")
    end,
    OnUpdate = function(id, dt)
        elapsed = elapsed + dt -- real fixed simulation clock supplied by RuntimeLayer
        if reloadPass == 0 then
            once(8, "initial", function() marker("CAPTURE=initial") end)
            once(16, "pressure", function()
                Scene.SetRepresentationStreamingBudgets(0.000001, 160, 4096)
                marker("PHASE=pressure")
            end)
            once(24, "pressure-capture", function() marker("CAPTURE=pressure") end)
            once(32, "recover", function()
                Scene.SetRepresentationStreamingBudgets(256, 160, 4096)
                marker("PHASE=recovery")
            end)
            once(40, "recovered", function() marker("CAPTURE=recovered") end)
            once(48, "reload", function()
                ResidencyObserverReloadCount = 1 -- Lua VM persists across runtime scene reload
                marker("REQUEST=reload")
                Scene.ReloadCurrentScene()
            end)
        else
            once(8, "reloaded", function() marker("CAPTURE=reloaded") end)
            once(16, "done", function()
                marker("DONE")
                Application.QuitGame()
            end)
        end
    end
}
