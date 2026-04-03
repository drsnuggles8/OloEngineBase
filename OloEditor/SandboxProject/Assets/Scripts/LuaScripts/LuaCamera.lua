-- LuaCamera.lua — Lua port of Camera.cs
-- A simple camera that follows the player entity with an offset.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaCamera.lua"

local LuaCamera = {}

local distanceFromPlayer = 5.0
local panOffset = { x = 0.0, y = 0.0 }

function LuaCamera.OnCreate(id)
    panOffset = { x = 0.0, y = 0.0 }
    local name = entity_utils.get_name(id) or "unknown"
    Log.Info("[LuaCamera] OnCreate — entity " .. name .. " (" .. tostring(id) .. ")")
end

function LuaCamera.OnUpdate(id, dt)
    -- Arrow key panning (accumulates into persistent offset)
    local speed = 1.0
    if Input.IsKeyDown(KeyCode.Up)    then panOffset.y = panOffset.y + speed * dt end
    if Input.IsKeyDown(KeyCode.Down)  then panOffset.y = panOffset.y - speed * dt end
    if Input.IsKeyDown(KeyCode.Left)  then panOffset.x = panOffset.x - speed * dt end
    if Input.IsKeyDown(KeyCode.Right) then panOffset.x = panOffset.x + speed * dt end

    -- Follow player (XY only, preserve current Z for zoom)
    local playerID = entity_utils.find_by_name("Player")
    if playerID then
        local playerPos = entity_utils.get_translation(playerID)
        local pos = entity_utils.get_translation(id)
        pos.x = playerPos.x + panOffset.x
        pos.y = playerPos.y + panOffset.y
        -- Keep pos.z as-is so LuaPlayer zoom (Q/E) persists
        entity_utils.set_translation(id, pos)
    end
end

function LuaCamera.OnDestroy(id)
    Log.Info("[LuaCamera] OnDestroy — entity " .. tostring(id))
end

return LuaCamera
