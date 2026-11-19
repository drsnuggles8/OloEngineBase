-- Stacks Light's keeper greets each approaching boat once. The rig and all its
-- attachments are authored in Drift so a Continue restores the same entities.
local Keeper = {}
local state = {}
-- Scene story flags travel in drift_voyage. Keeping the greeting here means
-- Continue beside the stair does not count as a new arrival.
local greetingFlag = "drift.stacks_light.greeted"

local function clamp(x, lo, hi) return math.min(hi, math.max(lo, x)) end
local function yaw(id)
    local r = entity_utils.get_rotation(id)
    if math.abs(r.x) > 1.5 and math.abs(r.z) > 1.5 then return math.pi - r.y end
    return r.y
end

function Keeper.OnCreate(id)
    local s = { clock = 0, waveUntil = 0, greeted = dialogue_vars.get_bool(greetingFlag), look = 0 }
    state[id] = s
    for key, name in pairs({ boat = "Boat", sky = "Atmosphere", tower = "Stacks Light",
                             wick = "Keeper Lamp Socket", light = "Keeper Lamplight",
                             foundation = "Keeper Hair Foundation", left = "Keeper Eye L", right = "Keeper Eye R" }) do
        s[key] = entity_utils.find_by_name(name)
        if not s[key] or s[key] == 0 then
            Log.Warn("[DriftKeeper] missing " .. name)
            s[key] = nil
        end
    end
    s.anim = entity_utils.get_component(id, "AnimationStateComponent")
    if s.anim then
        s.clip = s.anim.currentClip
        if s.greeted and s.clip == "Wave" then
            s.waveUntil = math.max(0, 3.6 - s.anim.currentTime)
        end
    end
    s.morph = entity_utils.get_component(id, "MorphTargetComponent")
    s.fibres = s.foundation and entity_utils.get_component(s.foundation, "MorphTargetComponent")
    s.ik = entity_utils.get_component(id, "IKTargetComponent")
    local skeleton = entity_utils.get_component(id, "SkeletonComponent")
    if s.ik and skeleton then
        s.ik.aimBoneIndex = skeleton:GetBoneIndex("head")
        s.ik.aimIKEnabled = s.ik.aimBoneIndex >= 0
    end
end

function Keeper.OnUpdate(id, dt)
    local s = state[id]
    if not s or dt <= 0 then return end
    s.clock = s.clock + dt
    local here = entity_utils.get_world_translation(id)
    local boat = s.boat and entity_utils.get_world_translation(s.boat)
    local dx, dz = 0, 0
    if boat then dx, dz = boat.x - here.x, boat.z - here.z end
    local range = boat and math.sqrt(dx * dx + dz * dz) or math.huge
    local sky = s.sky and entity_utils.get_component(s.sky, "TimeOfDayComponent")
    local hour = sky and sky.timeOfDayHours or 12
    local dark = hour >= 18.5 or hour < 6
    if range > 78 and s.greeted then
        s.greeted = false
        dialogue_vars.set_bool(greetingFlag, false)
    end
    if dark or range > 78 then s.waveUntil = 0 end
    if range < 62 and not s.greeted and not dark then
        s.greeted, s.waveUntil = true, s.clock + 3.6
        dialogue_vars.set_bool(greetingFlag, true)
    end
    local clip = dark and "LanternRaise" or (s.clock < s.waveUntil and "Wave" or "Idle")
    if s.anim and clip ~= s.clip then
        s.anim:PlayClip(clip, clip == "Idle")
        s.clip = clip
        Log.Info("[DriftKeeper] " .. clip)
    end
    local alpha = 1 - math.exp(-4 * dt)
    local attending = boat and range < 115
    s.look = s.look + ((attending and 1 or 0) - s.look) * alpha
    if s.ik then
        local heading = yaw(id) + (s.tower and yaw(s.tower) or 0)
        local bearing = math.atan(dx, dz) - heading
        bearing = (bearing + math.pi) % (2 * math.pi) - math.pi
        local targetYaw = attending and clamp(bearing, -1.0, 1.0) or 0
        local targetPitch = attending and clamp(math.atan(boat.y + 1.5 - here.y - 1.63, math.max(1, range)), -.65, .42) or 0
        s.headYaw = (s.headYaw or 0) + (targetYaw - (s.headYaw or 0)) * alpha
        s.pitch = (s.pitch or 0) + (targetPitch - (s.pitch or 0)) * alpha
        s.ik.aimWeight = s.look
        s.ik.aimTarget = vec3.new(here.x + 8 * math.sin(heading + s.headYaw),
                                  here.y + 1.63 + 8 * math.tan(s.pitch),
                                  here.z + 8 * math.cos(heading + s.headYaw))
        local gaze = vec3.new(-.12 * s.pitch, clamp(bearing - s.headYaw, -.25, .25) * s.look, 0)
        if s.left then entity_utils.set_rotation(s.left, gaze) end
        if s.right then entity_utils.set_rotation(s.right, gaze) end
    end
    if s.morph then
        local phase = s.clock % 4.7
        local blink = math.max(0, 1 - math.abs(phase - 3.22) / .105)
        s.morph:SetWeight("MorphTarget_0", blink)
        s.morph:SetWeight("MorphTarget_1", blink)
        local smile = clip == "Wave" and .42 or 0
        s.morph:SetWeight("MorphTarget_3", smile)
        if s.fibres then
            s.fibres:SetWeight("MorphTarget_0", blink)
            s.fibres:SetWeight("MorphTarget_1", blink)
            s.fibres:SetWeight("MorphTarget_3", smile)
        end
    end
    if s.wick and s.light then
        local light = entity_utils.get_component(s.light, "PointLightComponent")
        if light then
            light.intensity = dark and 2.1 or 0
            if dark then entity_utils.set_translation(s.light, entity_utils.get_world_translation(s.wick)) end
        end
    end
end

function Keeper.OnDestroy(id) state[id] = nil end
return Keeper
