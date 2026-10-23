-- PlayHand.lua — the player's side of playing with a dog (issue #1559).
--
-- Attach to the play scene's RuntimeCamera. Breed-agnostic: it finds what it
-- needs by name and by what the pointer hits, never by breed.
--
--   Right-drag    orbit the camera around the play spot ("PlayFocus")
--   Wheel         zoom
--   W A S D       walk the play spot across the lawn (Shift to hurry), to follow the dog
--   Left on dog   hold and stroke to pet: the "Hand" entity (a GroomCollider)
--                 rides the coat under the cursor and parts the fur
--   Left on toy   grab a "Toy..." entity; flick and release to throw it, the
--                 release speed and direction set the throw
--
-- The cursor stays free (CursorMode.Normal). The dog's side of the game is
-- DogPlay.lua; the two talk through the shared global PlayBus table, which
-- DogPlay creates. Every frame PlayBus.cursor says where the cursor points, so
-- the dog can watch the hand before it touches.
--
-- Scene contract:
--   PlayFocus  orbit pivot (its translation)
--   Hand       GroomColliderComponent, disabled until the hand touches fur
--   Toy*       Rigidbody3D (dynamic) + collider; grabbed by a physics raycast
--   the dog    PickVolumesComponent (what petting hits) + DogPlay.lua

local PlayHand = {}

local ORBIT_PER_PIXEL = 0.006   -- rad of orbit per pixel of right-drag
local PITCH_MIN, PITCH_MAX = -0.05, 1.25
local ZOOM_PER_NOTCH = 0.12     -- fraction of the distance per wheel notch
local DISTANCE_MIN, DISTANCE_MAX = 0.55, 9.0
local PICK_RANGE = 40.0
local HAND_LIFT = 0.025         -- the hand presses into the coat: its centre this far above the skin it
                                -- touched, so its sphere reaches from under the skin up through the fur
local HAND_PARKED = { 0.0, -50.0, 0.0 } -- out of every coat's reach while not petting
local THROW_FORWARD = 1.4       -- an upward flick on screen throws AWAY from the camera, this much faster
local THROW_SIDE = 1.0          -- a sideways flick throws sideways
local THROW_LIFT = 0.55         -- and the upward flick's arc
local THROW_MAX = 11.0          -- m/s
local DROP_BELOW = 0.6          -- a release slower than this just drops the toy
local PET_GRACE = 0.2           -- s: a petting hand stays on the coat through a miss this short
local REST_PIXELS = 3.0         -- a cursor this close to where the hand touched leaves it resting there
local WALK_SPEED = 2.5          -- m/s the play spot moves under W A S D, twice that with Shift
local WALK_LIMIT = 18.0         -- m from the lawn's centre: the play area is 20 m each way
local HOLD_CLEARANCE = 0.08     -- m a held toy keeps in front of the dog's fur along the cursor ray
local HOLD_HEIGHT = 0.07        -- m a held toy's centre keeps above the ground under it
local CURSOR_SKY_DEPTH = 40.0   -- m: a cursor that meets nothing points far off, at nothing near the dog

local state = {}

local function lengthOf(x, y, z) return math.sqrt(x * x + y * y + z * z) end

local function name(id)
    return entity_utils.get_name(id) or ""
end

local function isToy(id)
    -- A toy in the dog's mouth is the dog's until it drops it.
    return id ~= nil and id ~= 0 and string.sub(name(id), 1, 3) == "Toy" and (PlayBus == nil or PlayBus.inMouth ~= id)
end

local function bus()
    PlayBus = PlayBus or {}
    return PlayBus
end

-- The camera's world position from orbit angles around the focus.
local function placeCamera(id, s)
    local f = entity_utils.get_translation(s.focus)
    local cp, sp = math.cos(s.pitch), math.sin(s.pitch)
    local cy, sy = math.cos(s.yaw), math.sin(s.yaw)
    -- Engine euler convention: Rotation = Ry(yaw) * Rx(pitch), identity looks down -Z,
    -- so the camera sits BEHIND its view direction: +Z rotated by yaw, raised by pitch.
    local x = f.x + s.distance * cp * sy
    local y = f.y + s.distance * sp
    local z = f.z + s.distance * cp * cy
    entity_utils.set_translation(id, vec3.new(x, y, z))
    entity_utils.set_rotation(id, vec3.new(-s.pitch, s.yaw, 0.0))
end

