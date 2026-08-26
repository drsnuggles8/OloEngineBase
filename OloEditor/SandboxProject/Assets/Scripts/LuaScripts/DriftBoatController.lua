-- DriftBoatController.lua — Drift (issues #879, #899), the player's boat.
--
-- Attach to the boat ROOT (the entity carrying Rigidbody3D + Buoyancy + Boat +
-- Sail) via LuaScriptComponent,
-- ScriptFile = "Scripts/LuaScripts/DriftBoatController.lua".
--
-- What this script owns and what it deliberately does not:
--
--   * It owns INPUT SHAPING only. Every hull and rig number (thrust, rudder
--     torque, drag, immersion, sail area, yard limits, centre of effort) is
--     authored in the scene, so the feel pass is a scene edit and needs no
--     rebuild. The script writes exactly three fields — throttleInput,
--     steerInput and sailSetInput — and reads none of the tuning.
--   * It does NOT read key codes. Everything goes through InputActionManager
--     action names defined in <project>/Config/InputActions.yaml, which is what
--     makes the rebinding UI (#883) free later. Keyboard and gamepad both work
--     with no branch in here: a gamepad stick reports its analog deflection and
--     a key reports a full-scale press, through the same call.
--   * It owns the SAIL'S POSE, but not its angle. SailSystem solves the yard
--     angle from the apparent wind and publishes it as SailComponent.yardAngle
--     (read-only from Lua, on purpose). All this script does is copy that onto
--     the sail entity's Y rotation, which is what makes the drawn sail and the
--     applied force the same thing rather than two things that agree by luck.
--
-- WHAT #899 REMOVED, and why it is worth knowing it is gone:
--
--   This script used to roll the hull MESH to fake heel, because BoatSystem
--   applies hull drag at the centre of mass and so the physics body could not
--   heel at all. The sail's force is applied at the centre of effort, well above
--   the centre of mass, so the body now heels for real, against buoyancy's
--   righting moment — and it heels to LEEWARD under wind pressure rather than
--   INTO the turn off yaw rate, which is a different motion and the correct one.
--   The fake, the yaw-rate differentiator and the euler-branch heading helper it
--   needed are all deleted. If a future change makes the boat stop heeling, the
--   thing to check is SailComponent.centreOfEffortY, not this file.
--
-- Two feel notes worth not re-deriving:
--
--   * Every smoothing step here is 1 - exp(-dt/tau), never a fixed per-frame
--     lerp factor. Scripts run inside the fixed-step tick, which runs 0..N times
--     per rendered frame, so a per-frame factor would make the boat respond
--     differently at different frame rates. The exponential form composes
--     exactly, so it does not.
--   * Throttle spools up slower than it spools down (kThrottleTauUp vs
--     kThrottleTauDown). A boat that answers the throttle instantly reads as a
--     car; a boat that also takes as long to slow down reads as broken. The
--     asymmetry is most of the difference between "heavy" and "unresponsive".

local BoatController = {}

-- ── Input shaping ───────────────────────────────────────────────────────────
local kThrottleTauUp     = 0.85   -- s, auxiliary engine spooling up
local kThrottleTauDown   = 0.35   -- s, backing off
local kRudderTau         = 0.30   -- s, rudder slewing hard over
local kInputDeadzone     = 0.12   -- gamepad stick/trigger deadzone

-- Rudder authority is reduced at speed on top of BoatSystem's own
-- speed-proportional authority. Without this the boat is docile at harbour
-- speed and spins on its axis at full ahead, which is the single most common
-- way an arcade boat becomes a fight to steer.
local kSteerFalloffSpeed = 16.0   -- m/s at which the falloff reaches full effect
local kSteerFalloffMin   = 0.62   -- rudder command multiplier at/above that speed

-- ── Sail handling ───────────────────────────────────────────────────────────
-- Shortening sail is a WINCH, not a switch: holding Q/E walks the sail set up
-- and down at this rate so the player commits to a decision rather than
-- toggling it. Roughly four seconds from bare poles to full canvas.
local kSailSetRate       = 0.28   -- units of sail set per second, held

-- Automatic reefing. Above kReefWind of APPARENT wind the boat starts taking
-- canvas in on its own, reaching kReefFloor by kReefWindMax. This exists so a
-- player who sails into the storm state without touching anything gets a boat
-- that is hard work rather than a boat on its beam ends — but it only ever
-- LOWERS the ceiling, so a player who has already shortened sail further keeps
-- their setting.
local kReefWind          = 11.0   -- m/s apparent, where auto-reefing begins
local kReefWindMax       = 20.0   -- m/s apparent, where it bottoms out
local kReefFloor         = 0.42   -- least canvas auto-reefing will leave set

local kSailChildName     = "Boat Sail"

-- ── Runtime state ───────────────────────────────────────────────────────────
local throttle   = 0.0
local steer      = 0.0
local sailSet    = 1.0
local prevPos    = nil
local speed      = 0.0
local sailID     = nil
local warnedNoBoat = false
local warnedNoSail = false
local wasLuffing = false
local saveElapsed = 0.0
local nextSaveAt = 3.0
local kSaveInterval = 30.0
local kSaveSlot = "drift_voyage"

-- Exponential blend factor for a time constant, frame-rate independent.
local function blend(tau, dt)
    if tau <= 0.0 or dt <= 0.0 then return 1.0 end
    return 1.0 - math.exp(-dt / tau)
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
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

function BoatController.OnCreate(id)
    -- The boat's bindings live in the Vehicle context (Config/InputActions.yaml).
    -- Nothing else in this scene wants the Gameplay context, so a hard switch is
    -- correct here; a scene that layered a menu over the boat would push instead.
    Input.SetInputContext(InputContext.Vehicle)

    throttle, steer = 0.0, 0.0
    sailSet = 1.0
    prevPos, speed = nil, 0.0
    wasLuffing = false
    saveElapsed, nextSaveAt = 0.0, 3.0
    sailID = entity_utils.find_by_name(kSailChildName)

    Log.Info("[Drift] Under sail — A/D helm, Q/E sail set, W/S auxiliary engine, C look astern.")
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

    local sail = entity_utils.get_component(id, "SailComponent")
    if not sail and not warnedNoSail then
        warnedNoSail = true
        Log.Warn("[Drift] No SailComponent on the boat — the auxiliary engine is the only propulsion.")
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

    -- Persist a first Continue point shortly after the voyage becomes live,
    -- then refresh it at a deliberately low cadence. SaveGameManager captures
    -- on the game thread and writes asynchronously, so this never asks the
    -- renderer or a script callback to swap registries mid-tick.
    saveElapsed = saveElapsed + dt
    if saveElapsed >= nextSaveAt then
        SaveGame.Save(kSaveSlot, "Drift voyage")
        nextSaveAt = saveElapsed + kSaveInterval
        Log.Info("[Drift] Voyage progress queued for save.")
    end

    -- ── Auxiliary engine ────────────────────────────────────────────────────
    local throttleTarget = axisMagnitude("Boat.ThrottleAhead") - axisMagnitude("Boat.ThrottleAstern")
    throttleTarget = clamp(throttleTarget, -1.0, 1.0)
    local tau = (math.abs(throttleTarget) > math.abs(throttle)) and kThrottleTauUp or kThrottleTauDown
    throttle = throttle + (throttleTarget - throttle) * blend(tau, dt)
    boat.throttleInput = clamp(throttle, -1.0, 1.0)

    -- ── Rudder ──────────────────────────────────────────────────────────────
    local steerTarget = axisMagnitude("Boat.SteerStarboard") - axisMagnitude("Boat.SteerPort")
    steerTarget = clamp(steerTarget, -1.0, 1.0)
    steer = steer + (steerTarget - steer) * blend(kRudderTau, dt)

    local falloff = 1.0 - (1.0 - kSteerFalloffMin) * clamp(speed / kSteerFalloffSpeed, 0.0, 1.0)

    -- Straight through: BoatComponent's "1 = full starboard" now really is
    -- starboard. This line used to be negated because BoatSystem applied a +Y
    -- yaw torque for a positive steer, which takes a +Z-forward hull's bow
    -- toward +X — the PORT beam. Fixed in the engine by #897, so the workaround
    -- is gone and the sign the player's binding produces is the sign the boat
    -- turns.
    boat.steerInput = clamp(steer * falloff, -1.0, 1.0)

    if not sail then return end

    -- ── Sail set ────────────────────────────────────────────────────────────
    local setDelta = axisMagnitude("Boat.SailMore") - axisMagnitude("Boat.SailLess")
    sailSet = clamp(sailSet + setDelta * kSailSetRate * dt, 0.0, 1.0)

    -- The auto-reef CEILING. Applied to the commanded value rather than stored
    -- into it, so easing off in a blow does not silently throw away what the
    -- player had set for when the wind drops again.
    local gale = clamp((sail.apparentWindSpeed - kReefWind) / (kReefWindMax - kReefWind), 0.0, 1.0)
    local ceiling = 1.0 - (1.0 - kReefFloor) * gale
    sail.sailSetInput = clamp(math.min(sailSet, ceiling), 0.0, 1.0)

    -- ── Pose the sail ───────────────────────────────────────────────────────
    -- The one line this whole issue was about. SailComponent.yardAngle is in
    -- radians about the hull's +Y (positive = the trim for wind from starboard,
    -- see SailComponent for why that is not the same as "braced to starboard"),
    -- and the sail model was re-origined onto its mast so this rotation braces
    -- the yard round the mast rather than swinging it around the boat — see the
    -- sail entity's comment in Drift.olo.
    if not sailID then
        sailID = entity_utils.find_by_name(kSailChildName)
    end
    if sailID then
        local rot = entity_utils.get_rotation(sailID)
        rot.y = sail.yardAngle
        entity_utils.set_rotation(sailID, rot)
    end

    -- ── In irons ────────────────────────────────────────────────────────────
    -- Told once per transition, not per tick. Without a cue, a player who has
    -- luffed head to wind sees a boat that has simply stopped answering, which
    -- reads as a bug rather than as a mistake they made.
    local luffing = sail.luffing
    if luffing ~= wasLuffing then
        if luffing then
            Log.Info("[Drift] In irons — the sail is not drawing. Bear away with the helm.")
        end
        wasLuffing = luffing
    end
end

function BoatController.OnDestroy(id)
    -- Hand the input context back so a scene switch doesn't inherit the boat's map.
    Input.SetInputContext(InputContext.Gameplay)
end

return BoatController
