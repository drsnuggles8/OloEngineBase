-- DriftWeatherDirector.lua — Drift (issue #882), weather and time of day per leg.
--
-- Attach to the "Atmosphere" entity, which carries ProceduralSkyComponent,
-- TimeOfDayComponent, WeatherStateComponent and CloudscapeComponent. This
-- script owns none of the blending: WeatherSystem cross-blends the authored
-- presets and writes the scene-level Wind/Fog/Precipitation settings itself,
-- and TimeOfDaySystem drives the sun/moon and the sky bake off the clock. All
-- this script does is decide WHICH state and WHAT TIME each leg gets, and
-- couple the sea to the wind the director produced.
--
-- WHAT A "LEG" IS HERE
--   The passage from where the boat is now to the island it is steering at.
--   #881 owns the real discovery loop (landing triggers, markers, saved
--   progress); this script deliberately does not pre-empt it, so a leg ends on
--   the cheapest honest signal available in this scene — the boat closing an
--   island — and each island re-arms once the boat is back outside its own
--   departure radius. Sail out to an island and away again and you get a
--   different sky each time, which is the whole point of the issue.
--
--   SINCE #880 THERE ARE SIX ISLANDS, and each one carries its own arm/disarm
--   state. That is not tidiness: their departure circles overlap on several of
--   the passages between them, so a single global flag driven by "the nearest
--   island" would never re-arm on those routes and every leg there would
--   advance on the 300 s failsafe instead of on arrival. See the loop in
--   OnUpdate.
--
--   The thresholds are DERIVED per island from its own TerrainComponent — the
--   radial island falloff radius times the tile size is roughly where that
--   island's coast falls, and the two thresholds are margins outside it — so a
--   retuned island needs no edit here, and a seventh one needs only its name in
--   kIslandNames. This script used to hard-code one centre and one 240 m
--   half-size, which is exactly the kind of mirror that goes stale on the first
--   move.
--
--   kLegMaxSeconds is a failsafe, not a design: a player who never closes the
--   island still sees the weather move rather than sitting under one sky
--   forever. It is long enough that it never fires on a normal run out.
--
-- WHY THE CLOCK IS WARPED RATHER THAN SET
--   Each leg has a target hour. Snapping TimeOfDayComponent.timeOfDayHours to
--   it is a hard cut in the sky, the lighting and the AtmosphereSky bake all at
--   once — exactly the "jarring transition" the acceptance criteria rule out.
--   Instead the clock is DRIVEN forward through the intervening hours over
--   kTimeWarpSeconds, always the short way FORWARD (time does not run
--   backwards). The sun visibly sweeps while you are out on the water, which
--   reads as the crossing taking hours, and the sky rebake follows it in
--   RebakeQuantumGameMinutes steps instead of one pop.
--
-- ORDER-OF-TICK NOTE
--   The gameplay scheduler runs "Scripts" before "Weather" (Scene.cpp), so the
--   wind speed this script reads is the one the director blended LAST tick. At
--   the 25 s sea-state time constant below that is invisible, and reading a
--   settled value is what keeps this script out of a feedback loop with the
--   blend it is watching.

local WeatherDirector = {}

-- ── The schedule ────────────────────────────────────────────────────────────
-- Deterministic and short on purpose: it is a demo reel, not a simulation.
-- Four legs, each visibly a different hour AND a different sky, cycling.
-- Storm at dusk is the store-page frame — a squall at low sun over a rough sea
-- is where this engine's ocean, cloud shadowing and precipitation all read at
-- once.
--
-- THE HOURS ARE DERIVED, NOT CHOSEN. They are the hours at which Drift.olo's
-- authored latitude (34) and day of year (172) put the sun where the name says
-- it is, read off the live ephemeris rather than guessed:
--     5.2h -> +3.7 deg   12.0h -> +79.4 deg
--    18.9h -> +2.6 deg   22.6h -> -29.2 deg (night)
-- The first draft used round-ish numbers (6.6 / 17.4) and both rendered as flat
-- mid-morning light, because at this latitude in June the sun is already 20 deg
-- up by 6.6h. If the latitude or the day of year on the component changes,
-- these MUST be re-derived — nothing checks them, and the failure is a "dawn"
-- leg that simply looks like another midday one.
local kLegs = {
    { name = "Clear dawn",      weather = "Clear",    hour = 5.2  },
    { name = "Overcast noon",   weather = "Overcast", hour = 12.0 },
    { name = "Dusk squall",     weather = "Storm",    hour = 18.9 },
    { name = "Clearing night",  weather = "Clear",    hour = 22.6 },
}

