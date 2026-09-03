-- Fires the authored projectile weapon after a short grace period. The bot is
-- rotated toward the player in FPSCombatDemo.olo, so no test-only API is used.
local CombatBot = {}
local elapsed = 0.0

function CombatBot.OnUpdate(id, dt)
    elapsed = elapsed + dt
    local weapon = entity_utils.get_component(id, "WeaponComponent")
    if weapon then
        weapon.fire = elapsed >= 2.0
    end
end

return CombatBot
