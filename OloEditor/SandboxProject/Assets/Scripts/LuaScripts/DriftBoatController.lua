-- DriftBoatController.lua — Drift (issue #879), the player's boat.
--
-- Attach to the boat ROOT (the entity carrying Rigidbody3D + Buoyancy + Boat)
-- via LuaScriptComponent, ScriptFile = "Scripts/LuaScripts/DriftBoatController.lua".
--
-- What this script owns and what it deliberately does not:
--
--   * It owns INPUT SHAPING only. Every hull number (thrust, rudder torque,
--     drag, immersion) is authored on BoatComponent in the scene, so the feel
--     pass is a scene edit and needs no rebuild. The script writes exactly two
--     fields — throttleInput and steerInput — and reads none of the others.
--   * It does NOT read key codes. Everything goes through InputActionManager
--     action names defined in <project>/Config/InputActions.yaml, which is what
--     makes the rebinding UI (#883) free later. Keyboard and gamepad both work
--     with no branch in here: a gamepad stick reports its analog deflection and
--     a key reports a full-scale press, through the same call.
--   * It owns the VISUAL heel. BoatSystem applies its hull drag at the centre
--     of mass (BoatSystem.cpp, the "Hull drag" block passes no application
--     point), so the physics hull cannot roll into a turn no matter how it is
--     tuned — the only roll a boat gets is from buoyancy reacting to waves.
--     Rolling the hull MESH, a child of the physics root, is what puts the boat
--     on its ear in a hard turn without touching the collider, the buoyancy
--     probe box or the camera (all of which read the root).
--
-- Two feel notes worth not re-deriving:
--
--   * Every smoothing step here is 1 - exp(-dt/tau), never a fixed per-frame
--     lerp factor. Scripts run inside the fixed-step tick, which runs 0..N
--     times per rendered frame, so a per-frame factor would make the boat
--     respond differently at different frame rates. The exponential form
--     composes exactly, so it does not.
--   * Throttle spools up slower than it spools down (kThrottleTauUp vs
--     kThrottleTauDown). A boat that answers the throttle instantly reads as a
--     car; a boat that also takes as long to slow down reads as broken. The
--     asymmetry is most of the difference between "heavy" and "unresponsive".

local BoatController = {}

-- ── Input shaping ───────────────────────────────────────────────────────────
local kThrottleTauUp     = 0.85   -- s, engine spooling up (deliberately slow)
local kThrottleTauDown   = 0.35   -- s, backing off
local kRudderTau         = 0.30   -- s, rudder slewing hard over
local kInputDeadzone     = 0.12   -- gamepad stick/trigger deadzone

-- Rudder authority is reduced at speed on top of BoatSystem's own
-- speed-proportional authority. Without this the boat is docile at harbour
-- speed and spins on its axis at full ahead, which is the single most common
-- way an arcade boat becomes a fight to steer.
local kSteerFalloffSpeed = 16.0   -- m/s at which the falloff reaches full effect
local kSteerFalloffMin   = 0.62   -- rudder command multiplier at/above that speed

-- ── Visual heel (hull mesh child only — see the header) ─────────────────────
local kHeelPerYawRate    = 0.45   -- radians of roll per rad/s of yaw rate
local kHeelMaxDeg        = 14.0
local kHeelTau           = 0.28   -- s, how fast the hull leans in and back out
local kHeelSpeedRef      = 6.0    -- m/s at which heel reaches full scale
local kHullChildName     = "Boat Hull"

-- ── Runtime state ───────────────────────────────────────────────────────────
local throttle   = 0.0
local steer      = 0.0
local heel       = 0.0
local prevYaw    = nil
local prevPos    = nil
local speed      = 0.0
local hullID     = nil
local warnedNoBoat = false

-- Exponential blend factor for a time constant, frame-rate independent.
local function blend(tau, dt)
    if tau <= 0.0 or dt <= 0.0 then return 1.0 end
    return 1.0 - math.exp(-dt / tau)
end

local function deadzone(v)
    local m = math.abs(v)
    if m <= kInputDeadzone then return 0.0 end
    -- Rescale so the first movement past the deadzone starts from zero rather
    -- than jumping to kInputDeadzone.
    return (m - kInputDeadzone) / (1.0 - kInputDeadzone)
end

-- Magnitude of a one-way action. A digital press reports +1 whichever direction
-- the binding names, so the caller applies the sign — never the returned value's.
local function axisMagnitude(actionName)
    return deadzone(Input.GetActionAxisValue(actionName))
end

local function wrapAngle(a)
    while a > math.pi do a = a - 2.0 * math.pi end
    while a < -math.pi do a = a + 2.0 * math.pi end
    return a
end

-- Compass heading of an entity's local +Z (the hull's forward, Jolt convention),
-- in radians, measured the same way atan2(x, z) measures it.
--
-- DO NOT use the euler triple's .y for this, however obviously "yaw" it looks.
-- TransformComponent stores a quaternion and derives the euler through
-- glm::eulerAngles, whose middle angle is confined to [-pi/2, pi/2]; a heading
-- past 90 degrees is therefore represented on the OTHER branch, as
-- (x +/- pi, pi - y, z +/- pi). The engine picks whichever branch is closest to
-- the previous frame's, so the triple stays continuous — but .y ON ITS OWN
-- mirrors and reverses sign at exactly the moment the boat turns hard, which is
-- exactly when this is being read. Measured here: on a hard turn the heel
-- flipped to the wrong side mid-turn, and the sampled .y read -1.54 rad while
-- the boat's own course over ground said it was past 90 degrees and still
-- swinging.
--
-- glm::quat(vec3) composes as Rx.Ry.Rz, so local +Z lands on
-- (sin y, -cos y * sin x, cos y * cos x). The cos y * cos x denominator is what
-- carries the mirrored branch: where |x| is near pi it goes negative and atan2
-- returns sign(y) * (pi - |y|), the correct continuation, instead of y.
--
-- The composition order was established by MEASUREMENT, not from the header:
-- the first attempt assumed Ry.Rx.Rz (which the follow-camera agent-rules doc's
-- pitch/yaw example implies) and produced (sin y * cos x, ..., cos y * cos x).
-- That agrees with this everywhere |x| is small and disagrees by a sign flip on
-- exactly the mirrored branch — i.e. it looks right in every gentle test and is
-- wrong precisely when it matters. Cross-checked against the boat's course over
-- ground on a logged hard turn.
local function headingOf(entityID)
    local e = entity_utils.get_rotation(entityID)
    local fx = math.sin(e.y)
    local fz = math.cos(e.y) * math.cos(e.x)
    if math.abs(fx) < 1e-9 and math.abs(fz) < 1e-9 then
        return nil -- hull pointing straight up or down: heading is undefined
    end
    return math.atan(fx, fz)
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

function BoatController.OnCreate(id)
    -- The boat's bindings live in the Vehicle context (Config/InputActions.yaml).
    -- Nothing else in this scene wants the Gameplay context, so a hard switch is
    -- correct here; a scene that layered a menu over the boat would push instead.
    Input.SetInputContext(InputContext.Vehicle)

    throttle, steer, heel = 0.0, 0.0, 0.0
    prevYaw, prevPos, speed = nil, nil, 0.0
    hullID = entity_utils.find_by_name(kHullChildName)

    Log.Info("[Drift] Boat ready — W/S throttle, A/D rudder, C look astern (gamepad: triggers + left stick).")
end

function BoatController.OnUpdate(id, dt)
    if dt <= 0.0 then return end

    local boat = entity_utils.get_component(id, "BoatComponent")
    if not boat then
        if not warnedNoBoat then
            warnedNoBoat = true
            Log.Warn("[Drift] DriftBoatController is attached to an entity with no BoatComponent.")
        end
        return
    end

    -- A context switch elsewhere (or a script reload mid-session) would silently
    -- leave every action reading zero, which looks exactly like broken input.
    if Input.GetInputContext() ~= InputContext.Vehicle then
        Input.SetInputContext(InputContext.Vehicle)
    end

    -- ── Speed, measured from the root's own travel ──────────────────────────
    -- There is no Lua accessor for a Jolt body's velocity, so differentiate the
    -- transform. Smoothed, because a one-tick difference in a swell is noisy.
    local pos = entity_utils.get_translation(id)
    if prevPos then
        local dx, dz = pos.x - prevPos.x, pos.z - prevPos.z
        local instant = math.sqrt(dx * dx + dz * dz) / dt
        speed = speed + (instant - speed) * blend(0.20, dt)
    end
    prevPos = pos

    -- ── Throttle ────────────────────────────────────────────────────────────
    local throttleTarget = axisMagnitude("Boat.ThrottleAhead") - axisMagnitude("Boat.ThrottleAstern")
    throttleTarget = clamp(throttleTarget, -1.0, 1.0)
    local tau = (math.abs(throttleTarget) > math.abs(throttle)) and kThrottleTauUp or kThrottleTauDown
    throttle = throttle + (throttleTarget - throttle) * blend(tau, dt)

    -- ── Rudder ──────────────────────────────────────────────────────────────
    local steerTarget = axisMagnitude("Boat.SteerStarboard") - axisMagnitude("Boat.SteerPort")
    steerTarget = clamp(steerTarget, -1.0, 1.0)
    steer = steer + (steerTarget - steer) * blend(kRudderTau, dt)

    local falloff = 1.0 - (1.0 - kSteerFalloffMin) * clamp(speed / kSteerFalloffSpeed, 0.0, 1.0)

    boat.throttleInput = clamp(throttle, -1.0, 1.0)

    -- NEGATED, and this is not a typo. BoatComponent documents m_SteerInput as
    -- "1 = full starboard" and BoatSystem applies +Y torque for it, but +Y takes
    -- the hull's +Z forward toward +X — and for a +Z-forward object in this
    -- right-handed Y-up frame, +X is to PORT as seen from a camera sitting
    -- behind it (forward x up = (-1,0,0)). Measured, not reasoned about after
    -- the fact: D held, heading went 0 -> +0.42 -> +1.12 rad and world x went
    -- 1.4 -> 9.5 -> 22, i.e. the boat left frame to the LEFT. So the player's
    -- "starboard" is a negative steerInput. Filed against the engine; flipped
    -- here because #879 is content work.
    boat.steerInput = clamp(-steer * falloff, -1.0, 1.0)

    -- ── Visual heel ─────────────────────────────────────────────────────────
    if not hullID then
        hullID = entity_utils.find_by_name(kHullChildName)
    end
    if hullID then
        local heading = headingOf(id)
        local yawRate = 0.0
        if heading and prevYaw then
            yawRate = wrapAngle(heading - prevYaw) / dt
        end
        prevYaw = heading or prevYaw

        -- Bank INTO the turn: yawing to starboard (negative yaw about +Y, since
        -- +Y torque yaws the bow to starboard in a right-handed frame with Z
        -- forward) should drop the starboard rail. A roll about the hull's +Z
        -- lifts starboard toward up, so the sign is negated once here.
        local speedScale = clamp(speed / kHeelSpeedRef, 0.0, 1.0)
        local maxHeel = math.rad(kHeelMaxDeg)
        local heelTarget = clamp(-yawRate * kHeelPerYawRate * speedScale, -maxHeel, maxHeel)
        heel = heel + (heelTarget - heel) * blend(kHeelTau, dt)

        local hullRot = entity_utils.get_rotation(hullID)
        hullRot.z = heel
        entity_utils.set_rotation(hullID, hullRot)
    end
end

function BoatController.OnDestroy(id)
    -- Hand the input context back so a scene switch doesn't inherit the boat's map.
    Input.SetInputContext(InputContext.Gameplay)
end

return BoatController