-- Weather cross-blend length. Long enough that no single frame shows a step in
-- the fog, the cloud deck or the rain; short enough to land well inside a leg.
local kTransitionSeconds = 14.0
-- Clock warp length. Deliberately a little longer than the weather blend so the
-- two do not finish together and read as one scripted event.
local kTimeWarpSeconds   = 18.0

-- The six islands #880 scattered across the sea. The only thing mirrored from
-- the scene is the NAME; everything else about each one — where it is, how big
-- it is, where its coast falls — is read off its own entity.
local kIslandNames = {
    "Island - Ridgeback",
    "Island - Mesa",
    "Island - Atoll",
    "Island - Stacks",
    "Island - Dunes",
    "Island - Sisters",
}
local kBoatName      = "Boat"
local kSeaName       = "Sea"

-- Margins outside an island's own coast, not absolute distances: the six run
-- from a 260 m tile to a 420 m one, so one fixed radius would be inshore of the
-- big islands and half a kilometre off the small ones.
local kLandfallMargin   = 45.0   -- m outside the coast: close enough to count as arriving
local kDepartureMargin  = 135.0  -- must clear this again before a leg can end
local kLegMaxSeconds    = 300.0  -- failsafe only

-- ── Sea state ───────────────────────────────────────────────────────────────
-- The three anchors are the calm / moderate / rough sets #879 authored and
-- play-tested by hand (see the SEA STATE block in Drift.olo); this script
-- interpolates between them instead of inventing new numbers, so the boat's
-- feel at each anchor is the feel that was signed off.
--
-- The wind speeds are the WeatherPreset WindSpeed values the director blends
-- toward, as authored on Drift.olo's Atmosphere entity: Clear 2 m/s,
-- Overcast 5, Storm 14. Change one there and the matching anchor here moves
-- with it — the mapping is what couples them, not a shared constant.
local kWindCalm  = 2.0   -- m/s → anchor 1
local kWindMod   = 5.0   -- m/s → anchor 2
local kWindRough = 14.0  -- m/s → anchor 3

-- A sea does not build or lie down with the wind — it lags it by a long way,
-- and that lag is also what keeps the transition from popping. 25 s is short
-- enough to be inside one leg and long enough to read as inertia.
local kSeaTau = 25.0

