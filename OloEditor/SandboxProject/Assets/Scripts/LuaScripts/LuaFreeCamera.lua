-- LuaFreeCamera.lua — runtime free-fly FPS camera for Play mode.
--
-- Lets you fly around while the scene is playing (the editor's free camera only
-- works in edit mode; in Play the view is locked to the primary CameraComponent).
--
--   Mouse        aim (FPS-style — the cursor is captured while playing)
--   W / S        move forward / back (in the direction you're looking)
--   A / D        strafe left / right
--   E / Q        move up / down (world)
--   LeftShift    boost speed
--   Esc          release the mouse (so you can click Stop / the editor UI)
--   Left-click   re-capture the mouse for FPS look
--   Arrow keys   look (keyboard alternative, always works)
--
-- Attach to a camera entity via LuaScriptComponent:
--   ScriptFile: Scripts/LuaScripts/LuaFreeCamera.lua
--
-- Cursor capture uses Input.SetCursorMode(CursorMode.Locked): the OS cursor is
-- hidden + pinned and Input.GetMousePosition() reports unbounded virtual motion,
-- so mouse-look never stalls at the window edge.
--
-- Movement basis matches the engine euler convention
--   Rotation = glm::quat(vec3(pitch, yaw, roll)) = Ry(yaw) * Rx(pitch)
-- so forward = (-cos(pitch)*sin(yaw), sin(pitch), -cos(pitch)*cos(yaw)) and
-- right = (cos(yaw), 0, -sin(yaw)). Identity rotation looks down -Z.

local LuaFreeCamera = {}

local MOVE_SPEED  = 8.0     -- units / second
local BOOST_MULT  = 3.0     -- LeftShift multiplier
local LOOK_SPEED  = 1.6     -- radians / second (arrow keys)
local MOUSE_SENS  = 0.0025  -- radians / pixel of mouse motion
local PITCH_LIMIT = 1.4     -- clamp so you can't flip over the pole (~80 deg)

-- Per-entity state: capture flag, last mouse sample, edge-detect helpers.
local state = {}

local function capture(s)
    Input.SetCursorMode(CursorMode.Locked)
    s.captured = true
    s.skipDelta = true -- ignore the first delta after (re)capture to avoid a jump
end

local function release(s)
    Input.SetCursorMode(CursorMode.Normal)
    s.captured = false
end

function LuaFreeCamera.OnCreate(id)
    local mx, my = Input.GetMousePosition()
    local s = { captured = false, skipDelta = true, lastX = mx, lastY = my, prevLMB = false }
    state[id] = s
    capture(s) -- start in FPS mode
    Log.Info("[LuaFreeCamera] FPS camera: mouse aims, WASD move, E/Q up/down, Shift boost, Esc frees the mouse")
end

function LuaFreeCamera.OnUpdate(id, dt)
    local s = state[id]
    if not s then
        local mx, my = Input.GetMousePosition()
        s = { captured = false, skipDelta = true, lastX = mx, lastY = my, prevLMB = false }
        state[id] = s
    end

    -- ---- Capture toggle ----
    if s.captured and Input.IsKeyJustPressed(KeyCode.Escape) then
        release(s)
    end
    local lmb = Input.IsMouseButtonDown(MouseButton.ButtonLeft)
    if (not s.captured) and lmb and (not s.prevLMB) then
        capture(s)
    end
    s.prevLMB = lmb

    -- ---- Look ----
    local rot = entity_utils.get_rotation(id) -- x = pitch, y = yaw, z = roll
    local pitch = rot.x
    local yaw = rot.y

    -- Arrow keys (keyboard alternative).
    if Input.IsKeyDown(KeyCode.Left)  then yaw = yaw + LOOK_SPEED * dt end
    if Input.IsKeyDown(KeyCode.Right) then yaw = yaw - LOOK_SPEED * dt end
    if Input.IsKeyDown(KeyCode.Up)    then pitch = pitch + LOOK_SPEED * dt end
    if Input.IsKeyDown(KeyCode.Down)  then pitch = pitch - LOOK_SPEED * dt end

    -- Mouse aim (only while captured).
    local mx, my = Input.GetMousePosition()
    if s.captured then
        local dx, dy = 0.0, 0.0
        if s.skipDelta then
            s.skipDelta = false
        else
            dx = mx - s.lastX
            dy = my - s.lastY
        end
        yaw = yaw - dx * MOUSE_SENS    -- mouse right -> turn right
        pitch = pitch - dy * MOUSE_SENS -- mouse up (screen y down) -> look up
    end
    s.lastX = mx
    s.lastY = my

    if pitch >  PITCH_LIMIT then pitch =  PITCH_LIMIT end
    if pitch < -PITCH_LIMIT then pitch = -PITCH_LIMIT end
    rot.x = pitch
    rot.y = yaw
    rot.z = 0.0
    entity_utils.set_rotation(id, rot)

    -- ---- Movement basis from the (updated) yaw/pitch ----
    local cp = math.cos(pitch)
    local sp = math.sin(pitch)
    local cy = math.cos(yaw)
    local sy = math.sin(yaw)
    local fwdX, fwdY, fwdZ = -cp * sy, sp, -cp * cy -- look direction
    local rightX, rightZ = cy, -sy                  -- horizontal strafe

    local step = MOVE_SPEED * dt
    if Input.IsKeyDown(KeyCode.LeftShift) then step = step * BOOST_MULT end

    -- ---- Move ----
    local pos = entity_utils.get_translation(id)
    if Input.IsKeyDown(KeyCode.W) then
        pos.x = pos.x + fwdX * step
        pos.y = pos.y + fwdY * step
        pos.z = pos.z + fwdZ * step
    end
    if Input.IsKeyDown(KeyCode.S) then
        pos.x = pos.x - fwdX * step
        pos.y = pos.y - fwdY * step
        pos.z = pos.z - fwdZ * step
    end
    if Input.IsKeyDown(KeyCode.D) then
        pos.x = pos.x + rightX * step
        pos.z = pos.z + rightZ * step
    end
    if Input.IsKeyDown(KeyCode.A) then
        pos.x = pos.x - rightX * step
        pos.z = pos.z - rightZ * step
    end
    if Input.IsKeyDown(KeyCode.E) then pos.y = pos.y + step end
    if Input.IsKeyDown(KeyCode.Q) then pos.y = pos.y - step end
    entity_utils.set_translation(id, pos)
end

function LuaFreeCamera.OnDestroy(id)
    -- Always hand the cursor back when play stops, even if we were captured.
    Input.SetCursorMode(CursorMode.Normal)
    state[id] = nil
end

return LuaFreeCamera
