-- LuaPlayer.lua — Lua port of Player.cs
-- A simple 2D player controller with WASD movement using Rigidbody2D impulse.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaPlayer.lua"

local LuaPlayer = {}

-- KeyCode values (GLFW)
local KEY_W = 87
local KEY_A = 65
local KEY_S = 83
local KEY_D = 68
local KEY_Q = 81
local KEY_E = 69

local speed = 1.0
local entityID = nil

function LuaPlayer.OnCreate(id)
    entityID = id
    Log.Info("[LuaPlayer] OnCreate — entity " .. tostring(id))
end

function LuaPlayer.OnUpdate(id, dt)
    local transform = entity_utils.get_component(id, "TransformComponent")
    if not transform then return end

    local velocityX = 0.0
    local velocityY = 0.0

    if Input.IsKeyDown(KEY_W) then velocityY = 1.0 end
    if Input.IsKeyDown(KEY_S) then velocityY = velocityY - 1.0 end
    if Input.IsKeyDown(KEY_A) then velocityX = -1.0 end
    if Input.IsKeyDown(KEY_D) then velocityX = velocityX + 1.0 end

    -- Apply movement directly to translation (simple, no physics)
    local pos = transform.translation
    pos.x = pos.x + velocityX * speed * dt
    pos.y = pos.y + velocityY * speed * dt
    transform.translation = pos

    -- Camera distance control
    local cameraID = entity_utils.find_by_name("Camera")
    if cameraID then
        local camTransform = entity_utils.get_component(cameraID, "TransformComponent")
        if camTransform then
            if Input.IsKeyDown(KEY_Q) then
                local p = camTransform.translation
                p.z = p.z + speed * 2.0 * dt
                camTransform.translation = p
            elseif Input.IsKeyDown(KEY_E) then
                local p = camTransform.translation
                p.z = p.z - speed * 2.0 * dt
                camTransform.translation = p
            end
        end
    end
end

function LuaPlayer.OnDestroy(id)
    Log.Info("[LuaPlayer] OnDestroy — entity " .. tostring(id))
end

return LuaPlayer
