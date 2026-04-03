-- LuaCamera.lua — Lua port of Camera.cs
-- A simple camera that follows the player entity with an offset.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaCamera.lua"

local LuaCamera = {}

local distanceFromPlayer = 5.0

function LuaCamera.OnCreate(id)
    local name = entity_utils.get_name(id) or "unknown"
    Log.Info("[LuaCamera] OnCreate — entity " .. name .. " (" .. tostring(id) .. ")")
end

function LuaCamera.OnUpdate(id, dt)
    -- Follow player
    local playerID = entity_utils.find_by_name("Player")
    if playerID then
        local playerPos = entity_utils.get_translation(playerID)
        local pos = entity_utils.get_translation(id)
        pos.x = playerPos.x
        pos.y = playerPos.y
        pos.z = distanceFromPlayer
        entity_utils.set_translation(id, pos)
    end

    -- Arrow key panning
    local speed = 1.0
    local pos = entity_utils.get_translation(id)
    local moved = false

    if Input.IsKeyDown(KeyCode.Up)    then pos.y = pos.y + speed * dt; moved = true end
    if Input.IsKeyDown(KeyCode.Down)  then pos.y = pos.y - speed * dt; moved = true end
    if Input.IsKeyDown(KeyCode.Left)  then pos.x = pos.x - speed * dt; moved = true end
    if Input.IsKeyDown(KeyCode.Right) then pos.x = pos.x + speed * dt; moved = true end

    if moved then
        entity_utils.set_translation(id, pos)
    end
end

function LuaCamera.OnDestroy(id)
    Log.Info("[LuaCamera] OnDestroy — entity " .. tostring(id))
end

return LuaCamera
