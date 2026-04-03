-- LuaCamera.lua — Lua port of Camera.cs
-- A simple camera that follows the player entity with an offset.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaCamera.lua"

local LuaCamera = {}

local distanceFromPlayer = 5.0
local KEY_UP    = 265
local KEY_DOWN  = 264
local KEY_LEFT  = 263
local KEY_RIGHT = 262

function LuaCamera.OnCreate(id)
    Log.Info("[LuaCamera] OnCreate — entity " .. tostring(id))
end

function LuaCamera.OnUpdate(id, dt)
    local transform = entity_utils.get_component(id, "TransformComponent")
    if not transform then return end

    -- Follow player
    local playerID = entity_utils.find_by_name("Player")
    if playerID then
        local playerTransform = entity_utils.get_component(playerID, "TransformComponent")
        if playerTransform then
            local playerPos = playerTransform.translation
            local pos = transform.translation
            pos.x = playerPos.x
            pos.y = playerPos.y
            pos.z = distanceFromPlayer
            transform.translation = pos
        end
    end

    -- Arrow key panning
    local speed = 1.0
    local pos = transform.translation
    local moved = false

    if Input.IsKeyDown(KEY_UP)    then pos.y = pos.y + speed * dt; moved = true end
    if Input.IsKeyDown(KEY_DOWN)  then pos.y = pos.y - speed * dt; moved = true end
    if Input.IsKeyDown(KEY_LEFT)  then pos.x = pos.x - speed * dt; moved = true end
    if Input.IsKeyDown(KEY_RIGHT) then pos.x = pos.x + speed * dt; moved = true end

    if moved then
        transform.translation = pos
    end
end

function LuaCamera.OnDestroy(id)
    Log.Info("[LuaCamera] OnDestroy — entity " .. tostring(id))
end

return LuaCamera
