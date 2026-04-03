-- LuaGameManager.lua — Lua port of GameManager.cs
-- Global game state: pause (Esc), death detection, victory condition, time scale.
-- Attach via LuaScriptComponent with ScriptFile = "Scripts/LuaScripts/LuaGameManager.lua"

local GameManager = {}

local paused = false
local gameOver = false
local victory = false

-- Shared game state table for other scripts to check
GameState = GameState or {}
GameState.paused = false

function GameManager.OnCreate(id)
    GameState.paused = false
    Log.Info("[LuaGameManager] OnCreate — Game started")
end

function GameManager.OnUpdate(id, dt)
    if gameOver then
        -- Restart on R
        if Input.IsKeyJustPressed(KeyCode.R) then
            GameState.paused = false
            Scene.ReloadCurrentScene()
        end
        return
    end

    -- Pause toggle
    if Input.IsKeyJustPressed(KeyCode.Escape) then
        paused = not paused
        GameState.paused = paused
        if paused then
            Application.SetTimeScale(0.0)
            Log.Info("[LuaGameManager] Paused")
            -- Show pause overlay
            local overlayID = entity_utils.find_by_name("Pause Overlay")
            if overlayID then
                local panel = entity_utils.get_component(overlayID, "UIPanelComponent")
                if panel then
                    panel.backgroundColor = vec4.new(0.0, 0.0, 0.05, 0.85)
                end
            end
        else
            Application.SetTimeScale(1.0)
            Log.Info("[LuaGameManager] Unpaused")
            local overlayID = entity_utils.find_by_name("Pause Overlay")
            if overlayID then
                local panel = entity_utils.get_component(overlayID, "UIPanelComponent")
                if panel then
                    panel.backgroundColor = vec4.new(0.0, 0.0, 0.0, 0.0)
                end
            end
        end
    end

    -- Quit while paused
    if paused and Input.IsKeyJustPressed(KeyCode.Q) then
        Application.QuitGame()
        return
    end

    if paused then return end

    -- Check player death
    local playerID = entity_utils.find_by_name("Player")
    if playerID then
        local ac = entity_utils.get_component(playerID, "AbilityComponent")
        if ac and not ac:HasTag("State.Alive") then
            gameOver = true
            GameState.paused = true
            Application.SetTimeScale(0.0)
            Log.Warn("[LuaGameManager] Player died — Game Over! Press R to restart")
            local overlayID = entity_utils.find_by_name("Death Overlay")
            if overlayID then
                local panel = entity_utils.get_component(overlayID, "UIPanelComponent")
                if panel then
                    panel.backgroundColor = vec4.new(0.15, 0.0, 0.0, 0.85)
                end
            end
            return
        end
    end

    -- Check victory (all enemies dead)
    local allDead = true
    local enemies = { "Goblin", "Fire Mage" }
    for _, name in ipairs(enemies) do
        local enemyID = entity_utils.find_by_name(name)
        if enemyID then
            local ac = entity_utils.get_component(enemyID, "AbilityComponent")
            if ac and ac:HasTag("State.Alive") then
                allDead = false
                break
            end
        end
    end

    if allDead then
        victory = true
        gameOver = true
        GameState.paused = true
        Application.SetTimeScale(0.0)
        Log.Info("[LuaGameManager] Victory! All enemies defeated. Press R to restart")
        local overlayID = entity_utils.find_by_name("Victory Overlay")
        if overlayID then
            local panel = entity_utils.get_component(overlayID, "UIPanelComponent")
            if panel then
                panel.backgroundColor = vec4.new(0.0, 0.1, 0.0, 0.85)
            end
        end
    end
end

function GameManager.OnDestroy(id)
    -- Restore time scale on exit
    Application.SetTimeScale(1.0)
    Log.Info("[LuaGameManager] OnDestroy — cleaning up")
end

return GameManager