local function cursorRay(id)
    local mx, my = Input.GetMousePosition()
    return Camera.ScreenToWorldRay(id, vec2.new(mx, my))
end

-- Let go of the spot the hand rests on (it stays where it is in the world).
local function unanchor(s)
    if s.anchor then
        entity_utils.detach(s.hand)
        s.anchor = nil
    end
end

local function parkHand(s)
    unanchor(s)
    entity_utils.set_translation(s.hand, vec3.new(HAND_PARKED[1], HAND_PARKED[2], HAND_PARKED[3]))
    local collider = entity_utils.get_component(s.hand, "GroomColliderComponent")
    if collider then collider.enabled = false end
    bus().petting = nil
end

-- Petting: put the hand just under the coat the cursor is on, and tell the dog.
-- False, leaving the hand where it was, when the cursor is not on a dog. The
-- hand then RESTS on the spot it touched: it rides the bone under it, so a dog
-- that breathes, leans or flaps an ear carries the hand along, as a real one
-- would, until the cursor moves on (pixel x, y).
local function stroke(id, s, ray, x, y)
    local hit = Physics.RaycastPickVolumes(ray.origin, ray.direction, PICK_RANGE)
    if hit == nil then
        return false
    end
    unanchor(s)
    local n = hit.normal
    entity_utils.set_translation(s.hand, vec3.new(hit.position.x + n.x * HAND_LIFT,
        hit.position.y + n.y * HAND_LIFT, hit.position.z + n.z * HAND_LIFT))
    local collider = entity_utils.get_component(s.hand, "GroomColliderComponent")
    if collider then collider.enabled = true end
    if hit.bone ~= "" and entity_utils.attach_in_place(s.hand, hit.entityID, hit.bone) then
        s.anchor = { x = x, y = y, n = n }
    end

    -- How much the hand moved this frame along the coat: a held, still hand is
    -- a touch; a moving one is a stroke.
    local p = hit.position
    local moved = 0.0
    if s.lastPet then moved = lengthOf(p.x - s.lastPet.x, p.y - s.lastPet.y, p.z - s.lastPet.z) end
    s.lastPet = { x = p.x, y = p.y, z = p.z }
    bus().petting = {
        dog = hit.entityID, region = hit.region, bone = hit.bone,
        x = p.x, y = p.y, z = p.z, ny = n.y, moved = moved, hand = s.hand,
    }
    return true
end

-- Where the cursor points, for the dog's attention: on the dog, on whatever
-- else the ray meets, or (pointing at the sky) a point a few metres out.
local function publishCursor(s, ray, dx, dy)
    if ray == nil then
        bus().cursor = nil
        return
    end
    local o, d = ray.origin, ray.direction
    local onDog = Physics.RaycastPickVolumes(o, d, PICK_RANGE)
    local cursor
    if onDog ~= nil then
        local p = onDog.position
        cursor = { x = p.x, y = p.y, z = p.z, overDog = true, region = onDog.region }
    else
        local hit = Physics.Raycast(o, d, PICK_RANGE)
        local t = hit ~= nil and hit.distance or CURSOR_SKY_DEPTH
        cursor = { x = o.x + d.x * t, y = o.y + d.y * t, z = o.z + d.z * t, overDog = false }
    end
    cursor.moved = math.abs(dx) + math.abs(dy) > 0.5
    bus().cursor = cursor
end

-- How far along the ray its first hit lies, passing through `ignore` (a held
-- toy sits on the ray, at the cursor). nil when it meets nothing in range.
local function firstHitPast(origin, dir, range, ignore)
    local o = origin
    local travelled = 0.0
    for _ = 1, 4 do
        local hit = Physics.Raycast(o, dir, range - travelled)
        if hit == nil then return nil end
        if hit.entityID ~= ignore then return travelled + hit.distance end
        local step = hit.distance + 0.01
        travelled = travelled + step
        o = vec3.new(o.x + dir.x * step, o.y + dir.y * step, o.z + dir.z * step)
    end
    return nil
end

local function grab(id, s, toy, ray, distance)
    -- The toy rides the hand at the depth it was picked at; the hand follows the
    -- cursor on that sphere around the camera.
    s.holdDepth = distance
    local p = entity_utils.get_translation(toy)
    entity_utils.set_translation(s.hand, p)
    if entity_utils.attach_to(toy, s.hand, "", vec3.new(0, 0, 0)) then
        s.held = toy
        s.mode = "hold"
        bus().held = toy
    end
end

