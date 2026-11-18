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
--   * It poses what hangs off those two: the side rudder turns with the helm,
--     and the sheets and braces are stretched from their leads on the gunwale
--     to the clews and yard arms wherever the yard has braced them to. None of
--     that feeds back into the physics; it is the boat showing what she is
--     doing. The geometry it needs is all in the scene (each line's own
--     translation is its lead, each marker entity under the sail is its far
--     end), so a re-rigged boat is a scene edit, not an edit here. The
--     exceptions are the rudder's angle at full helm (kRudderMaxAngle) and the
--     order of the sail's morph targets (kSailMorphs), both below:
--     DriftBoatDelivery fails if either disagrees with the generated rig file.
--   * It gives the CLOTH its shape. The sail's mesh carries morph targets and
--     this script blends them from what the wind is doing to her (see "The
--     cloth" below). That is drawing only: the force is SailSystem's.
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
-- THE AUXILIARY IS A SWEEP. BoatComponent still calls it thrust and the tests
-- still call it the engine, but the boat that is drawn has no engine: W/S works
-- the steering oar as a sculling sweep over the quarter, which is why the
-- scene puts the thrust point at the rudder blade.
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
local kThrottleTauUp     = 0.85   -- s, the sweep getting into its stroke
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

-- ── What the crew's hands are on ────────────────────────────────────────────
-- The rudder entity is canted about Z to lie along the hull's side (authored in
-- the scene) and this script only ever writes its Y, which the engine applies
-- first: the blade turns about its own shaft. 28 degrees at full helm.
local kRudderName        = "Boat Rudder"
local kRudderMaxAngle    = 0.49   -- rad: DriftBoat.rig.json's rudder.maxAngleDeg (DriftBoatDelivery checks it)

-- ── The cloth ───────────────────────────────────────────────────────────────
-- The sail's morph targets, in the order the model's files hold them
-- (DriftBoat.rig.json, sail.morphTargets). The engine names an imported target
-- by its index, so the i-th name here is "MorphTarget_<i-1>".
--   Slack   she hangs from the yard, no wind in her
--   Hard    a gale in her
--   Aback   the wind on her fore face, pressing her round the mast
--   Furled  gathered right up to the yard
--   Luff*   a slow, deep wave, three phases a third of a cycle apart
--   Ripple* a quick, shallow one, the same way
-- The file's own shape, every weight at zero, is the sail full of a breeze.
local kSailMorphs = { "Slack", "Hard", "Aback", "Furled", "Luff0", "Luff120", "Luff240", "Ripple0", "Ripple120", "Ripple240" }
local kMorph = {}
for i, name in ipairs(kSailMorphs) do kMorph[name] = "MorphTarget_" .. (i - 1) end
-- The three phases of each wave, with how far round the cycle each one sits.
local kThird = 2.0 * math.pi / 3.0
local kLuffWave = { { kMorph.Luff0, 0.0 }, { kMorph.Luff120, kThird }, { kMorph.Luff240, 2.0 * kThird } }
local kRippleWave = { { kMorph.Ripple0, 0.0 }, { kMorph.Ripple120, kThird }, { kMorph.Ripple240, 2.0 * kThird } }

-- Wind speed squared is the pressure in her, less when the wind meets the cloth
-- at a slant (kSlantFill of it is left when it runs right along her). At
-- kFillPressure she is the shape the file holds, the Overcast breeze on a
-- reach; below it she slackens toward Slack, and by kHardPressure times that
-- she is as full as Hard.
--
-- How squarely the wind meets her is deliberately a small part of it. SailSystem
-- lets the boat run up to the speed at which the apparent wind has drawn almost
-- along the braced yard, so on any steady reach the wind meets the cloth at ten
-- or fifteen degrees. Read literally that is a sail on the edge of collapse, and
-- drawn literally a boat making her best speed carried a half-empty, shaking
-- sail. What the eye expects there is a full one.
local kFillPressure      = 9.0    -- (m/s)^2
local kHardPressure      = 4.0
local kSlantFill         = 0.35
local kClothTau          = 0.45   -- s, cloth does not change shape at once
-- She shakes when SailSystem says she is not drawing (in irons, or no sail
-- set), and a little when the wind is within kLuffEdge of running along her.
local kLuffEdge          = 0.15
local kLuffingFill       = 0.35   -- how much of her belly a luffing sail keeps
-- A shortened sail keeps this much less of the full sail's belly and waves.
local kFurledBody        = 0.85
local kFurledClewNames   = { ["Boat Clew Port"] = "Boat Clew Port Furled", ["Boat Clew Starboard"] = "Boat Clew Starboard Furled" }

