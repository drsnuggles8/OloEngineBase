-- LuaTerrainController.lua — Lua port of TerrainController.cs (issue #293).
-- Drives TerrainComponent's procedural-generation params from a gameplay script.
-- Attach via LuaScriptComponent with
--   ScriptFile = "Scripts/LuaScripts/LuaTerrainController.lua"
--
-- On create it applies a few authored knobs and rolls a fresh seed so a "new game"
-- looks distinct from the editor-authored preview; press R at runtime to regenerate
-- the world. Setting params alone does nothing — the engine rebuilds the height
-- field on the next tick only after regenerate() drops the cached terrain data.

local TerrainController = {}

-- Authored knobs applied on first run.
local octaves = 7
local ridgeBlend = 0.6   -- 0 = rolling hills, 1 = sharp ridged mountains
local heightScale = 120.0

local function reseed(id)
    local terrain = entity_utils.get_component(id, "TerrainComponent")
    if terrain == nil then return end
    -- math.random returns a float in [0,1); scale into the i32 seed range.
    terrain.seed = math.floor(math.random() * 2147483647)
    terrain:regenerate() -- rebuilds the height field on the next tick
    Log.Info("[LuaTerrainController] regenerated with seed " .. tostring(terrain.seed))
end

function TerrainController.OnCreate(id)
    local terrain = entity_utils.get_component(id, "TerrainComponent")
    if terrain == nil then
        Log.Warn("[LuaTerrainController] entity has no TerrainComponent")
        return
    end
    terrain.proceduralEnabled = true
    terrain.octaves = octaves
    terrain.ridgeBlend = ridgeBlend
    terrain.heightScale = heightScale
    reseed(id)
end

function TerrainController.OnUpdate(id, dt)
    -- Roll a fresh world on demand (e.g. a "regenerate" button / level reset).
    if Input.IsKeyJustPressed(KeyCode.R) then
        reseed(id)
    end
end

return TerrainController
