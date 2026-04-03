-- LuaPlayerController.lua — Lua port of PlayerController.cs
-- Top-down player with WASD movement, ability hotkeys (1/2/3), and camera zoom.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaPlayerController.lua"

local PlayerController = {}

local moveSpeed = 5.0
local myID = nil
local alive = true

-- VFX timers
local slashVfxTimer = 0.0
local fireballVfxTimer = 0.0
local healVfxTimer = 0.0
local vfxDuration = 0.5

-- Damage flash
local flashTimer = 0.0
local flashDuration = 0.15
local lastHealth = -1.0
local originalColor = nil

-- Module-local helpers (forward declarations)
local FindNearestEnemy
local TriggerFlash

function PlayerController.OnCreate(id)
    myID = id
    local name = entity_utils.get_name(id) or "Player"
    Log.Info("[LuaPlayerController] OnCreate — " .. name)

    -- Initialize ability attributes only if not already serialized
    local abilities = entity_utils.get_component(id, "AbilityComponent")
    if abilities then
        local hp = abilities:GetCurrentAttribute("Health")
        if hp <= 0.0 then
            abilities:InitDefaultRPG(100.0, 50.0, 15.0, 5.0)
        end
        abilities:AddTag("State.Alive")
        lastHealth = abilities:GetCurrentAttribute("Health")
        Log.Info("[LuaPlayerController] Abilities initialized — HP: " .. tostring(lastHealth))
    end
end

function PlayerController.OnUpdate(id, dt)
    if not alive then return end
    if GameState and GameState.paused then return end

    -- Check if still alive
    local abilities = entity_utils.get_component(id, "AbilityComponent")
    if abilities then
        local health = abilities:GetCurrentAttribute("Health")
        if health <= 0.0 then
            alive = false
            abilities:RemoveTag("State.Alive")
            Log.Warn("[LuaPlayerController] Player died!")
            return
        end

        -- Damage flash detection
        if lastHealth > 0.0 and health < lastHealth then
            flashTimer = flashDuration
            local sr = entity_utils.get_component(id, "SpriteRendererComponent")
            if sr then
                if not originalColor then
                    originalColor = vec4.new(sr.color.x, sr.color.y, sr.color.z, sr.color.w)
                end
                sr.color = vec4.new(1.0, 0.3, 0.3, 1.0)
            end
        end
        lastHealth = health
    end

    -- Update flash timer
    if flashTimer > 0.0 then
        flashTimer = flashTimer - dt
        if flashTimer <= 0.0 then
            local sr = entity_utils.get_component(id, "SpriteRendererComponent")
            if sr and originalColor then
                sr.color = originalColor
                originalColor = nil
            end
        end
    end

    -- WASD movement
    local pos = entity_utils.get_translation(id)
    local vx = 0.0
    local vy = 0.0

    if Input.IsKeyDown(KeyCode.W) then vy = 1.0 end
    if Input.IsKeyDown(KeyCode.S) then vy = vy - 1.0 end
    if Input.IsKeyDown(KeyCode.A) then vx = -1.0 end
    if Input.IsKeyDown(KeyCode.D) then vx = vx + 1.0 end

    -- Normalize diagonal
    if vx ~= 0.0 and vy ~= 0.0 then
        local inv = 1.0 / math.sqrt(2.0)
        vx = vx * inv
        vy = vy * inv
    end

    pos.x = pos.x + vx * moveSpeed * dt
    pos.y = pos.y + vy * moveSpeed * dt
    entity_utils.set_translation(id, pos)

    -- Camera zoom (Q/E)
    local cameraID = entity_utils.find_by_name("Camera")
    if cameraID then
        local camPos = entity_utils.get_translation(cameraID)
        if Input.IsKeyDown(KeyCode.Q) then
            camPos.z = camPos.z + moveSpeed * 2.0 * dt
            entity_utils.set_translation(cameraID, camPos)
        elseif Input.IsKeyDown(KeyCode.E) then
            camPos.z = camPos.z - moveSpeed * 2.0 * dt
            entity_utils.set_translation(cameraID, camPos)
        end
    end

    -- Ability hotkeys (1 = Slash, 2 = Fireball, 3 = Heal)
    -- Each ability enforces its own cooldown via the AbilityComponent system
    if abilities then
        if Input.IsKeyJustPressed(KeyCode.D1) then
            -- Slash — melee on nearest enemy
            local targetID = FindNearestEnemy(id, 3.0)
            if targetID then
                if Damage.TryActivateAbilityOnTarget(id, "Ability.Melee.Slash", targetID) then
                    TriggerFlash(id, vec4.new(1.0, 1.0, 0.3, 1.0))
                    Log.Info("[LuaPlayerController] Slash!")
                end
            end
        elseif Input.IsKeyJustPressed(KeyCode.D2) then
            -- Fireball — ranged on nearest enemy
            local targetID = FindNearestEnemy(id, 10.0)
            if targetID then
                if Damage.TryActivateAbilityOnTarget(id, "Ability.Spell.Fire.Fireball", targetID) then
                    TriggerFlash(id, vec4.new(1.0, 0.5, 0.1, 1.0))
                    Log.Info("[LuaPlayerController] Fireball!")
                end
            end
        elseif Input.IsKeyJustPressed(KeyCode.D3) then
            -- Heal — self
            if Damage.TryActivateAbility(id, "Ability.Buff.Heal") then
                TriggerFlash(id, vec4.new(0.3, 1.0, 0.3, 1.0))
                Log.Info("[LuaPlayerController] Heal!")
            end
        end
    end
end

function PlayerController.OnDestroy(id)
    Log.Info("[LuaPlayerController] OnDestroy — entity " .. tostring(id))
end

-- Module-local helpers

FindNearestEnemy = function(myEntityID, maxRange)
    local myPos = entity_utils.get_translation(myEntityID)
    local bestDist = maxRange
    local bestID = nil

    local enemies = { "Goblin", "Fire Mage" }
    for _, name in ipairs(enemies) do
        local enemyID = entity_utils.find_by_name(name)
        if enemyID then
            local ac = entity_utils.get_component(enemyID, "AbilityComponent")
            if ac and ac:HasTag("State.Alive") then
                local ePos = entity_utils.get_translation(enemyID)
                local dx = ePos.x - myPos.x
                local dy = ePos.y - myPos.y
                local dist = math.sqrt(dx * dx + dy * dy)
                if dist < bestDist then
                    bestDist = dist
                    bestID = enemyID
                end
            end
        end
    end
    return bestID
end

TriggerFlash = function(entityID, color)
    local sr = entity_utils.get_component(entityID, "SpriteRendererComponent")
    if sr then
        if not originalColor then
            originalColor = vec4.new(sr.color.x, sr.color.y, sr.color.z, sr.color.w)
        end
        sr.color = color
        flashTimer = flashDuration
    end
end

return PlayerController
