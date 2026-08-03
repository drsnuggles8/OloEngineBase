-- NetworkDemoController.lua — the playable 2-client server-authoritative demo.
--
-- Attach via LuaScriptComponent with
--   ScriptFile = "Scripts/LuaScripts/NetworkDemoController.lua"
-- on the NetworkDemo scene's "NetworkDemoController" entity.
--
-- HOW TO RUN THE DEMO
--   1. Start a dedicated server:
--        OloServer.exe --scene SandboxProject/Assets/Scenes/NetworkDemo.olo --port 7777
--      (or press F9 in one client to host a listen server in-process).
--   2. Launch two clients (OloEditor Play, or OloRuntime) and press F10 in each.
--   3. Each client gets its own coloured square. WASD moves YOUR square; the other
--      client's square moves too, driven entirely by the server's snapshots.
--
-- The scene is 2D (orthographic camera + sprites, no asset imports). Switch the
-- editor to 2D mode before entering Play or the sprites will not be drawn — the
-- 2D/3D flag is an editor/runtime setting, not something the .olo file carries.
--
-- WHAT THIS SCRIPT DOES *NOT* DO, deliberately: it never moves the pawn itself.
-- It only submits input. The square you see move locally has been moved by the
-- prediction path applying the very same input command the server will apply, so
-- if the two ever disagree the correction is visible instead of hidden behind a
-- second, script-local movement implementation.

local NetworkDemoController = {}

local kPort = 7777
local kHost = "127.0.0.1"
local kMoveSpeed = 4.0
-- Must be >= kMoveSpeed * dt for any dt the game runs at, or the server's clamp
-- would silently throttle legitimate movement. 4 units/s at 10 fps = 0.4.
local kMaxStepDistance = 0.5

local announcedPawn = false

function NetworkDemoController.OnCreate(id)
    -- Install the engine's built-in movement command on BOTH the prediction path
    -- and the server's authoritative path. One registration, one implementation.
    Network.useMovementInput(kMaxStepDistance)

    -- A multicast RPC so the demo shows the RPC path working end to end: any
    -- client can ask the server to announce something, and every client (plus the
    -- server) prints it.
    Network.registerRPC("Demo.Announce", { target = "multicast", reliability = "reliable" },
        function(ctx, args)
            Log.Info("[NetworkDemo] announce: " .. tostring(args[1]) ..
                     " (server=" .. tostring(ctx.isServer) .. ")")
        end)

    -- Server-target RPC: a client asks the server to reset its own pawn to the
    -- origin. requiresOwnership means the server refuses a client that names
    -- someone else's pawn — the authority check, not a client-side courtesy.
    Network.registerRPC("Demo.ResetPawn", { target = "server", reliability = "reliable",
                                            requiresOwnership = true },
        function(ctx, args)
            local t = entity_utils.get_component(ctx.entityID, "TransformComponent")
            if t then
                t.translation = vec3.new(0.0, 0.0, 0.0)
                Log.Info("[NetworkDemo] reset pawn for client " .. tostring(ctx.senderClientID))
            end
        end)

    Log.Info("[NetworkDemo] F9 = host on " .. kPort ..
             ", F10 = connect to " .. kHost .. ":" .. kPort ..
             ", WASD = move, R = reset pawn, T = announce")
end

function NetworkDemoController.OnUpdate(id, dt)
    -- Session control ------------------------------------------------------
    if Input.IsKeyJustPressed(KeyCode.F9) and not Network.isServer() then
        if Network.startServer(kPort) then
            Log.Info("[NetworkDemo] hosting on port " .. kPort)
        else
            Log.Warn("[NetworkDemo] failed to host on port " .. kPort)
        end
    end

    if Input.IsKeyJustPressed(KeyCode.F10) and not Network.isClient() then
        if Network.connect(kHost, kPort) then
            Log.Info("[NetworkDemo] connecting to " .. kHost .. ":" .. kPort)
        else
            Log.Warn("[NetworkDemo] failed to start connecting")
        end
    end

    -- Our pawn is spawned by the SERVER, one per connection, so it appears a
    -- round trip after connecting rather than immediately.
    local pawn = Network.getLocalPlayerEntity()
    if pawn == 0 then
        announcedPawn = false
        return
    end

    if not announcedPawn then
        announcedPawn = true
        Log.Info("[NetworkDemo] pawn " .. tostring(pawn) ..
                 " assigned to client " .. tostring(Network.getLocalClientID()))
    end

    -- Movement -------------------------------------------------------------
    local vx, vy = 0.0, 0.0
    if Input.IsKeyDown(KeyCode.W) then vy = vy + 1.0 end
    if Input.IsKeyDown(KeyCode.S) then vy = vy - 1.0 end
    if Input.IsKeyDown(KeyCode.A) then vx = vx - 1.0 end
    if Input.IsKeyDown(KeyCode.D) then vx = vx + 1.0 end

    if vx ~= 0.0 or vy ~= 0.0 then
        -- Normalise so diagonals are not faster, then bake the whole step here.
        -- The displacement (not a direction + speed) is what travels, because
        -- reconciliation replays these commands with no timeline of their own.
        local length = math.sqrt(vx * vx + vy * vy)
        local step = kMoveSpeed * dt
        Network.sendMoveInput(pawn, vx / length * step, vy / length * step, 0.0)
    end

    -- RPCs -----------------------------------------------------------------
    if Input.IsKeyJustPressed(KeyCode.R) then
        Network.invokeRPC("Demo.ResetPawn", pawn)
    end

    if Input.IsKeyJustPressed(KeyCode.T) then
        -- Multicast originates on the server, so a pure client asking for one is
        -- refused. On a listen server (F9) this announces to everybody.
        Network.invokeRPC("Demo.Announce", 0, { "hello from client " .. tostring(Network.getLocalClientID()) })
    end
end

return NetworkDemoController
