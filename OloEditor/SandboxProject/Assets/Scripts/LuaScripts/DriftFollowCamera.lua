-- DriftFollowCamera.lua — Drift (issue #879), the boat's chase camera.
--
-- Attach to the "Camera Target" ROOT entity (transform + tag only). The camera
-- entity carries CameraComponent + CameraRigComponent with Target pointing at
-- that proxy, NOT at the boat.
--
-- The proxy exists for YAW SMOOTHING. CameraRigComponent smooths POSITION
-- (m_PositionSmoothTime) but writes rotation outright, so a boat that snaps 40
-- degrees in a hard turn whips the camera with it. Smoothing the proxy's yaw is
-- the only place a scene can damp that without an engine change, and it is also
-- where the look-astern hold lives.
--
-- It carries the boat's heading as-is. It used to carry that heading turned
-- through 180 degrees, because CameraRigSystem read every non-player target's
-- facing as rotation * (0,0,-1) while every force-model vehicle is Jolt's
-- rotation * (0,0,+1) — so aimed at the boat the rig parked itself AHEAD of the
-- hull looking back at it. Issue #897 fixed that: the rig now asks the target's
-- own components which convention they use, and the scene tells it about a
-- proxy like this one through CameraRigComponent's TargetForward field, which
-- Drift.olo sets to 2 (+Z forward).
--
-- The rig still does the real work — boom, collision pull-in, position
-- smoothing — and it does it at the right point in the tick (the CameraRig
-- scheduler node runs after PhysicsFence and PropagateTransforms). This script
-- runs in the Scripts node, i.e. one tick earlier, so the proxy pose it writes
-- is one fixed step stale. That is invisible next to the rig's own ~0.2 s of
-- position smoothing, and it is the honest trade for keeping the placement
-- itself in the system that is ordered correctly.
--
-- The camera never clips the sea by construction rather than by clamping: the
-- rig's boom probe is a physics raycast and water is not a physics body, so
-- there is nothing to hit. Instead the pivot sits well above the waterline and
-- the rig's authored FallbackPitchDeg tilts the boom upward, which puts the eye
-- several metres clear of the highest crest at every sea state in the scene.

local FollowCamera = {}

local kBoatName        = "Boat"
local kCameraName      = "Camera"

-- Yaw smoothing. Long enough to kill the whip on a hard rudder input, short
-- enough that the bow does not spend the turn out of frame.
local kYawTau          = 0.32   -- s
-- The look-astern swing is deliberately slower still: it is a deliberate act,
-- and snapping 180 degrees is disorienting in a swell.
local kLookAsternTau   = 0.45   -- s

-- Speed dressing. The boom stretches and the pivot drops slightly as the boat
-- picks up, which reads as acceleration without touching the field of view.
local kBoomNear        = 9.0    -- m, at rest
local kBoomFar         = 13.5   -- m, at kSpeedRef and above
local kPivotHighY      = 2.6    -- m above the boat origin (the waterline) at rest
local kPivotLowY       = 2.0    -- m at speed
local kSpeedRef        = 14.0   -- m/s
local kDressingTau     = 0.60   -- s, so the boom does not pump on every wave

local boatID     = nil
local cameraID   = nil
local yaw        = nil
local asternMix  = 0.0
local speed      = 0.0
local prevPos    = nil
local warned     = false

local function blend(tau, dt)
    if tau <= 0.0 or dt <= 0.0 then return 1.0 end
    return 1.0 - math.exp(-dt / tau)
end

local function wrapAngle(a)
    while a > math.pi do a = a - 2.0 * math.pi end
    while a < -math.pi do a = a + 2.0 * math.pi end
    return a
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- Compass heading of an entity's local +Z. See the long note on the identical
-- helper in DriftBoatController.lua: the euler triple's .y is NOT the heading
-- once the boat is more than 90 degrees round, because glm::eulerAngles confines
-- the middle angle to [-pi/2, pi/2] and represents anything past that on the
-- mirrored branch. Reading .y directly made the camera swing the wrong way
-- through hard turns.
local function headingOf(entityID)
    local e = entity_utils.get_rotation(entityID)
    -- glm::quat(vec3) composes as Rx.Ry.Rz, so local +Z lands on
    -- (sin y, -cos y * sin x, cos y * cos x). The cos x factor is what carries
    -- the mirrored branch: where |x| is near pi it is negative, and atan2 then
    -- returns sign(y) * (pi - |y|) — the correct continuation — instead of y.
    local fx = math.sin(e.y)
    local fz = math.cos(e.y) * math.cos(e.x)
    if math.abs(fx) < 1e-9 and math.abs(fz) < 1e-9 then
        return nil -- hull pointing straight up or down: heading is undefined
    end
    return math.atan(fx, fz)
end

function FollowCamera.OnCreate(id)
    boatID, cameraID = nil, nil
    yaw, prevPos = nil, nil
    asternMix, speed = 0.0, 0.0
    warned = false
end

function FollowCamera.OnUpdate(id, dt)
    if dt <= 0.0 then return end

    if not boatID then boatID = entity_utils.find_by_name(kBoatName) end
    if not boatID then
        if not warned then
            warned = true
            Log.Warn("[Drift] DriftFollowCamera cannot find an entity named '" .. kBoatName .. "'.")
        end
        return
    end

    local boatPos = entity_utils.get_translation(boatID)
    local boatHeading = headingOf(boatID)

    -- ── Speed (see DriftBoatController for why this is differentiated) ──────
    if prevPos then
        local dx, dz = boatPos.x - prevPos.x, boatPos.z - prevPos.z
        local instant = math.sqrt(dx * dx + dz * dz) / dt
        speed = speed + (instant - speed) * blend(0.25, dt)
    end
    prevPos = boatPos

    -- ── Yaw ─────────────────────────────────────────────────────────────────
    -- The proxy is declared +Z-forward in Drift.olo, so the boat's heading goes
    -- through unturned; the look-astern hold is the only pi in here.
    asternMix = asternMix + ((Input.IsActionPressed("Boat.LookAstern") and 1.0 or 0.0) - asternMix)
                            * blend(kLookAsternTau, dt)
    -- A boat pointing straight up or down has no heading; hold the last one
    -- rather than snapping the camera to an arbitrary direction.
    if boatHeading == nil then boatHeading = yaw or 0.0 end
    local desired = boatHeading + asternMix * math.pi

    if yaw == nil then
        yaw = desired
    else
        -- Step along the SHORTEST arc, or a turn through north unwinds the long
        -- way round and the camera spins through 350 degrees.
        yaw = yaw + wrapAngle(desired - yaw) * blend(kYawTau, dt)
    end
    yaw = wrapAngle(yaw)

    -- The proxy sits exactly on the boat: the rig applies its own pivot offset
    -- in the yaw frame, and doubling the offset here would fight it.
    entity_utils.set_translation(id, boatPos)

    local rot = entity_utils.get_rotation(id)
    rot.x, rot.y, rot.z = 0.0, yaw, 0.0
    entity_utils.set_rotation(id, rot)

    -- ── Speed dressing ──────────────────────────────────────────────────────
    if not cameraID then cameraID = entity_utils.find_by_name(kCameraName) end
    if cameraID then
        local rig = entity_utils.get_component(cameraID, "CameraRigComponent")
        if rig then
            local t = clamp(speed / kSpeedRef, 0.0, 1.0)
            local boomTarget  = kBoomNear + (kBoomFar - kBoomNear) * t
            local pivotTarget = kPivotHighY + (kPivotLowY - kPivotHighY) * t
            local a = blend(kDressingTau, dt)

            rig.boomLength = rig.boomLength + (boomTarget - rig.boomLength) * a

            local pivot = rig.pivotOffset
            pivot.y = pivot.y + (pivotTarget - pivot.y) * a
            rig.pivotOffset = pivot
        end
    end
end

function FollowCamera.OnDestroy(id)
end

return FollowCamera