local function release(s)
    local toy = s.held
    s.held = nil
    bus().held = nil
    if toy == nil then return end
    if not entity_utils.detach(toy) then return end
    -- The detach already gave the toy the hand's velocity; shape it into a throw.
    local rb = entity_utils.get_component(toy, "Rigidbody3DComponent")
    if rb == nil then return end
    local v = rb.linearVelocity
    if lengthOf(v.x, v.y, v.z) < DROP_BELOW then
        rb.linearVelocity = vec3.new(0, 0, 0)
        return
    end
    -- The hand moved on the screen, so its velocity lies in the view plane. A
    -- flick UP the screen means "away"; read the velocity in screen axes and
    -- turn its up part into a forward throw with an arc.
    local cp, sp = math.cos(s.pitch), math.sin(s.pitch)
    local cy, sy = math.cos(s.yaw), math.sin(s.yaw)
    local rx, rz = cy, -sy                          -- screen right (horizontal)
    local ux, uy, uz = -sp * sy, cp, -sp * cy        -- screen up
    local fx, fz = -sy, -cy                          -- away from the camera, on the ground
    local side = v.x * rx + v.z * rz
    local up = math.max(0.0, v.x * ux + v.y * uy + v.z * uz)
    local tx = rx * side * THROW_SIDE + fx * up * THROW_FORWARD
    local tz = rz * side * THROW_SIDE + fz * up * THROW_FORWARD
    local ty = up * THROW_LIFT
    local speed = lengthOf(tx, ty, tz)
    if speed > THROW_MAX then
        tx, ty, tz = tx * THROW_MAX / speed, ty * THROW_MAX / speed, tz * THROW_MAX / speed
    end
    rb.linearVelocity = vec3.new(tx, ty, tz)
    bus().thrown = { toy = toy, frame = s.frame }
end

function PlayHand.OnCreate(id)
    local focus = entity_utils.find_by_name("PlayFocus") or id
    local hand = entity_utils.find_by_name("Hand")
    local s = {
        focus = focus, hand = hand, mode = "idle", frame = 0,
        yaw = 0.6, pitch = 0.35, distance = 2.6,
    }
    -- Start from where the scene put the camera, so the authored framing holds.
    local c = entity_utils.get_translation(id)
    local f = entity_utils.get_translation(focus)
    local dx, dy, dz = c.x - f.x, c.y - f.y, c.z - f.z
    local d = lengthOf(dx, dy, dz)
    if d > 0.1 then
        s.distance = math.max(DISTANCE_MIN, math.min(DISTANCE_MAX, d))
        s.pitch = math.max(PITCH_MIN, math.min(PITCH_MAX, math.asin(dy / d)))
        s.yaw = math.atan(dx, dz)
    end
    local mx, my = Input.GetMousePosition()
    s.lastX, s.lastY = mx, my
    state[id] = s
    Input.SetCursorMode(CursorMode.Normal)
    if hand == nil then
        Log.Warn("[PlayHand] no 'Hand' entity: petting and throwing are off")
    else
        parkHand(s)
    end
end

