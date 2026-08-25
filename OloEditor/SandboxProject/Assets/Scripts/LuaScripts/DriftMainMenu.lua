-- DriftMainMenu.lua — authored start-scene shell for Drift (#883).
--
-- UIButtonComponent owns presentation/input state; this controller observes the
-- Pressed -> Hovered transition produced by UIInputSystem and routes the four
-- product actions through existing runtime-safe APIs. Scene changes remain
-- deferred to the host, and settings opens the existing RuntimeInputRebindMenu.

local MainMenu = {}

local kSaveSlot = "drift_voyage"

local buttonIDs = {}
local previousStates = {}
local transitionRequested = false

local function component(entityID, name)
    if not entityID then return nil end
    return entity_utils.get_component(entityID, name)
end

local function findButton(name)
    local id = entity_utils.find_by_name(name)
    buttonIDs[name] = id
    local button = component(id, "UIButtonComponent")
    if button then previousStates[name] = button.state end
    return button
end

local function clicked(name)
    local button = component(buttonIDs[name], "UIButtonComponent")
    if not button then return false end

    local current = button.state
    local previous = previousStates[name]
    previousStates[name] = current
    return previous == UIButtonState.Pressed and current == UIButtonState.Hovered
end

local function refreshContinue()
    local available = SaveGame.ValidateSave(kSaveSlot)
    local button = component(buttonIDs["Continue Button"], "UIButtonComponent")
    if button then button.interactable = available end

    local labelID = entity_utils.find_by_name("Continue Label")
    local label = component(labelID, "UITextComponent")
    if label then
        label.text = available and "CONTINUE" or "CONTINUE  -  NO VOYAGE"
        label.color = available and vec4.new(0.92, 0.95, 1.0, 1.0)
                                or vec4.new(0.48, 0.52, 0.58, 1.0)
    end
end

function MainMenu.OnCreate(id)
    transitionRequested = false
    Input.SetCursorMode(CursorMode.Normal)
    Input.SetInputContext(InputContext.Menu)

    findButton("New Game Button")
    findButton("Continue Button")
    findButton("Settings Button")
    findButton("Quit Button")
    refreshContinue()

    Log.Info("[Drift] Main menu ready — choose New Game, Continue, Settings, or Quit.")
end

function MainMenu.OnUpdate(id, dt)
    if transitionRequested then return end

    if clicked("New Game Button") then
        -- Delete first, then request an ordinary authored-scene transition: no
        -- old progress can leak into a new voyage.
        SaveGame.DeleteSave(kSaveSlot)
        transitionRequested = true
        Scene.LoadScene("Drift")
    elseif clicked("Continue Button") then
        if SaveGame.ValidateSave(kSaveSlot) then
            transitionRequested = true
            Scene.LoadSceneFromSave("Drift", kSaveSlot)
        else
            -- The file may have disappeared since OnCreate (cloud/local sync or
            -- external deletion). Keep the menu honest rather than loading blind.
            refreshContinue()
        end
    elseif clicked("Settings Button") then
        -- DriftBoatController reads the Vehicle map, so this edits real sailing
        -- controls rather than the generic Gameplay defaults.
        Input.RequestRebindMenu(InputContext.Vehicle)
    elseif clicked("Quit Button") then
        Application.QuitGame()
    end
end

return MainMenu
