-- LuaCameraFollow.lua — Lua port of CameraFollow.cs
-- Smooth overhead camera follow with configurable offset and lerp speed.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaCameraFollow.lua"

local CameraFollow = {}

local offsetY = 0.0
local offsetZ = 5.0
local smoothSpeed = 5.0

function CameraFollow.OnCreate(id)
    Log.Info("[LuaCameraFollow] OnCreate — " .. (entity_utils.get_name(id) or "Camera"))
end

function CameraFollow.OnUpdate(id, dt)
    local playerID = entity_utils.find_by_name("Player")
    if not playerID then return end

    local playerPos = entity_utils.get_translation(playerID)
    local pos = entity_utils.get_translation(id)

    -- Target position: player XY + offset
    local targetX = playerPos.x
    local targetY = playerPos.y + offsetY
    local targetZ = offsetZ

    -- Smooth interpolation (lerp)
    local t = math.min(1.0, smoothSpeed * dt)
    pos.x = pos.x + (targetX - pos.x) * t
    pos.y = pos.y + (targetY - pos.y) * t
    pos.z = pos.z + (targetZ - pos.z) * t

    entity_utils.set_translation(id, pos)

    -- Arrow key manual panning override
    local panSpeed = 3.0
    local panned = false
    if Input.IsKeyDown(KeyCode.Up)    then pos.y = pos.y + panSpeed * dt; panned = true end
    if Input.IsKeyDown(KeyCode.Down)  then pos.y = pos.y - panSpeed * dt; panned = true end
    if Input.IsKeyDown(KeyCode.Left)  then pos.x = pos.x - panSpeed * dt; panned = true end
    if Input.IsKeyDown(KeyCode.Right) then pos.x = pos.x + panSpeed * dt; panned = true end

    if panned then
        entity_utils.set_translation(id, pos)
    end
end

function CameraFollow.OnDestroy(id)
    Log.Info("[LuaCameraFollow] OnDestroy")
end

return CameraFollow