function PlayHand.OnUpdate(id, dt)
    local s = state[id]
    if s == nil then return end
    s.frame = s.frame + 1

    local mx, my = Input.GetMousePosition()
    local dx, dy = mx - s.lastX, my - s.lastY
    s.lastX, s.lastY = mx, my

    -- Orbit and zoom.
    if Input.IsMouseButtonDown(MouseButton.ButtonRight) then
        s.yaw = s.yaw - dx * ORBIT_PER_PIXEL
        s.pitch = math.max(PITCH_MIN, math.min(PITCH_MAX, s.pitch + dy * ORBIT_PER_PIXEL))
    end
    local _, wheel = Input.GetMouseScrollDelta()
    if wheel ~= 0 then
        s.distance = math.max(DISTANCE_MIN, math.min(DISTANCE_MAX, s.distance * (1.0 - ZOOM_PER_NOTCH * wheel)))
    end
    -- Walk: the play spot moves on the ground, forward being away from the camera.
    local fwd = (Input.IsKeyDown(KeyCode.W) and 1 or 0) - (Input.IsKeyDown(KeyCode.S) and 1 or 0)
    local right = (Input.IsKeyDown(KeyCode.D) and 1 or 0) - (Input.IsKeyDown(KeyCode.A) and 1 or 0)
    if fwd ~= 0 or right ~= 0 then
        local speed = WALK_SPEED * dt * (Input.IsKeyDown(KeyCode.LeftShift) and 2.0 or 1.0) / math.sqrt(fwd * fwd + right * right)
        local cy, sy = math.cos(s.yaw), math.sin(s.yaw)
        local f = entity_utils.get_translation(s.focus)
        local x = f.x + (-sy * fwd + cy * right) * speed
        local z = f.z + (-cy * fwd - sy * right) * speed
        x = math.max(-WALK_LIMIT, math.min(WALK_LIMIT, x))
        z = math.max(-WALK_LIMIT, math.min(WALK_LIMIT, z))
        entity_utils.set_translation(s.focus, vec3.new(x, f.y, z))
    end
    placeCamera(id, s)

    if s.hand == nil then return end
    local left = Input.IsMouseButtonDown(MouseButton.ButtonLeft)
    local ray = cursorRay(id)
    publishCursor(s, ray, dx, dy)

    if s.mode == "idle" then
        if left and not s.prevLeft and ray ~= nil then
            -- A toy under the cursor is grabbed; otherwise the dog is petted.
            local hit = Physics.Raycast(ray.origin, ray.direction, PICK_RANGE)
            if hit ~= nil and isToy(hit.entityID) then
                grab(id, s, hit.entityID, ray, hit.distance)
            elseif stroke(id, s, ray, mx, my) then
                s.mode = "pet"
                s.missFor = 0.0
            end
        end
    elseif s.mode == "pet" then
        -- Petting lasts while the button is held. A dog that shifts under the
        -- hand (an ear flaps, it leans in) slips out from under the cursor for
        -- a frame or two: the hand stays on the coat through a short miss, and
        -- lifts off only when the cursor has really left the dog.
        if not left then
            parkHand(s)
            s.lastPet = nil
            s.mode = "idle"
        elseif s.anchor and math.abs(mx - s.anchor.x) + math.abs(my - s.anchor.y) <= REST_PIXELS then
            -- A resting hand: it rides the spot, and the dog still feels it there.
            local h, n = entity_utils.get_world_translation(s.hand), s.anchor.n
            local p = bus().petting
            if p then
                p.x, p.y, p.z = h.x - n.x * HAND_LIFT, h.y - n.y * HAND_LIFT, h.z - n.z * HAND_LIFT
                p.moved = 0.0
            end
            s.missFor = 0.0
        elseif ray ~= nil and stroke(id, s, ray, mx, my) then
            s.missFor = 0.0
        else
            s.missFor = (s.missFor or 0.0) + dt
            if s.missFor > PET_GRACE then
                parkHand(s)
                s.lastPet = nil
            end
        end
    elseif s.mode == "hold" then
        if ray ~= nil then
            -- On the cursor's ray at the depth it was picked up at, but never
            -- into the dog's fur, and never lower than a toy's height above the
            -- ground under it. Drawn down the screen, a toy at a fixed depth went
            -- under the lawn, and let go there it fell for ever. The height is
            -- measured straight down, not along the ray: at the play camera's
            -- grazing angle a clearance along the ray is almost none in height,
            -- and its hit distance jumped about from frame to frame.
            local o, d = ray.origin, ray.direction
            local depth = s.holdDepth
            local dog = Physics.RaycastPickVolumes(o, d, PICK_RANGE)
            if dog ~= nil then depth = math.min(depth, dog.distance - HOLD_CLEARANCE) end
            depth = math.max(depth, 0.05)
            local p = { x = o.x + d.x * depth, y = o.y + d.y * depth, z = o.z + d.z * depth }
            local below = firstHitPast(vec3.new(p.x, p.y + 2.0, p.z), vec3.new(0, -1, 0), 50.0, s.held)
            if below ~= nil then
                local floor = p.y + 2.0 - below + HOLD_HEIGHT
                if p.y < floor then
                    if d.y < -1e-3 then
                        -- Slide back along the ray to where it reaches that height,
                        -- so the toy stays under the cursor.
                        local t = (floor - o.y) / d.y
                        p = { x = o.x + d.x * t, y = floor, z = o.z + d.z * t }
                    else
                        p.y = floor
                    end
                end
            end
            entity_utils.set_translation(s.hand, vec3.new(p.x, p.y, p.z))
        end
        if not left then
            release(s)
            parkHand(s)
            s.mode = "idle"
        end
    end
    s.prevLeft = left
end

function PlayHand.OnDestroy(id)
    state[id] = nil
end

return PlayHand
