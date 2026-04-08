-- LuaGoblinAI.lua — Lua port of GoblinAI.cs
-- Simple melee AI: idle → aggro → chase → attack cycle.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaGoblinAI.lua"

local GoblinAI = {}

local aggroRange = 8.0
local attackRange = 2.0
local moveSpeed = 3.0
local attackCooldown = 2.0

local alive = true
local attackTimer = 0.0
local flashTimer = 0.0
local flashDuration = 0.15
local lastHealth = -1.0
local originalColor = nil

function GoblinAI.OnCreate(id)
    local name = entity_utils.get_name(id) or "Goblin"
    Log.Info("[LuaGoblinAI] OnCreate — " .. name)

    local abilities = entity_utils.get_component(id, "AbilityComponent")
    if abilities then
        -- Only init defaults if not already serialized (Health == 0 means no data)
        local hp = abilities:GetCurrentAttribute("Health")
        if hp <= 0.0 then
            abilities:InitDefaultRPG(60.0, 0.0, 10.0, 3.0)
        end
        abilities:AddTag("State.Alive")
        lastHealth = abilities:GetCurrentAttribute("Health")
    end
end

function GoblinAI.OnUpdate(id, dt)
    if not alive then return end
    if GameState and GameState.paused then return end

    local abilities = entity_utils.get_component(id, "AbilityComponent")
    if abilities then
        local health = abilities:GetCurrentAttribute("Health")

        -- Death check
        if health <= 0.0 then
            alive = false
            abilities:RemoveTag("State.Alive")
            Log.Warn("[LuaGoblinAI] Goblin died!")
            return
        end

        -- Damage flash
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

    -- Flash restore
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

    -- Find player
    local playerID = entity_utils.find_by_name("Player")
    if not playerID then return end

    -- Check player alive
    local playerAC = entity_utils.get_component(playerID, "AbilityComponent")
    if playerAC and not playerAC:HasTag("State.Alive") then
        return -- player dead, stop
    end

    local myPos = entity_utils.get_translation(id)
    local playerPos = entity_utils.get_translation(playerID)
    local dx = playerPos.x - myPos.x
    local dy = playerPos.y - myPos.y
    local dist = math.sqrt(dx * dx + dy * dy)

    -- Outside aggro range — idle
    if dist > aggroRange then return end

    -- Update attack timer
    if attackTimer > 0.0 then
        attackTimer = attackTimer - dt
    end

    -- Within attack range — attack
    if dist <= attackRange then
        if attackTimer <= 0.0 and abilities then
            if Damage.TryActivateAbilityOnTarget(id, "Ability.Melee.Bite", playerID) then
                attackTimer = attackCooldown
                Log.Info("[LuaGoblinAI] Bite!")
            end
        end
        return -- don't move while attacking
    end

    -- Chase — move toward player
    local invDist = 1.0 / dist
    local dirX = dx * invDist
    local dirY = dy * invDist
    myPos.x = myPos.x + dirX * moveSpeed * dt
    myPos.y = myPos.y + dirY * moveSpeed * dt
    entity_utils.set_translation(id, myPos)
end

function GoblinAI.OnDestroy(id)
    Log.Info("[LuaGoblinAI] OnDestroy — entity " .. tostring(id))
end

return GoblinAI