local kSea = {
    -- t = 0 (calm)                t = 0.5 (moderate)            t = 1 (rough)
    -- DO NOT RAISE THESE without fixing the mesh first — measured in #943.
    -- They look absurdly small for a sea state, and the tempting reading is that
    -- they were only ever this low to hide a shading bug (WaterCommon.glsl used
    -- to build each octave's normal from an UNSCALED steepness, so WaveAmplitude
    -- did not affect shading normals at all). That bug is fixed, but raising
    -- these still does not work, for a second and independent reason:
    --
    -- sumGerstnerWaves runs an octave ladder down to 0.09 * avgWL — about 2 m at
    -- this scene's wavelengths — while the surface mesh is 640 quads across
    -- 1600 m, i.e. 2.5 m per quad. The finest octaves are at or below the mesh's
    -- Nyquist limit, so the mesh cannot represent them and the surface breaks
    -- into visible flat facets. Swept live at the chase camera: clean at 0.22,
    -- facets clearly present at 0.32, and the facet SIZE tracks GridResolution
    -- (80 -> big facets, 1600 -> small ones), which is what identifies them as
    -- geometry rather than shading.
    --
    -- So ~0.25 is the ceiling this mesh supports, and 0.22 already sits at it.
    -- To get a genuinely bigger sea, band-limit the octave ladder to the vertex
    -- spacing (or enable tessellation) — then these can go up.
    waveAmplitude      = { 0.05,   0.12,   0.22  },
    waveSpeed          = { 0.80,   1.00,   1.30  },
    -- Whitecaps start lower down the wave and burn brighter as it builds: this
    -- is the single most legible "the sea got up" cue in a still frame.
    foamHeightStart    = { 0.26,   0.16,   0.075 },
    foamBrightness     = { 0.85,   1.10,   0.90  },
    -- How much of the sea is ALLOWED to break, before the height/angle
    -- terms narrow it further (#943). Until that issue this was hardcoded
    -- at the 0.12 equivalent for every sea state, which is why a storm
    -- carried about 1% actual foam and could not read as rougher than a
    -- calm. 0.12 keeps calm exactly as it was.
    foamCoverage       = { 0.12,   0.30,   0.62  },
    foamFadeDistance   = { 0.45,   0.35,   0.22  },
    -- A rough sea is a worse mirror: the specular track breaks up into glitter.
    specularIntensity  = { 1.70,   1.40,   0.85  },
    noiseIntensity     = { 0.45,   0.65,   0.95  },
    -- ...and it reads colder and greyer, because it is carrying air.
    waterColorR        = { 0.110,  0.090,  0.028 },
    waterColorG        = { 0.400,  0.330,  0.075 },
    waterColorB        = { 0.530,  0.440,  0.105 },
    -- Kept coherent even though Drift runs the Gerstner path (UseFFT is false
    -- for the reasons in Drift.olo / issue #898): if the FFT surface is ever
    -- switched back on, the sea state must not silently stop tracking the wind.
    fftWindSpeed       = { 4.00,   8.00,   16.00 },
    fftAmplitude       = { 0.28,   0.55,   0.95  },
}

-- ── State ───────────────────────────────────────────────────────────────────
local legIndex   = 0
local legElapsed = 0.0
-- Arming is PER ISLAND (see resolveIslands): one global flag cannot work with
-- six of them, because their departure circles overlap on several passages.
local warp       = nil     -- { from, to, t } while the clock is being driven
local seaT       = nil     -- eased sea-state parameter in [0,1]
local boatID, seaID = nil, nil
-- One entry per island once resolved:
--   { centre = { x, z }, landfall, departure, armed }
local islands = nil
local warnedIsland, warnedSea = false, false

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function blend(tau, dt)
    if tau <= 0.0 or dt <= 0.0 then return 1.0 end
    return 1.0 - math.exp(-dt / tau)
end

-- Piecewise-linear through the three authored anchors at t = 0, 0.5, 1.
local function anchor3(a, t)
    if t <= 0.5 then
        local u = t * 2.0
        return a[1] + (a[2] - a[1]) * u
    end
    local u = (t - 0.5) * 2.0
    return a[2] + (a[3] - a[2]) * u
end

-- The inverse of anchor3 for a strictly increasing anchor set. Used once, on
-- the first tick, to work out which sea state the SCENE was authored at so the
-- ease can start from what is already on screen. Seeding from the wind's target
-- instead would snap the whole surface on frame 1 of Play — the scene authors
-- the moderate sea and leg 1 is calm, so the pop is guaranteed, and a pop on
-- the first frame is the one transition a player is certain to be looking at.
local function inverseAnchor3(a, v)
    if v <= a[1] then return 0.0 end
    if v >= a[3] then return 1.0 end
    if v <= a[2] then
        return 0.5 * (v - a[1]) / (a[2] - a[1])
    end
    return 0.5 + 0.5 * (v - a[2]) / (a[3] - a[2])
end

-- Hours from `from` to `to` going FORWARD around a 24 h clock, in [0, 24).
-- Time never runs backwards, so a target behind the current hour is a wrap all
-- the way round.
local function forwardHours(from, to)
    local d = to - from
    while d < 0.0 do d = d + 24.0 end
    return d
end

-- A warp is only worth running if the target is a real journey AHEAD. Two
-- degenerate cases both have to collapse to "no warp", and only one of them is
-- obvious:
--
--   * the target IS the current hour (leg 1, whose hour is what Drift.olo
--     authors the clock at) — forwardHours returns 0;
--   * the target is a little way BEHIND, because the component's own clock has
--     free-run past it while the leg was under way. At 30 real minutes per game
--     day the clock covers 0.8 game-hours per real minute, so a leg that runs
--     to its 300 s failsafe drifts ~6.7 h — comfortably past leg 3's 18.9 h
--     target and into leg 4's 22.6 h. forwardHours(22.66, 22.6) is 23.94, and
--     warping that is a full sunrise-to-sunrise sweep in 18 seconds: the single
--     most jarring thing this script could do, in the one place nobody would
--     look for it.
--
-- So anything within kWarpDeadZone hours either side of "already there" is
-- treated as arrived, and the clock is simply left to run.
local kWarpDeadZone = 1.0 -- hours

local function warpSpan(from, to)
    local d = forwardHours(from, to)
    if d < 1e-3 or d > (24.0 - kWarpDeadZone) then
        return nil
    end
    return d
end

local function componentOf(entityID, name)
    if not entityID then return nil end
    return entity_utils.get_component(entityID, name)
end

local function beginLeg(id, index)
    legIndex   = index
    legElapsed = 0.0

    local leg = kLegs[legIndex]

    local weather = componentOf(id, "WeatherStateComponent")
    if weather then
        weather.transitionDuration = kTransitionSeconds
        weather.targetState = leg.weather
    end

    local tod = componentOf(id, "TimeOfDayComponent")
    if tod then
        local from = tod.timeOfDayHours
        local span = warpSpan(from, leg.hour)
        -- nil span: already at (or just past) the hour, so leave the
        -- component's own clock alone rather than driving a warp.
        warp = span and { from = from, span = span, t = 0.0 } or nil
    end

    Log.Info("[Drift] Leg " .. tostring(legIndex) .. " — " .. leg.name ..
             " (" .. leg.weather .. ", " .. string.format("%.1f", leg.hour) .. "h)")
end

function WeatherDirector.OnCreate(id)
    legIndex, legElapsed = 0, 0.0
    warp, seaT = nil, nil
    boatID, seaID = nil, nil
    islands = nil
    warnedIsland, warnedSea = false, false

    -- The wind blows along the authored swell (WaveDir0 in Drift.olo), so wind
    -- and sea agree. Per-leg wind DIRECTION belongs with the sailing model in
    -- #899, not here: rotating it without also rotating the wave trains would
    -- put the whitecaps across the wind, which reads worse than a fixed wind.
    Scene.SetWindEnabled(true)
    Scene.SetWindDirection(vec3.new(0.989, 0.0, 0.148))

    beginLeg(id, 1)
end

-- Resolve every island once: centre from the tile's CORNER origin plus half its
-- world size, and the two thresholds from that island's own coast.
--
-- ESTIMATING THE COAST. The radial island falloff (#880) does not put the
-- coastline at IslandFalloffRadius — that is where the mask STARTS falling. It
-- reaches zero at the tile's inscribed circle (half the world size), and land
-- survives anywhere between the two that the noise left high enough. Measured
-- across all six islands, the waterline lands close to the midpoint of that
-- band, and the midpoint estimator is within ~10% on every one of them
-- (Ridgeback 168 predicted / 165-173 measured, Sisters 101 / 97-102, Stacks
-- 114 / 106-113). Both terms are read off the component, so retuning an island
-- in the editor moves the leg thresholds with it.
--
-- Returns nil until EVERY named island has resolved, so a partially-loaded
-- scene retries next tick instead of caching a short list and quietly ending
-- legs on the wrong subset.
local function resolveIslands()
    local resolved = {}
    for _, name in ipairs(kIslandNames) do
        local id = entity_utils.find_by_name(name)
        if not id then
            return nil
        end
        local terrain = entity_utils.get_component(id, "TerrainComponent")
        if not terrain then
            return nil
        end
        local t = entity_utils.get_translation(id)
        local coast = (terrain.islandFalloffRadius + 0.5) * 0.5 * terrain.worldSizeX
        resolved[#resolved + 1] = {
            centre    = { x = t.x + terrain.worldSizeX * 0.5, z = t.z + terrain.worldSizeZ * 0.5 },
            landfall  = coast + kLandfallMargin,
            departure = coast + kDepartureMargin,
            -- Armed on resolve: the boat starts on open water, not on a beach,
            -- so the first approach to any island is a genuine landfall.
            armed     = true,
        }
    end
    return resolved
end

function WeatherDirector.OnUpdate(id, dt)
    if dt <= 0.0 then return end
    legElapsed = legElapsed + dt

    -- ── Clock warp ──────────────────────────────────────────────────────────
    -- While a warp is live this script OWNS timeOfDayHours: the component's own
    -- clock is still advancing underneath (it is not paused, so the sky keeps
    -- moving between legs), and the assignment below simply wins for the frame.
    local tod = componentOf(id, "TimeOfDayComponent")
    if tod and warp then
        warp.t = clamp(warp.t + dt / kTimeWarpSeconds, 0.0, 1.0)
        -- Smoothstep, matching WeatherSystem::EaseSmoothstep, so the sun eases
        -- into and out of the sweep rather than starting and stopping dead.
        local e = warp.t * warp.t * (3.0 - 2.0 * warp.t)
        -- The setter wraps past 24 h, so no modulo is needed here.
        tod.timeOfDayHours = warp.from + warp.span * e
        if warp.t >= 1.0 then warp = nil end
    end

    -- ── Sea state follows the wind ──────────────────────────────────────────
    if not seaID then seaID = entity_utils.find_by_name(kSeaName) end
    local water = componentOf(seaID, "WaterComponent")
    if not water then
        if not warnedSea then
            warnedSea = true
            Log.Warn("[Drift] DriftWeatherDirector found no WaterComponent on '" ..
                     kSeaName .. "' — the sea will not track the wind.")
        end
    else
        local windSpeed = Scene.GetWindSpeed()
        local target
        if windSpeed <= kWindMod then
            target = 0.5 * clamp((windSpeed - kWindCalm) / (kWindMod - kWindCalm), 0.0, 1.0)
        else
            target = 0.5 + 0.5 * clamp((windSpeed - kWindMod) / (kWindRough - kWindMod), 0.0, 1.0)
        end

        if seaT == nil then
            -- Start from the sea the scene is authored at, then ease toward
            -- the wind's target like any other change.
            seaT = inverseAnchor3(kSea.waveAmplitude, water.waveAmplitude)
        end
        seaT = seaT + (target - seaT) * blend(kSeaTau, dt)

        water.waveAmplitude     = anchor3(kSea.waveAmplitude, seaT)
        water.waveSpeed         = anchor3(kSea.waveSpeed, seaT)
        water.foamCoverage      = anchor3(kSea.foamCoverage, seaT)
        water.foamHeightStart   = anchor3(kSea.foamHeightStart, seaT)
        water.foamBrightness    = anchor3(kSea.foamBrightness, seaT)
        water.foamFadeDistance  = anchor3(kSea.foamFadeDistance, seaT)
        water.specularIntensity = anchor3(kSea.specularIntensity, seaT)
        water.noiseIntensity    = anchor3(kSea.noiseIntensity, seaT)
        water.fftWindSpeed      = anchor3(kSea.fftWindSpeed, seaT)
        water.fftAmplitude      = anchor3(kSea.fftAmplitude, seaT)
        water.waterColor        = vec3.new(anchor3(kSea.waterColorR, seaT),
                                       anchor3(kSea.waterColorG, seaT),
                                       anchor3(kSea.waterColorB, seaT))
    end

    -- The cloud deck runs harder in a blow too. Coverage/type/wetness are the
    -- director's to set (it overrides them every tick while a
    -- WeatherStateComponent is enabled); the advection scale is not, so it is
    -- the one cloud knob a script may safely drive.
    if seaT then
        local clouds = componentOf(id, "CloudscapeComponent")
        if clouds then
            clouds.windAnimationScale = 0.7 + 2.1 * seaT
        end
    end

    -- ── Leg advance ─────────────────────────────────────────────────────────
    if not boatID then boatID = entity_utils.find_by_name(kBoatName) end
    if not islands then islands = resolveIslands() end

    local landfall = false
    if islands and boatID then
        local b = entity_utils.get_translation(boatID)
        -- ARMING IS PER ISLAND, and that is the whole reason this loop is not a
        -- nearest-island test. With one island a single flag was enough. With
        -- six, the departure circles OVERLAP on several of the passages between
        -- them — Ridgeback's reaches 303 m and Sisters' landfall ring is 266 m
        -- from Ridgeback's centre — so a boat crossing from one to the other is
        -- never outside "the nearest island's" departure radius, never arms, and
        -- every leg on that route advances on the 300 s failsafe instead. Per
        -- island there is no coupling at all: each one only ever compares the
        -- boat against its own two radii, exactly as the single-island version
        -- did.
        for _, isle in ipairs(islands) do
            local dx, dz = b.x - isle.centre.x, b.z - isle.centre.z
            local dist = math.sqrt(dx * dx + dz * dz)
            if dist > isle.departure then
                isle.armed = true
            elseif isle.armed and dist < isle.landfall then
                isle.armed = false
                landfall = true
            end
        end
    elseif not warnedIsland then
        warnedIsland = true
        Log.Warn("[Drift] DriftWeatherDirector could not resolve the islands (or '" .. kBoatName ..
                 "') — legs will only advance on the timeout.")
    end

    if landfall or legElapsed >= kLegMaxSeconds then
        beginLeg(id, (legIndex % #kLegs) + 1)
    end
end

function WeatherDirector.OnDestroy(id)
end

return WeatherDirector