-- Each line is one metre of rope along its own +Z, a child of the hull placed
-- at its lead. `anchor` is a marker entity under the sail: the clew or yard arm
-- the line is made fast to.
local kLines = {
    { line = "Boat Sheet Port",      anchor = "Boat Clew Port" },
    { line = "Boat Sheet Starboard", anchor = "Boat Clew Starboard" },
    { line = "Boat Brace Port",      anchor = "Boat Yardarm Port" },
    { line = "Boat Brace Starboard", anchor = "Boat Yardarm Starboard" },
}

-- ── Runtime state ───────────────────────────────────────────────────────────
local throttle   = 0.0
local steer      = 0.0
local sailSet    = 1.0
local prevPos    = nil
local speed      = 0.0
local sailID     = nil
local rudderID   = nil
local lines      = {}
local cloth      = { fill = 1.0, hard = 0.0, aback = 0.0, luff = 0.0, ripple = 0.0, luffPhase = 0.0, ripplePhase = 0.0, furled = 0.0 }
local rigTries   = 0      -- lookups left before the rig is taken as it stands
local kRigTries  = 30     -- ticks: entities created after this script still get found
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

-- Find the drawn rig by name: the sail, the rudder, and each line with the
-- marker it is made fast to. Returns the names that are not in the scene.
local function findRig()
    local missing = {}
    sailID = sailID or entity_utils.find_by_name(kSailChildName)
    if not sailID then missing[#missing + 1] = kSailChildName end
    rudderID = rudderID or entity_utils.find_by_name(kRudderName)
    if not rudderID then missing[#missing + 1] = kRudderName end
    lines = {}
    for _, l in ipairs(kLines) do
        local lineID = entity_utils.find_by_name(l.line)
        local anchorID = entity_utils.find_by_name(l.anchor)
        -- A clew rises as the sail is gathered up, so a sheet has a second marker:
        -- where its clew is with the sail furled. The markers themselves are never
        -- moved (a save would keep wherever a reef had left them); poseLines takes
        -- the point between the two.
        local furledName = kFurledClewNames[l.anchor]
        local furledID = furledName and entity_utils.find_by_name(furledName) or nil
        if lineID and anchorID then
            lines[#lines + 1] = { lineID = lineID, anchorID = anchorID, furledID = furledID }
        else
            if not lineID then missing[#missing + 1] = l.line end
            if not anchorID then missing[#missing + 1] = l.anchor end
        end
        if furledName and not furledID then missing[#missing + 1] = furledName end
    end
    return missing
end

-- Blend the sail's shape from the wind. Everything here is what the eye
-- expects of cloth and none of it is physics: SailSystem has already decided
-- the force from the same apparent wind.
local function poseCloth(sail, dt)
    local morph = entity_utils.get_component(sailID, "MorphTargetComponent")
    if not morph then return end

    -- How squarely the apparent wind meets the cloth, and on which face. The
    -- sail's normal is cos(yard) * forward + sin(yard) * port, and the wind
    -- comes FROM apparentWindAngle off the bow (positive on the starboard
    -- side), so it blows along -cos(angle) * forward + sin(angle) * port.
    -- Positive: into the sail from astern of it, the way she draws. Negative:
    -- onto her fore face, which lays her aback against the mast.
    local wind = sail.apparentWindSpeed
    local square = -math.cos(sail.apparentWindAngle + sail.yardAngle)
    local meets = math.abs(square)
    local pressure = wind * wind * (kSlantFill + (1.0 - kSlantFill) * meets) / kFillPressure
    local drawing = square >= 0.0
    local luffing = sail.luffing and wind > 0.5
    local keeps = luffing and kLuffingFill or 1.0
    local fillTarget = drawing and clamp(pressure, 0.0, 1.0) * keeps or 0.0
    local hardTarget = drawing and clamp((pressure - 1.0) / (kHardPressure - 1.0), 0.0, 1.0) * keeps or 0.0
    local abackTarget = drawing and 0.0 or clamp(wind * wind * meets / kFillPressure, 0.0, 1.0)
    local edgeOn = 1.0 - clamp(meets / kLuffEdge, 0.0, 1.0)
    local luffTarget = clamp(wind / 5.0, 0.0, 1.0) * math.max(luffing and 1.0 or 0.0, 0.6 * edgeOn)
    local rippleTarget = clamp(0.10 + wind / 14.0, 0.0, 1.0) * (1.0 - 0.6 * hardTarget)

    local k = blend(kClothTau, dt)
    cloth.fill = cloth.fill + (fillTarget - cloth.fill) * k
    cloth.hard = cloth.hard + (hardTarget - cloth.hard) * k
    cloth.aback = cloth.aback + (abackTarget - cloth.aback) * k
    cloth.luff = cloth.luff + (luffTarget - cloth.luff) * k
    cloth.ripple = cloth.ripple + (rippleTarget - cloth.ripple) * k

    -- What is set of her: sailSetInput is the share of the sail that is
    -- drawing, after the auto-reef, and the rest is gathered up to the yard.
    local furled = 1.0 - clamp(sail.sailSetInput, 0.0, 1.0)
    local body = 1.0 - kFurledBody * furled
    cloth.furled = furled

    morph:SetWeight(kMorph.Furled, furled)
    morph:SetWeight(kMorph.Slack, (1.0 - cloth.fill) * (1.0 - cloth.aback) * body)
    morph:SetWeight(kMorph.Hard, cloth.hard * body)
    morph:SetWeight(kMorph.Aback, cloth.aback * body)

    -- Two travelling waves, each the sum of three standing ones a third of a
    -- cycle apart with weights (1 + cos) / 2: a weight cannot go negative, and
    -- these three never do while their steady parts cancel.
    cloth.luffPhase = (cloth.luffPhase + dt * 2.0 * math.pi * (1.3 + 0.16 * wind)) % (2.0 * math.pi)
    cloth.ripplePhase = (cloth.ripplePhase + dt * 2.0 * math.pi * (0.45 + 0.07 * wind)) % (2.0 * math.pi)
    for i = 1, 3 do
        morph:SetWeight(kLuffWave[i][1], cloth.luff * body * 0.5 * (1.0 + math.cos(cloth.luffPhase - kLuffWave[i][2])))
        morph:SetWeight(kRippleWave[i][1], cloth.ripple * body * 0.5 * (1.0 + math.cos(cloth.ripplePhase - kRippleWave[i][2])))
    end
end

-- Look for the rig on the first few ticks only, then take it as it stands and
-- say once what is not there. A name lookup that misses walks every entity in
-- the scene, so a renamed rope must not cost that every tick for ever, and a
-- part that will never move must not be silent about it.
local function settleRig()
    if rigTries <= 0 then return end
    rigTries = rigTries - 1
    local missing = findRig()
    if #missing == 0 then
        rigTries = 0
    elseif rigTries == 0 then
        Log.Warn("[Drift] These parts of the boat's rig are not in the scene and will not move: "
            .. table.concat(missing, ", "))
    end
end

-- Stretch every line from its lead to where the yard has carried its far end.
-- Everything is in the HULL's frame: the lines and the sail are both children
-- of the hull mesh, and the sail's only rotation is the yard angle about Y.
local function poseLines(yard)
    local sailPos = entity_utils.get_translation(sailID)
    local c, s = math.cos(yard), math.sin(yard)
    for _, l in ipairs(lines) do
        local a = entity_utils.get_translation(l.anchorID)
        if l.furledID then
            -- The clew is this far from where it is with the sail full toward
            -- where it is with the sail furled.
            local f = entity_utils.get_translation(l.furledID)
            a.x = a.x + (f.x - a.x) * cloth.furled
            a.y = a.y + (f.y - a.y) * cloth.furled
            a.z = a.z + (f.z - a.z) * cloth.furled
        end
        local lead = entity_utils.get_translation(l.lineID)
        local dx = sailPos.x + c * a.x + s * a.z - lead.x
        local dy = sailPos.y + a.y - lead.y
        local dz = sailPos.z - s * a.x + c * a.z - lead.z
        local len = math.sqrt(dx * dx + dy * dy + dz * dz)
        if len > 1.0e-4 then
            -- The engine composes Euler angles X first, then Y: pitch the rope's
            -- +Z up to the far end's height, then swing it round to its bearing.
            local rot = entity_utils.get_rotation(l.lineID)
            rot.x = -math.asin(clamp(dy / len, -1.0, 1.0))
            rot.y = math.atan(dx, dz)
            rot.z = 0.0
            entity_utils.set_rotation(l.lineID, rot)
            local scale = entity_utils.get_scale(l.lineID)
            scale.z = len
            entity_utils.set_scale(l.lineID, scale)
        end
    end
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
    sailID, rudderID, lines = nil, nil, {}
    cloth = { fill = 1.0, hard = 0.0, aback = 0.0, luff = 0.0, ripple = 0.0, luffPhase = 0.0, ripplePhase = 0.0, furled = 0.0 }
    rigTries = kRigTries
    settleRig()

    Log.Info("[Drift] Under sail — A/D helm, Q/E sail set, W/S the sweep, C look astern.")
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
        Log.Warn("[Drift] No SailComponent on the boat — the sweep is the only propulsion.")
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

    -- ── The sweep (BoatComponent's thrust) ──────────────────────────────────
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

    -- The drawn rudder follows the HELM (what the hands are doing), not the
    -- speed-trimmed command above. Positive helm is starboard, and a positive Y
    -- rotation swings the blade's trailing edge to starboard with it.
    settleRig()
    if rudderID then
        local rot = entity_utils.get_rotation(rudderID)
        rot.y = steer * kRudderMaxAngle
        entity_utils.set_rotation(rudderID, rot)
    end

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
    if sailID then
        local rot = entity_utils.get_rotation(sailID)
        rot.y = sail.yardAngle
        entity_utils.set_rotation(sailID, rot)
        poseCloth(sail, dt)
        poseLines(sail.yardAngle)
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
