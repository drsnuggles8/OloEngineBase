-- DogPlay.lua — a dog that is alive and plays (issue #1559). Attach to the dog.
--
-- Three layers, so the dog never snaps between "standing still" and a scripted
-- action:
--
--   1. ATTENTION, every frame, whatever else it does: the head (aim IK on the
--      head bone) tracks what interests the dog -- the cursor or hand near it,
--      a toy in the player's hand or in flight, the player, now and then a spot
--      on the lawn -- with quick glances between targets. A target outside the
--      head's reach turns the body instead.
--   2. MOOD: energy, excitement, affection-need and curiosity rise and fall with
--      what happens (running tires it, petting satisfies it, a held-up toy
--      excites it, being ignored makes it want company, idling makes it
--      curious).
--   3. INITIATIVE: when it is free, the dog picks what its mood calls for -- come
--      to the player and nudge the hand, fetch a toy on its own and bring it,
--      invite play, wander and sniff, sit and scratch, stretch, shake off, lie
--      down when tired and roll over for a belly rub, zoomies when very excited
--      -- instead of idling on a clock.
--
-- Breed-agnostic: the clips come from the shared generator (dog_clips.py), and
-- what differs per breed is authored in the scene as children of the dog:
--   MouthSocketBall / MouthSocketStick  where a held toy sits (BoneAttachment "jaw")
--   PickUpReach                         where the mouth meets the ground at PickUp's grab
-- and the dog carries a NavAgentComponent (UseRootMotion), an IKTargetComponent,
-- a PickVolumesComponent and an AnimationStateComponent.
--
-- The player's side is PlayHand.lua; the two talk through the global PlayBus:
--   PlayBus.petting = { dog, region, bone, x, y, z, ny, moved, hand } while a hand strokes
--   PlayBus.cursor  = { x, y, z, overDog, region, moved } where the cursor points
--   PlayBus.held    = toy id while the player holds a toy
--   PlayBus.thrown  = { toy, frame } when one is let go
--   PlayBus.inMouth = toy id while the dog carries it
--   PlayBus.reaction, dogState, dogAction, mood   (published, for tests and tools)
-- PlayConfig (a global the scene or a test may set):
--   fetchDisabled = true   watch thrown toys, never fetch
--   mood = { energy, excitement, affection, curiosity }   start (or restart) from this mood
--   seed = n               the dog's random choices repeat for the same seed

local DogPlay = {}

-- Clip events (rig.json events)
local GRAB_AT = 0.62
local RELEASE_AT = 0.48

local GRAB_REACH = 0.35
local STOP_EXTRA = 0.25
local RETURN_STOP = 0.35
-- Where "the player's feet" are: on the ground this far in front of the camera,
-- toward the play spot. Close enough to be with the player, far enough that a
-- dog waiting there is in the picture rather than filling it (it was 0.9 m).
local FEET_AHEAD = 1.3
-- A player who walks off holding a toy is followed once they are FOLLOW_FAR (m)
-- from the dog, until the dog is back within FOLLOW_NEAR; past FOLLOW_RUN it runs.
local FOLLOW_FAR = 2.2
local FOLLOW_NEAR = 1.0
local FOLLOW_RUN = 4.5
local PET_LINGER = 0.4
local CHASE_GIVE_UP = 20.0
local TURN_RATE = 1.4         -- rad/s (80 deg/s), turning on the spot (TurnInPlace plays meanwhile)
local FAVOURITE_SPEED = 1.35
local BLEND_QUICK = 0.25
local BLEND_SOFT = 0.45
-- Stepping round on the spot starts past TURN_START and stops inside TURN_STOP
-- (rad): one threshold made the dog flick between stepping and standing every
-- few frames while what it watched sat near it.
local TURN_START = 0.25
local TURN_STOP = 0.08

-- Attention
-- rad either side of the body's forward the head looks round: a dog turns its
-- head about 60-70 degrees and the rest with its body. At 1.75 (100 degrees),
-- shared by a three-bone neck, a toy behind the dog twisted the neck like a doll's.
local HEAD_YAW_LIMIT = 1.1
local HEAD_PITCH_LIMIT = 0.9  -- rad up or down the head looks; past it the look stops at the edge
local GLANCE_EVERY = { 3.0, 7.0 }
local GLANCE_FOR = { 0.8, 1.6 }
-- A dog's head turns briskly but not instantly: the look point swings round the
-- head at most this fast (rad/s, about 115 deg/s), and eases in at HEAD_EASE (1/s).
-- Chasing a live cursor at 6-14/s made the head whip about with every mouse move.
local HEAD_TURN_SPEED = 2.0
-- The head never aims at a point closer than this (m). A toy swept across the
-- dog slides along its fur, within centimetres of the head, and aiming at a
-- point that close flipped the aim from frame to frame: the head spun round.
local LOOK_MIN_DISTANCE = 0.5
local HEAD_EASE = 4.0
local LOOK_WEIGHT_RATE = 2.5  -- 1/s the look's weight eases toward what attention asks
-- The body turns toward what the dog watches once it is this far off the
-- body's forward (rad) for TURN_AFTER seconds: a dog watching a toy moves its
-- whole self, not its neck alone.
local BODY_FOLLOW = 0.5
local TURN_AFTER = 0.3
local NOTICE_HAND = 1.6       -- a cursor this close to the dog's head is interesting

-- Mood rates per second
local ENERGY_RUN = -0.035
local ENERGY_TROT = -0.008
local ENERGY_REST = 0.05
local ENERGY_IDLE = 0.004
local EXCITE_DECAY = 0.05
local AFFECTION_GROW = 0.012
local AFFECTION_PETTED = -0.25
local CURIOSITY_GROW = 0.02

-- A dog does not stretch twice in a minute: seconds before an idle habit
-- comes round again.
local COOLDOWN = { stretch = 45.0, shake = 30.0, zoomies = 40.0, sniff = 12.0, sit = 8.0, nudge = 6.0 }

local state = {}

local MOOD_DEFAULT = { energy = 0.85, excitement = 0.35, affection = 0.3, curiosity = 0.4 }

-- ---------------------------------------------------------------- helpers

local function clamp01(v) return math.max(0.0, math.min(1.0, v)) end
local function comp(id, name) return entity_utils.get_component(id, name) end
local function lerp(a, b, t) return a + (b - a) * t end
local function rand(lo, hi) return lo + (hi - lo) * math.random() end
local function vec(t) return vec3.new(t.x, t.y, t.z) end
local function copy3(v) return { x = v.x, y = v.y, z = v.z } end

-- Where an entity is in the world. A toy rides a hand or a mouth as a child,
-- and its own translation is then relative to that parent.
local function where(id) return entity_utils.get_world_translation(id) end

local function dist2(a, b)
    local dx, dz = a.x - b.x, a.z - b.z
    return math.sqrt(dx * dx + dz * dz)
end

local function dist3(a, b)
    local dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

-- The dog's heading about world up (glm Euler folds yaw past +-90 degrees).
local function yawOf(id)
    local r = entity_utils.get_rotation(id)
    if math.abs(r.x) > 1.5 and math.abs(r.z) > 1.5 then return math.pi - r.y end
    return r.y
end

local function wrap(a) return (a + math.pi) % (2 * math.pi) - math.pi end

-- Where the dog's head is: the mouth socket's place in the body's frame,
-- taken once at rest and carried by the body. The look swings round this
-- point; swung round the body's middle (the root, 0.4 m behind the head) a
-- look point half a metre out sweeps past the head several times as fast.
local function headAt(id, s)
    local here = entity_utils.get_translation(id)
    local o = s.headOffset
    if o == nil then return { x = here.x, y = here.y + 0.55, z = here.z } end
    local yaw = yawOf(id)
    local c, sn = math.cos(yaw), math.sin(yaw)
    return { x = here.x + o.x * c + o.z * sn, y = here.y + o.y, z = here.z - o.x * sn + o.z * c }
end

local function sampleHead(id, s)
    if s.headOffset or s.ballSocket == nil or s.ballSocket == 0 then return end
    local here = entity_utils.get_translation(id)
    local mouth = entity_utils.get_world_translation(s.ballSocket)
    local dx, dy, dz = mouth.x - here.x, mouth.y - here.y, mouth.z - here.z
    local reach = math.sqrt(dx * dx + dy * dy + dz * dz)
    if reach < 0.15 or reach > 2.0 then return end -- not posed yet
    local yaw = yawOf(id)
    local c, sn = math.cos(yaw), math.sin(yaw)
    s.headOffset = { x = dx * c - dz * sn, y = dy, z = dx * sn + dz * c }
end

-- Bearing of a world point from the dog, relative to where it faces.
local function bearing(id, p)
    local here = entity_utils.get_translation(id)
    return wrap(math.atan(p.x - here.x, p.z - here.z) - yawOf(id))
end

local function play(s, clip, loop, blend, speed)
    if not s.clips[clip] then
        if not s.warned[clip] then
            s.warned[clip] = true
            Log.Warn("[DogPlay] the model has no '" .. clip .. "' clip: that behaviour is skipped")
        end
        return false
    end
    if s.clip ~= clip or s.loop ~= loop then
        s.anim.blendDuration = blend or BLEND_SOFT
        s.anim:PlayClip(clip, loop)
        s.clip, s.loop = clip, loop
    end
    s.anim.playbackSpeed = speed or 1.0
    return true
end

local function clipDone(s, clip)
    return s.anim.currentClip == clip and s.anim.isFinished
end

local function clipAt(s, clip, t)
    return s.anim.currentClip == clip and s.anim.normalizedTime >= t
end

-- Turn toward a world point on the spot; true once facing it.
local function face(id, s, target, dt, stepping)
    local yaw = yawOf(id)
    local delta = bearing(id, target)
    if not stepping then
        -- A fine turn (lining the mouth up with a toy): continuous, no dead band.
        local step = math.max(-TURN_RATE * dt, math.min(TURN_RATE * dt, delta))
        entity_utils.set_rotation(id, vec3.new(0.0, yaw + step, 0.0))
        return math.abs(delta - step) < 0.1
    end
    if math.abs(delta) > TURN_START then s.turning = true
    elseif math.abs(delta) < TURN_STOP then s.turning = false end
    if not s.turning then
        return true
    end
    local step = math.max(-TURN_RATE * dt, math.min(TURN_RATE * dt, delta))
    entity_utils.set_rotation(id, vec3.new(0.0, yaw + step, 0.0))
    play(s, "TurnInPlace", true, BLEND_SOFT, 0.75)
    return false
end

local function playerPos(s)
    return s.camera and entity_utils.get_translation(s.camera) or s.home
end

local function playerFeet(s)
    local cam = playerPos(s)
    local focus = s.focus and entity_utils.get_translation(s.focus) or s.home
    local dx, dz = focus.x - cam.x, focus.z - cam.z
    local len = math.sqrt(dx * dx + dz * dz)
    if len < 1e-3 then return copy3(s.home) end
    return { x = cam.x + dx / len * FEET_AHEAD, y = s.home.y, z = cam.z + dz / len * FEET_AHEAD }
end

local function socketFor(s, toy)
    local n = entity_utils.get_name(toy) or ""
    if string.find(n, "Stick") and s.stickSocket then return s.stickSocket end
    return s.ballSocket
end

local function speedOf(toy)
    local rb = comp(toy, "Rigidbody3DComponent")
    if rb == nil then return 0 end
    local v = rb.linearVelocity
    return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
end

local function landed(toy)
    return #Physics.GetContacts(toy) > 0 or speedOf(toy) < 0.4
end

local function petting(id)
    local p = PlayBus and PlayBus.petting
    if p and p.dog == id then return p end
    return nil
end

local function favourite(p)
    if p.region == "Ears" then return true end
    return (p.region == "Neck" or p.region == "Torso") and p.ny < -0.25
end

local function reachForward(s)
    if s.reach == nil then return 0.6 end
    local r = entity_utils.get_translation(s.reach)
    return math.sqrt(r.x * r.x + r.z * r.z)
end

-- Lying still on the lawn: not in a hand or a mouth (both are on the bus), not in flight.
local function onTheLawn(toy, bus)
    return toy ~= bus.held and toy ~= bus.inMouth and speedOf(toy) < 0.4
end

-- The nearest toy lying on the lawn, away from the player's feet, the dog could
-- fetch on its own.
local function freeToy(s, bus)
    local best, bestD = nil, 9.0
    local here = entity_utils.get_translation(s.id)
    for _, toy in ipairs(s.toys) do
        if onTheLawn(toy, bus) then
            local p = where(toy)
            local d = dist2(here, p)
            if d < bestD and dist2(p, playerFeet(s)) > 1.2 then best, bestD = toy, d end
        end
    end
    return best
end

-- ---------------------------------------------------------------- the mood

local function applyConfiguredMood(s)
    local m = PlayConfig and PlayConfig.mood
    if m and m ~= s.moodSource then
        s.moodSource = m
        s.mood = { energy = m.energy or MOOD_DEFAULT.energy, excitement = m.excitement or MOOD_DEFAULT.excitement,
                   affection = m.affection or MOOD_DEFAULT.affection, curiosity = m.curiosity or MOOD_DEFAULT.curiosity }
        s.decideIn = 0.0
        if PlayConfig.seed then math.randomseed(PlayConfig.seed) end
    end
end

local function updateMood(s, dt, bus, pet)
    s.clock = s.clock + dt
    local m = s.mood
    local running = s.clip == "Run"
    local lyingDown = s.posture == "lying"
    if running then m.energy = m.energy + ENERGY_RUN * dt
    elseif s.clip == "Trot" or s.clip == "Carry" then m.energy = m.energy + ENERGY_TROT * dt
    elseif lyingDown then m.energy = m.energy + ENERGY_REST * dt
    else m.energy = m.energy + ENERGY_IDLE * dt end

    -- Excitement drifts back toward a calm level; play lifts it.
    local calm = 0.25
    m.excitement = m.excitement + (calm - m.excitement) * math.min(1.0, EXCITE_DECAY * dt * 4.0)
    if bus.held then m.excitement = m.excitement + 0.35 * dt end
    if pet then m.excitement = m.excitement + 0.08 * dt end

    -- Company: wanted more the longer nobody plays; petting and play satisfy it.
    local engaged = pet ~= nil or bus.held ~= nil or (bus.cursor ~= nil and bus.cursor.overDog)
    if pet then m.affection = m.affection + AFFECTION_PETTED * dt
    elseif engaged then m.affection = m.affection - 0.04 * dt
    else m.affection = m.affection + AFFECTION_GROW * dt end

    if s.action == "attend" or s.action == "sit" then m.curiosity = m.curiosity + CURIOSITY_GROW * dt end

    m.energy, m.excitement = clamp01(m.energy), clamp01(m.excitement)
    m.affection, m.curiosity = clamp01(m.affection), clamp01(m.curiosity)
end

-- ---------------------------------------------------------------- attention

-- What the dog looks at this frame, and how strongly: a world point, a
-- weight, whether only the head turns to it (a glance, or a hand on the dog
-- itself: a dog does not walk round after a hand on its back), and the entity
-- to lean into -- or nil (look where the body faces).
local function pickAttention(id, s, bus, pet, dt)
    local head = headAt(id, s)

    -- Actions that pose the head themselves.
    if s.state == "pickup" or s.state == "drop" then return nil end
    local a = s.action
    if a == "bellyup" or a == "sniff" or a == "shake" or a == "stretch" or a == "nudge" or s.phase == "liedown"
        or s.phase == "getup" or s.phase == "scratch" then
        return nil
    end
    if pet then
        -- A hand on the head: it looks up at whoever pets it, holding its head
        -- still in the hand. Anywhere else: it turns into the hand.
        if pet.region == "Head" or pet.region == "Ears" then
            return copy3(playerPos(s)), pet.fav and 0.85 or 0.6, true
        end
        return { x = pet.x, y = pet.y, z = pet.z }, pet.fav and 0.85 or 0.6, true, pet.hand
    end
    if s.state == "watch" or s.state == "chase" then return copy3(where(s.toy)), 0.9 end
    if s.state == "carry" then return copy3(playerPos(s)), 0.35 end
    if bus.held then return copy3(where(bus.held)), 0.85 end

    local cursor = bus.cursor
    if cursor and cursor.overDog then
        if cursor.region == "Head" or cursor.region == "Ears" then return copy3(playerPos(s)), 0.6, true end
        return { x = cursor.x, y = cursor.y, z = cursor.z }, 0.8, true
    end
    if cursor and dist3(head, cursor) < NOTICE_HAND then
        return { x = cursor.x, y = cursor.y, z = cursor.z }, 0.6
    end

    -- Otherwise the player, with a glance elsewhere now and then.
    s.glanceIn = (s.glanceIn or 2.0) - dt
    if s.glanceIn <= 0.0 then
        if s.glance then
            s.glance = nil
            s.glanceIn = rand(GLANCE_EVERY[1], GLANCE_EVERY[2])
        else
            local yaw = yawOf(id) + rand(-1.4, 1.4)
            local r = rand(1.5, 5.0)
            s.glance = { x = head.x + math.sin(yaw) * r, y = s.home.y + rand(0.0, 0.6), z = head.z + math.cos(yaw) * r }
            s.glanceIn = rand(GLANCE_FOR[1], GLANCE_FOR[2])
        end
    end
    if s.glance then return s.glance, 0.55, true end
    return copy3(playerPos(s)), 0.5
end

local function updateAttention(id, s, bus, pet, dt)
    if s.ik == nil then return end
    -- The head's place is read off the clip's pose, before any look bends it.
    if (s.weight or 0.0) < 0.02 then sampleHead(id, s) end
    local target, weight, headOnly, leanInto = pickAttention(id, s, bus, pet, dt)
    if target then
        -- Outside the head's reach the head stops at the edge, and a standing
        -- free dog turns its body toward it (updateFree reads s.wantFace).
        local b = bearing(id, target)
        s.wantFace = (not headOnly and math.abs(b) > BODY_FOLLOW) and target or nil
        -- The look is held as the head's own angle off the body's forward, its
        -- distance and its height, and eased in those, never as a world point:
        -- a world point left on one side while the body turned the other way
        -- drifted round behind the dog and the neck twisted after it. Out of
        -- the head's reach the head stops at the edge, looking as far round as
        -- it can; swapping sides it swings through the front, never the back.
        -- Passing behind the dog the bearing flips from +pi to -pi: the head
        -- keeps the side it was on until the target has really come round.
        -- The head's own bearing to the target, from the head: the body's (b,
        -- from its middle) decides only whether the body turns.
        local head = headAt(id, s)
        local want = wrap(math.atan(target.x - head.x, target.z - head.z) - yawOf(id))
        if math.abs(want) > HEAD_YAW_LIMIT then
            local side = want > 0 and 1 or -1
            if s.clampSide and side ~= s.clampSide and math.abs(want) > 2.6 then side = s.clampSide end
            s.clampSide = side
            want = side * HEAD_YAW_LIMIT
        else
            s.clampSide = nil
        end
        -- Too close to the head to aim at: pushed out to LOOK_MIN_DISTANCE; right
        -- over the head, the angle it already had.
        local flat = dist2(head, target)
        local wantDist = math.max(flat, LOOK_MIN_DISTANCE)
        local wantPitch = math.atan(target.y - head.y, math.max(flat, 0.1))
        wantPitch = math.max(-HEAD_PITCH_LIMIT, math.min(HEAD_PITCH_LIMIT, wantPitch))
        if flat < 0.05 and s.lookYaw then want, wantPitch = s.lookYaw, s.lookPitch end
        if s.lookYaw == nil then
            s.lookYaw, s.lookPitch, s.lookDist = want, wantPitch, wantDist
        end
        -- Both angles ease, and neither swings faster than HEAD_TURN_SPEED.
        local ease = math.min(1.0, dt * HEAD_EASE)
        local cap = HEAD_TURN_SPEED * dt
        s.lookYaw = s.lookYaw + math.max(-cap, math.min(cap, (want - s.lookYaw) * ease))
        s.lookYaw = math.max(-HEAD_YAW_LIMIT, math.min(HEAD_YAW_LIMIT, s.lookYaw))
        s.lookPitch = s.lookPitch + math.max(-cap, math.min(cap, (wantPitch - s.lookPitch) * ease))
        s.lookDist = s.lookDist + (wantDist - s.lookDist) * ease
        local dir = yawOf(id) + s.lookYaw
        s.look = { x = head.x + math.sin(dir) * s.lookDist, y = head.y + math.tan(s.lookPitch) * s.lookDist,
                   z = head.z + math.cos(dir) * s.lookDist }
        -- Petted on the body: lean into the hand itself, which rides the coat.
        s.ik.aimTargetEntity = leanInto or 0
        s.ik.aimTarget = vec(s.look)
        s.attention = target
    else
        s.wantFace = nil
        s.attention = nil
        weight = 0.0
        -- While a clip poses the head, the look drifts back to straight ahead,
        -- where that clip holds it: attention coming back then starts from
        -- there, not from an angle left over from before the body turned (the
        -- head flicked to the old side and back, or carried the toy home
        -- looking sideways).
        if s.lookYaw then
            local cap = HEAD_TURN_SPEED * dt
            s.lookYaw = s.lookYaw - math.max(-cap, math.min(cap, s.lookYaw))
            s.lookPitch = s.lookPitch - math.max(-cap, math.min(cap, s.lookPitch))
        end
    end
    s.lookWeight = weight
    -- The head hands over between the clip's pose and the look over about a
    -- second; at 5/s it snapped across in a fifth of one.
    local k = math.min(1.0, dt * LOOK_WEIGHT_RATE)
    s.weight = s.weight + (s.lookWeight - s.weight) * k
    s.ik.aimWeight = s.weight
end

-- ---------------------------------------------------------------- free time

local function setAction(s, name)
    s.action = name
    s.actionTime = 0.0
    s.phase = nil
    if s.following then
        s.following = false
        s.nav:clearTarget()
    end
    s.lastDone[name] = s.clock
end

-- Whether an idle habit is due again (see COOLDOWN).
local function due(s, name)
    local last = s.lastDone[name]
    return last == nil or s.clock - last >= (COOLDOWN[name] or 0.0)
end

-- From lying, get up first; true once standing.
local function standUp(s)
    if s.posture == "lying" then
        if s.phase ~= "getup" then
            s.phase = "getup"
            if not play(s, "GetUp", false, BLEND_SOFT) then
                s.posture, s.phase = "standing", nil
                return true
            end
        end
        if clipDone(s, "GetUp") then
            s.posture = "standing"
            s.phase = nil
            return true
        end
        return false
    end
    s.posture = "standing"
    return true
end

local function decide(id, s, bus)
    local m = s.mood
    local here = entity_utils.get_translation(id)
    local toPlayer = dist2(here, playerFeet(s))
    local cursor = bus.cursor
    local handNear = cursor ~= nil and dist3(headAt(id, s), cursor) < NOTICE_HAND

    if m.energy < 0.22 then return "rest" end
    if bus.held then return "invite" end
    if m.affection > 0.62 and toPlayer > 1.2 then return "approach" end
    if m.affection > 0.55 and handNear and due(s, "nudge") then return "nudge" end
    -- A hand coming near has its whole attention.
    if handNear then return "attend" end
    if m.excitement > 0.75 and m.energy > 0.6 and due(s, "zoomies") and math.random() < 0.25 then return "zoomies" end
    local mayFetch = not (PlayConfig and PlayConfig.fetchDisabled)
    if mayFetch and m.excitement > 0.5 and m.energy > 0.5 and m.affection > 0.4 and freeToy(s, bus) then
        return "bringtoy"
    end
    if m.curiosity > 0.7 and m.energy > 0.35 and due(s, "sniff") then return "sniff" end
    if m.energy < 0.45 then return (math.random() < 0.5 and due(s, "sit")) and "sit" or "rest" end
    local r = math.random()
    if r < 0.12 and due(s, "stretch") then return "stretch" end
    if r < 0.2 and due(s, "shake") then return "shake" end
    if r < 0.35 and due(s, "sit") then return "sit" end
    return "attend"
end

-- Walk somewhere on the navmesh; the clip's root motion carries the dog, so a
-- model without the gait falls back to Walk (play() has said so).
local function goTo(s, p, clip, speed)
    s.nav.targetPosition = vec3.new(p.x, s.home.y, p.z)
    if not play(s, clip, true, BLEND_SOFT, speed) then play(s, "Walk", true, BLEND_SOFT, speed) end
end

local function startChase(id, s, toy)
    s.toy = toy
    s.state = "chase"
    s.timer = 0.0
    play(s, "Run", true, BLEND_SOFT)
end

-- One frame of free time.
local function updateFree(id, s, bus, dt)
    s.actionTime = (s.actionTime or 0.0) + dt
    s.decideIn = (s.decideIn or 0.0) - dt
    local m = s.mood
    local here = entity_utils.get_translation(id)

    local a = s.action
    -- A toy in the player's hand is news: idle habits give way to it at once.
    if bus.held and not s.sawHeld then
        s.sawHeld = true
        if a ~= "invite" and a ~= "bringtoy" and a ~= "rest" then
            s.interruptible = true
            s.decideIn = 0.0
        end
    elseif not bus.held then
        s.sawHeld = false
    end
    if a == nil or (s.interruptible and s.decideIn <= 0.0) then
        local nextAction = decide(id, s, bus)
        if nextAction ~= a then
            if s.posture == "lying" and nextAction ~= "rest" and not standUp(s) then return end
            setAction(s, nextAction)
            a = nextAction
        end
        s.decideIn = rand(2.5, 5.0)
    end
    s.interruptible = true
    PlayBus.dogAction = a

    if a == "attend" then
        -- Standing with the player in view; the head does the attending. Turn to
        -- keep what it watches within reach of the head.
        s.wantFaceFor = s.wantFace and (s.wantFaceFor or 0.0) + dt or 0.0
        if s.wantFace and s.wantFaceFor > TURN_AFTER then
            face(id, s, s.wantFace, dt, true)
        elseif s.clip ~= "HeadTilt" or clipDone(s, "HeadTilt") then
            play(s, m.energy < 0.6 and "Pant" or "Idle", true, BLEND_SOFT, lerp(0.85, 1.25, m.excitement))
        end
        -- A curious head tilt at a hand that moves about in front of it.
        if bus.cursor and bus.cursor.moved and s.attention and math.random() < dt * 0.4 then
            play(s, "HeadTilt", false, BLEND_SOFT)
        end
    elseif a == "invite" then
        -- A toy held up (or one just dropped at the player's feet): bounce, bow.
        if not bus.held and s.actionTime > 6.0 then setAction(s, "attend"); return end
        -- The body keeps to the toy as the player moves it: stepping round when
        -- it is well off to one side, easing round without steps when it is near
        -- in front, and bouncing or bowing while it watches.
        -- A player walking off with the toy is followed: the dog trots (runs,
        -- when it is far behind) after them and stops a step short.
        local feet = playerFeet(s)
        local gap = dist2(here, feet)
        if bus.held and (gap > FOLLOW_FAR or (s.following and gap > FOLLOW_NEAR)) then
            if not s.following or s.actionTime - (s.retarget or -1) > 0.3 or not s.nav.hasTarget then
                goTo(s, feet, gap > FOLLOW_RUN and "Run" or "Trot")
                s.retarget = s.actionTime
            end
            s.following = true
            s.turning = false
            return
        elseif s.following then
            s.following = false
            s.nav:clearTarget()
        end
        local target = bus.held and where(bus.held) or playerPos(s)
        if not face(id, s, target, dt, true) then return end
        if m.excitement > 0.6 then play(s, "Excited", true, BLEND_QUICK, lerp(0.9, 1.3, m.excitement))
        else play(s, "PlayBow", true, BLEND_SOFT) end
    elseif a == "approach" then
        if not standUp(s) then return end
        s.interruptible = false
        local feet = playerFeet(s)
        if dist2(here, feet) > 0.6 then
            if s.actionTime - (s.retarget or -1) > 0.3 or not s.nav.hasTarget then
                goTo(s, feet, "Trot")
                s.retarget = s.actionTime
            end
        else
            s.nav:clearTarget()
            setAction(s, (m.affection > 0.5) and "nudge" or "attend")
        end
        if s.actionTime > 15.0 then s.nav:clearTarget(); setAction(s, "attend") end
    elseif a == "nudge" then
        if not standUp(s) then return end
        -- Into the hand when it is near, else up at the player: never toward a
        -- cursor pointing at the sky, which lies far behind the dog.
        local cursor = bus.cursor
        local head = headAt(id, s)
        local target = (cursor and dist3(head, cursor) < NOTICE_HAND) and cursor or copy3(playerPos(s))
        if s.phase == nil and not face(id, s, target, dt, true) then return end
        s.interruptible = false
        if s.phase == nil then
            s.phase = "nudge"
            if not play(s, "Nudge", false, BLEND_QUICK) then setAction(s, "attend"); return end
        elseif clipDone(s, "Nudge") then
            m.affection = clamp01(m.affection - 0.15)
            setAction(s, "attend")
        end
    elseif a == "zoomies" then
        if not standUp(s) then return end
        s.interruptible = false
        if s.phase == nil then
            s.phase = 1
            s.loopPoints = {}
            for i = 1, 4 do
                local ang = (i / 4) * 2 * math.pi + rand(-0.3, 0.3)
                s.loopPoints[i] = { x = s.home.x + math.sin(ang) * 3.0, z = s.home.z + math.cos(ang) * 3.0 }
            end
        end
        if s.phase <= 4 and dist2(here, s.loopPoints[s.phase]) < 0.8 then s.phase = s.phase + 1 end
        if s.phase > 4 or s.actionTime > 8.0 then
            s.nav:clearTarget()
            m.excitement = clamp01(m.excitement - 0.3)
            setAction(s, bus.held and "invite" or "attend")
            return
        end
        if s.actionTime - (s.retarget or -1) > 0.3 or not s.nav.hasTarget then
            goTo(s, s.loopPoints[s.phase], "Run")
            s.retarget = s.actionTime
        end
    elseif a == "bringtoy" then
        if not standUp(s) then return end
        local toy = freeToy(s, bus)
        if toy == nil then setAction(s, "attend"); return end
        startChase(id, s, toy)
    elseif a == "sniff" then
        if not standUp(s) then return end
        s.interruptible = s.actionTime > 2.0
        if s.phase == nil then
            local ang = rand(0, 2 * math.pi)
            local r = rand(1.0, 3.5)
            s.sniffTo = { x = s.home.x + math.sin(ang) * r, z = s.home.z + math.cos(ang) * r }
            s.phase = "go"
        end
        if s.phase == "go" then
            if dist2(here, s.sniffTo) > 0.4 and s.actionTime < 12.0 then
                if s.actionTime - (s.retarget or -1) > 0.5 or not s.nav.hasTarget then
                    goTo(s, s.sniffTo, "Sniff")
                    s.retarget = s.actionTime
                end
            else
                s.nav:clearTarget()
                s.phase = "look"
                s.lookUntil = s.actionTime + rand(1.0, 2.0)
                m.curiosity = 0.1
                play(s, "HeadTilt", false, BLEND_SOFT)
            end
        elseif s.actionTime > (s.lookUntil or 0.0) then
            setAction(s, "attend")
        end
    elseif a == "sit" then
        if not standUp(s) then return end
        if s.phase == nil then
            s.phase = "sit"
            play(s, "Sit", false, BLEND_SOFT)
            s.sitFor = rand(5.0, 12.0)
        elseif s.phase == "sit" and clipDone(s, "Sit") and math.random() < dt * 0.15 and s.clips["Scratch"] then
            s.phase = "scratch"
            play(s, "Scratch", false, BLEND_QUICK)
        elseif s.phase == "scratch" and clipDone(s, "Scratch") then
            s.phase = "sat"
            play(s, "Sit", false, BLEND_QUICK)
        end
        if s.actionTime > (s.sitFor or 8.0) then setAction(s, "attend") end
    elseif a == "stretch" or a == "shake" then
        if not standUp(s) then return end
        s.interruptible = false
        local clip = (a == "stretch") and "Stretch" or "Shake"
        if s.phase == nil then
            s.phase = clip
            if not play(s, clip, false, BLEND_SOFT) then setAction(s, "attend"); return end
        elseif clipDone(s, clip) then
            setAction(s, "attend")
        end
    elseif a == "rest" then
        -- Lie down and recover; up again when rested, or when something fun happens.
        if s.posture ~= "lying" then
            s.interruptible = false
            if s.phase ~= "liedown" then
                s.phase = "liedown"
                if not play(s, "LieDown", false, BLEND_SOFT) then s.posture, s.phase = "lying", nil end
            elseif clipDone(s, "LieDown") then
                s.posture = "lying"
                s.phase = nil
                play(s, "Lie", true, BLEND_QUICK)
            end
            return
        end
        play(s, "Lie", true, BLEND_SOFT)
        s.interruptible = m.energy > 0.7 or bus.held ~= nil or m.affection > 0.75
    end
end

-- ---------------------------------------------------------------- fetch

-- The fetch chain (also a toy the dog fetches on its own): watch, chase, pick
-- up, carry to the player's feet, drop. Returns true while it runs.
local function updateFetch(id, s, bus, dt)
    if s.state == "free" then return false end
    s.timer = s.timer + dt
    local here = entity_utils.get_translation(id)
    if s.state == "watch" then
        if PlayConfig and PlayConfig.fetchDisabled then
            if landed(s.toy) and s.timer > 1.0 then s.state = "free"; s.toy = nil end
        elseif s.mood.energy < 0.12 then
            -- Too tired to run after it: watches it land, and lies down.
            if landed(s.toy) and s.timer > 1.0 then s.state = "free"; s.toy = nil; setAction(s, "rest") end
        elseif landed(s.toy) and s.timer > 0.15 and standUp(s) then
            startChase(id, s, s.toy)
        end
    elseif s.state == "chase" then
        local toy = where(s.toy)
        local reach = reachForward(s)
        local d = dist2(here, toy)
        local ux, uz = (toy.x - here.x) / math.max(d, 1e-3), (toy.z - here.z) / math.max(d, 1e-3)
        local goal = vec3.new(toy.x - ux * reach, here.y, toy.z - uz * reach)
        if s.timer - (s.retarget or -1) > 0.25 or not s.nav.hasTarget then
            s.nav.targetPosition = goal
            s.retarget = s.timer
        end
        if d < reach + STOP_EXTRA then
            s.nav:clearTarget()
            s.attached = false
            s.state = "pickup"
            play(s, "PickUp", false, BLEND_QUICK)
        elseif s.timer > CHASE_GIVE_UP or s.nav.targetUnreachable then
            s.nav:clearTarget()
            s.state = "free"
            setAction(s, "attend")
        end
    elseif s.state == "pickup" then
        face(id, s, where(s.toy), dt, false)
        if not s.attached and clipAt(s, "PickUp", GRAB_AT) then
            local m = where(s.toy)
            local r = entity_utils.get_translation(s.reach or id)
            local rot = yawOf(id)
            local mx = here.x + math.sin(rot) * r.z + math.cos(rot) * r.x
            local mz = here.z + math.cos(rot) * r.z - math.sin(rot) * r.x
            if math.sqrt((m.x - mx) ^ 2 + (m.z - mz) ^ 2) <= GRAB_REACH and
                entity_utils.attach_to(s.toy, socketFor(s, s.toy), "", vec3.new(0, 0, 0)) then
                s.attached = true
                bus.inMouth = s.toy
            else
                startChase(id, s, s.toy)
            end
        end
        if s.attached and clipDone(s, "PickUp") then
            s.state = "carry"
            s.timer = 0.0
            play(s, "Carry", true, BLEND_SOFT)
        end
    elseif s.state == "carry" then
        local feet = playerFeet(s)
        if s.timer - (s.retarget or -1) > 0.25 or not s.nav.hasTarget then
            s.nav.targetPosition = vec3.new(feet.x, here.y, feet.z)
            s.retarget = s.timer
        end
        if dist2(here, feet) < RETURN_STOP + STOP_EXTRA then
            s.nav:clearTarget()
            s.state = "drop"
            s.dropping = false
            play(s, "Idle", true, BLEND_QUICK)
        end
    elseif s.state == "drop" then
        local facing = face(id, s, playerPos(s), dt, true)
        if facing and not s.dropping then
            s.dropping = true
            play(s, "Drop", false, BLEND_QUICK)
        end
        if s.dropping and clipAt(s, "Drop", RELEASE_AT) and s.toy then
            entity_utils.detach(s.toy)
            bus.inMouth = nil
            bus.returned = { toy = s.toy, x = here.x, z = here.z }
            s.toy = nil
            s.mood.excitement = clamp01(s.mood.excitement + 0.25)
            s.mood.affection = clamp01(s.mood.affection - 0.1)
        end
        if s.dropping and s.toy == nil and clipDone(s, "Drop") then
            -- Waits for the next throw, bouncing for it.
            s.state = "free"
            setAction(s, "invite")
            s.decideIn = rand(3.0, 5.0)
        end
    end
    return true
end

-- ---------------------------------------------------------------- the frame

local function update(id, s, dt)
    local bus = PlayBus or {}
    local pet = petting(id)
    local carrying = s.state == "carry" or s.state == "drop" or (s.state == "pickup" and s.attached)

    applyConfiguredMood(s)
    updateMood(s, dt, bus, pet)
    if pet then pet.fav = favourite(pet) end
    updateAttention(id, s, bus, pet, dt)

    -- Petting: everything waits but a toy already in the mouth.
    if pet and not carrying then
        if s.state ~= "free" then
            s.nav:clearTarget()
            s.state = "free"
        end
        s.petTime = (s.petTime or 0.0) + dt
        s.petLeft = PET_LINGER
        bus.reaction = { region = pet.region, favourite = pet.fav }
        if s.posture == "lying" or s.action == "bellyup" then
            -- Lying down and stroked: rolls over for a belly rub.
            if s.action ~= "bellyup" then setAction(s, "bellyup"); s.phase = "roll" end
            if s.phase == "roll" and (not play(s, "RollOver", false, BLEND_SOFT) or clipDone(s, "RollOver")) then
                s.phase = "belly"
            end
            if s.phase == "belly" then play(s, "BellyUp", true, BLEND_SOFT, pet.fav and FAVOURITE_SPEED or 1.0) end
        else
            if s.action ~= "petted" then setAction(s, "petted") end
            play(s, "Petted", true, BLEND_SOFT, pet.fav and FAVOURITE_SPEED or 1.0)
        end
        PlayBus.dogAction = s.action
        return
    end
    if s.action == "petted" or s.action == "bellyup" then
        s.petLeft = (s.petLeft or 0.0) - dt
        if s.petLeft > 0 then return end
        local petFor = s.petTime or 0.0
        s.petTime = 0.0
        if s.action == "bellyup" then
            -- Stays rolled over a moment, hoping for more, then rests.
            s.posture = "lying"
            setAction(s, "rest")
            s.decideIn = rand(2.0, 4.0)
        elseif petFor > 4.0 and math.random() < 0.6 then
            setAction(s, "shake")   -- a good shake after a long fuss
        elseif petFor < 2.0 and s.mood.affection > 0.4 then
            setAction(s, "nudge")   -- that was too short: more, please
        else
            setAction(s, "attend")
        end
        s.decideIn = math.max(s.decideIn or 0.0, 1.0)
    end

    -- A throw: watch it, then fetch it.
    local thrown = bus.thrown
    if thrown and (s.state == "free" or s.state == "watch") and not carrying then
        bus.thrown = nil
        s.toy = thrown.toy
        s.state = "watch"
        s.timer = 0.0
        s.mood.excitement = clamp01(s.mood.excitement + 0.3)
        if s.posture ~= "lying" then play(s, "Idle", true, BLEND_QUICK) end
    end

    if updateFetch(id, s, bus, dt) then
        PlayBus.dogAction = s.state
        return
    end
    updateFree(id, s, bus, dt)
end

-- ---------------------------------------------------------------- lifecycle

function DogPlay.OnCreate(id)
    PlayBus = {} -- a fresh game: one shared table, reset with the dog
    local p = entity_utils.get_translation(id)
    local s = {
        id = id,
        anim = comp(id, "AnimationStateComponent"),
        nav = comp(id, "NavAgentComponent"),
        ik = comp(id, "IKTargetComponent"),
        ballSocket = entity_utils.find_by_name("MouthSocketBall"),
        stickSocket = entity_utils.find_by_name("MouthSocketStick"),
        reach = entity_utils.find_by_name("PickUpReach"),
        camera = entity_utils.find_by_name("RuntimeCamera"),
        focus = entity_utils.find_by_name("PlayFocus"),
        home = { x = p.x, y = p.y, z = p.z },
        toys = {},
        mood = { energy = MOOD_DEFAULT.energy, excitement = MOOD_DEFAULT.excitement,
                 affection = MOOD_DEFAULT.affection, curiosity = MOOD_DEFAULT.curiosity },
        clips = {}, warned = {}, lastDone = {}, clock = 0.0,
        state = "free", posture = "standing", weight = 0.0, lookWeight = 0.0, timer = 0.0,
    }
    for _, name in ipairs({ "ToyBall", "ToyStick" }) do
        local toy = entity_utils.find_by_name(name)
        if toy then s.toys[#s.toys + 1] = toy end
    end
    if s.anim then
        for _, name in ipairs(s.anim:GetClipNames()) do s.clips[name] = true end
    end
    math.randomseed((PlayConfig and PlayConfig.seed) or 1559)
    state[id] = s
    if s.anim == nil or s.nav == nil or s.ballSocket == nil then
        Log.Warn("[DogPlay] needs an AnimationStateComponent, a NavAgentComponent and a MouthSocketBall child: the dog will only idle")
    end
    if s.ik then
        local skeleton = comp(id, "SkeletonComponent")
        local head = skeleton and skeleton:GetBoneIndex("head") or -1
        if head >= 0 then
            s.ik.aimBoneIndex = head
            s.ik.aimIKEnabled = true
        else
            Log.Warn("[DogPlay] no 'head' bone: the dog will not look at things")
            s.ik.aimIKEnabled = false
        end
        s.ik.aimWeight = 0.0
    end
end

function DogPlay.OnUpdate(id, dt)
    local s = state[id]
    if s == nil or s.anim == nil then return end
    if s.nav ~= nil and s.ballSocket ~= nil then
        update(id, s, dt)
    end
    PlayBus.dogState = s.state
    PlayBus.mood = s.mood
end

function DogPlay.OnDestroy(id)
    state[id] = nil
end

return DogPlay
