-- LuaCamera.lua — Lua port of Camera.cs
-- A simple camera that follows the player entity with an offset.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaCamera.lua"

local LuaCamera = {}

local panOffsets = {}

function LuaCamera.OnCreate(id)
    panOffsets[id] = { x = 0.0, y = 0.0 }
    local name = entity_utils.get_name(id) or "unknown"
    Log.Info("[LuaCamera] OnCreate — entity " .. name .. " (" .. tostring(id) .. ")")
end

function LuaCamera.OnUpdate(id, dt)
    local offset = panOffsets[id]
    if not offset then
        offset = { x = 0.0, y = 0.0 }
        panOffsets[id] = offset
    end

    -- Arrow key panning (accumulates into persistent offset)
    local speed = 1.0
    if Input.IsKeyDown(KeyCode.Up)    then offset.y = offset.y + speed * dt end
    if Input.IsKeyDown(KeyCode.Down)  then offset.y = offset.y - speed * dt end
    if Input.IsKeyDown(KeyCode.Left)  then offset.x = offset.x - speed * dt end
    if Input.IsKeyDown(KeyCode.Right) then offset.x = offset.x + speed * dt end

    -- Follow player (XY only, preserve current Z for zoom)
    local playerID = entity_utils.find_by_name("Player")
    if playerID then
        local playerPos = entity_utils.get_translation(playerID)
        local pos = entity_utils.get_translation(id)
        pos.x = playerPos.x + offset.x
        pos.y = playerPos.y + offset.y
        -- Keep pos.z as-is so LuaPlayer zoom (Q/E) persists
        entity_utils.set_translation(id, pos)
    end
end

function LuaCamera.OnDestroy(id)
    panOffsets[id] = nil
    Log.Info("[LuaCamera] OnDestroy — entity " .. tostring(id))
end

return LuaCamera
