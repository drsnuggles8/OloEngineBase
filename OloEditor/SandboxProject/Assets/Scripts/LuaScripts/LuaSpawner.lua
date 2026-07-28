-- LuaSpawner.lua
-- Runtime spawning demo (issue #643) — the Lua half of the C#/Lua parity pair;
-- Source/Spawner.cs is the C# twin and does the same thing.
--
-- Attach (via LuaScriptComponent) to any entity. Every SPAWN_INTERVAL seconds
-- it spawns one entity in a ring around itself, and once RING_SIZE are alive it
-- retires the oldest — the classic projectile / wave-spawner shape, in
-- miniature. Watch the Console for "[LuaSpawner]".
--
-- Set PREFAB_PATH to spawn a prefab (with its whole child hierarchy) instead of
-- a bare entity. Path lookup is EDITOR-ONLY: a packed runtime serves assets by
-- handle and has no path index, so a shipping script should carry the handle
-- (e.g. off an authored template entity's PrefabComponent.prefabID) rather than
-- resolve a path at runtime. This demo resolves once, in OnCreate, and warns if
-- it can't.
--
-- ── The one rule that shapes this script ────────────────────────────────────
-- Spawns and destroys are DEFERRED. The engine is iterating the script
-- component pools while your OnUpdate runs, so creating or destroying an entity
-- right there would invalidate that iteration. Instead the request is queued
-- and applied once every script's OnUpdate has returned this tick. That means:
--
--   * the id you get back is the entity's real, final id — store it, pass it
--     around, entity_utils.is_valid already says true for it,
--   * you cannot read or write its components until the NEXT tick, so pass the
--     spawn position to the call instead of assigning it afterwards,
--   * by the time physics and rendering run later in the same tick, the entity
--     is fully live — the spawn shows up in the same frame.

local M = {}

local SPAWN_INTERVAL = 0.5   -- seconds between spawns
local RING_SIZE      = 8     -- how many spawned entities stay alive
local RADIUS         = 3.0   -- spawn ring radius around this entity
local PREFAB_PATH    = nil   -- e.g. "Prefabs/Projectile.oprefab"; nil = bare entity

local timer   = 0.0
local angle   = 0.0
local spawned = {}           -- ring buffer of spawned entity ids
local head    = 1
local count   = 0
local prefabHandle = 0

function M.OnCreate(id)
    if PREFAB_PATH then
        prefabHandle = Scene.FindPrefabByPath(PREFAB_PATH)
        if prefabHandle == 0 then
            Log.Warn("[LuaSpawner] Could not resolve '" .. PREFAB_PATH ..
                     "' — falling back to bare entities.")
        end
    end
    Log.Info("[LuaSpawner] Ready — every " .. SPAWN_INTERVAL .. "s, keeping " ..
             RING_SIZE .. " alive.")
end

local function next_ring_position(id)
    angle = angle + 0.7
    local origin = entity_utils.get_translation(id)
    return vec3.new(origin.x + math.cos(angle) * RADIUS,
                    origin.y,
                    origin.z + math.sin(angle) * RADIUS)
end

local function spawn_one(position)
    if prefabHandle ~= 0 then
        -- The prefab's whole hierarchy arrives; the transform we pass replaces
        -- the prefab's authored one. Destroying the root later takes the
        -- children with it.
        return Scene.Instantiate(prefabHandle, position)
    end
    -- A bare entity: transform + tag only. Add components to it from the NEXT
    -- tick onwards, once the drain has created it.
    return Scene.CreateEntity("SpawnedByLua", position)
end

function M.OnUpdate(id, ts)
    timer = timer + ts
    if timer < SPAWN_INTERVAL then
        return
    end
    timer = 0.0

    -- Retire the oldest before spawning its replacement so the live count never
    -- overshoots. Both requests land in the same drain, in this order.
    if count == RING_SIZE then
        local oldest = spawned[head]
        if oldest then
            -- Safe to call on something already gone — a repeat destroy is a
            -- no-op, not a double free.
            Scene.DestroyEntity(oldest)
        end
        spawned[head] = nil
        count = count - 1
    end

    local newID = spawn_one(next_ring_position(id))
    if newID == 0 then
        Log.Warn("[LuaSpawner] Spawn request was rejected.")
        return
    end

    spawned[head] = newID
    head = (head % RING_SIZE) + 1
    count = count + 1
end

function M.OnDestroy(id)
    -- Take the spawned entities with us. The engine drains whatever is still
    -- queued at runtime stop, so this is best-effort tidiness rather than a
    -- leak guard.
    for i = 1, RING_SIZE do
        if spawned[i] then
            Scene.DestroyEntity(spawned[i])
            spawned[i] = nil
        end
    end
    count = 0
end

return M
