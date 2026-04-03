-- LuaPlayer.lua — Lua port of Player.cs
-- A simple 2D player controller with WASD movement using Rigidbody2D impulse.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaPlayer.lua"

local LuaPlayer = {}

local speed = 1.0

function LuaPlayer.OnCreate(id)
    local name = entity_utils.get_name(id) or "unknown"
    Log.Info("[LuaPlayer] OnCreate — entity " .. name .. " (" .. tostring(id) .. ")")
end

function LuaPlayer.OnUpdate(id, dt)
    local pos = entity_utils.get_translation(id)

    local velocityX = 0.0
    local velocityY = 0.0

    if Input.IsKeyDown(KeyCode.W) then velocityY = 1.0 end
    if Input.IsKeyDown(KeyCode.S) then velocityY = velocityY - 1.0 end
    if Input.IsKeyDown(KeyCode.A) then velocityX = -1.0 end
    if Input.IsKeyDown(KeyCode.D) then velocityX = velocityX + 1.0 end

    -- Apply movement directly to translation (simple, no physics)
    pos.x = pos.x + velocityX * speed * dt
    pos.y = pos.y + velocityY * speed * dt
    entity_utils.set_translation(id, pos)

    -- Camera distance control
    local cameraID = entity_utils.find_by_name("Camera")
    if cameraID then
        local camPos = entity_utils.get_translation(cameraID)
        if Input.IsKeyDown(KeyCode.Q) then
            camPos.z = camPos.z + speed * 2.0 * dt
            entity_utils.set_translation(cameraID, camPos)
        elseif Input.IsKeyDown(KeyCode.E) then
            camPos.z = camPos.z - speed * 2.0 * dt
            entity_utils.set_translation(cameraID, camPos)
        end
    end
end

function LuaPlayer.OnDestroy(id)
    Log.Info("[LuaPlayer] OnDestroy — entity " .. tostring(id))
end

return LuaPlayer
