-- Steamworks platform services demo (issue #644).
--
-- Shows the intended usage pattern for the `Steam` table: achievements, rich presence, overlay
-- pause, and Steam Cloud.
--
-- THE ONE THING TO TAKE FROM THIS FILE: every Steam.* call is safe with Steam absent. You do NOT
-- need to guard calls with `if Steam.isAvailable()`. With no Steam client, no App ID, or an
-- engine built without the SDK at all, the functions return false / "" / nil / an empty table and
-- do nothing. Check isAvailable() only to hide UI or skip work you would rather not do — never as
-- a safety guard.
--
-- To see any of this actually happen you need the Steam client running and a `steam_appid.txt`
-- containing `480` (Valve's public Spacewar test app, which every Steam account owns) in the
-- working directory — OloEditor/. See docs/ops/build.md § "Steamworks SDK".
--
-- The achievement ids below are Spacewar's real, published ones, so this script unlocks
-- genuinely against App ID 480. Replace them with your own game's ids.

local ACH_WIN_ONE_GAME = "ACH_WIN_ONE_GAME"
local ACH_TRAVEL_FAR_ACCUM = "ACH_TRAVEL_FAR_ACCUM"

local CLOUD_SETTINGS_FILE = "demo_settings.json"

-- Overlay edge detection: the overlay covers the game, so a real game pauses while it is up.
-- Track the previous state rather than acting on the level, or you re-pause every frame.
local wasOverlayActive = false

-- Rich presence is a network round-trip; only push it when the text actually changes.
local lastPresence = nil

local function setPresence(text)
    if text ~= lastPresence then
        -- "status" is the well-known key Steam shows in the friends list. Values over Steam's
        -- 256-byte limit are REJECTED (returns false), not silently truncated.
        Steam.setRichPresence("status", text)
        lastPresence = text
    end
end

function OnCreate()
    if not Steam.isAvailable() then
        -- Not an error. This is the normal state on a machine without Steam, and the rest of
        -- this script still runs correctly — every call below simply does nothing.
        print("[SteamDemo] Steam unavailable; running without platform services.")
        return
    end

    print(string.format("[SteamDemo] Steam ready. AppID=%d, player='%s'",
                        Steam.getAppID(), Steam.getPersonaName()))

    -- isAchievementUnlocked() tells a fresh unlock from a repeat one. unlockAchievement() itself
    -- returns true in BOTH cases (it means "make sure this is unlocked"), so if you want to play
    -- a sting only on the first unlock, ask first.
    if not Steam.isAchievementUnlocked(ACH_WIN_ONE_GAME) then
        if Steam.unlockAchievement(ACH_WIN_ONE_GAME) then
            print("[SteamDemo] Unlocked " .. ACH_WIN_ONE_GAME .. " — the overlay toast should appear.")
        end
    else
        print("[SteamDemo] " .. ACH_WIN_ONE_GAME .. " was already unlocked.")
    end

    -- Repeat unlocks are cheap by design: the engine checks the current state first and skips
    -- the network store, so calling this every frame would not spam Steam. Do not rely on that
    -- as a licence to be careless, but it does mean an accidental repeat is harmless.
    Steam.unlockAchievement(ACH_WIN_ONE_GAME)

    setPresence("Exploring the sandbox")

    -- --- Steam Cloud -----------------------------------------------------------------------
    --
    -- Cloud has TWO switches: the player's account-wide setting and the per-app one. Both must
    -- be on. isCloudEnabled() reports the combination, and every cloud call no-ops when it is
    -- false, so a player who turned Cloud off simply keeps local-only saves.
    if Steam.isCloudEnabled() then
        local total, available = Steam.getCloudQuota()
        print(string.format("[SteamDemo] Cloud quota: %d / %d bytes free", available, total))

        -- `or` works because cloudRead returns nil (not "") when the file is absent.
        local previous = Steam.cloudRead(CLOUD_SETTINGS_FILE) or "{}"
        print("[SteamDemo] Previous cloud settings: " .. previous)

        Steam.cloudWrite(CLOUD_SETTINGS_FILE, '{"difficulty":"normal","runs":1}')

        for i, name in ipairs(Steam.cloudEnumerate()) do
            print(string.format("[SteamDemo] Cloud file %d: %s", i, name))
        end
    else
        print("[SteamDemo] Steam Cloud is disabled for this account or app; saves stay local.")
    end
end

function OnUpdate(ts)
    -- Pause on the rising edge of the overlay, resume on the falling edge. Reads false forever
    -- when Steam is absent, so this branch simply never fires.
    local overlayActive = Steam.isOverlayActive()
    if overlayActive ~= wasOverlayActive then
        if overlayActive then
            print("[SteamDemo] Overlay opened — a real game would pause here.")
            setPresence("Paused")
        else
            print("[SteamDemo] Overlay closed — resuming.")
            setPresence("Exploring the sandbox")
        end
        wasOverlayActive = overlayActive
    end
end

function OnDestroy()
    -- Clearing presence on exit stops the friends list showing a stale "still playing" status.
    Steam.clearRichPresence()
end
