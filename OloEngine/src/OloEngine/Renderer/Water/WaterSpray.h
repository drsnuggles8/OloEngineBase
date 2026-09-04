#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Renderer/Water/WaterFoam.h"
#include "OloEngine/Renderer/Water/WaterRainRipples.h"

#include <glm/glm.hpp>

#include <cmath>
#include <numeric>
#include <vector>

namespace OloEngine::WaterSpray
{
    // =========================================================================
    // THE SPRAY EMISSION CONTRACT (issue #1034, §2.3)
    //
    // Crests that fold hard enough throw bubbles and spray. This header owns
    // the decision of WHERE and HOW MUCH; WaterSpraySystem owns the GPU
    // particle system it feeds.
    //
    // ---- 1. There is only ONE crest detector ---------------------------------
    //
    // The issue is explicit that spray and foam advection must not each grow
    // their own crest test, and they do not: `Emit` below calls
    // WaterFoam::DepositFromFold — the SAME function, on the SAME fold signal
    // (the FFT's saturate(1 - Jacobian)) — that compute/WaterDisturbance_Update
    // .comp calls to lay foam down. A crest that spawns spray is exactly a
    // crest that deposits foam, by construction rather than by tuning.
    //
    // What this does NOT do is read the foam TEXTURE. That would be a
    // GPU->CPU readback every frame, i.e. a full pipeline stall, to learn
    // something it can evaluate directly from the ocean field it already has a
    // CPU copy of (OceanFFTField retains one so buoyancy can float without a
    // readback). Sharing the criterion is the requirement; sharing the storage
    // would be a performance bug wearing the requirement's clothes.
    //
    // ---- 2. Why the sample points are a WORLD-ANCHORED grid -------------------
    //
    // Candidate crests are sampled on a jittered lattice anchored at the world
    // origin, not on a random scatter and not on a camera-relative pattern.
    // Both alternatives are worse in the same way: they make WHERE spray
    // appears depend on the camera, so flying the camera over a fixed sea
    // re-rolls which crests are spraying and the whole effect crawls. Anchoring
    // at the origin means a crest either sprays or does not, and moving the
    // camera only changes which ones are close enough to be sampled.
    //
    // It also makes the whole thing deterministic under a mocked clock, which
    // is what lets a golden capture of spray exist at all.
    //
    // ---- 3. Why emission is probabilistic rather than one-per-cell ------------
    //
    // A cell above the threshold emits `rate * dt * deposit` particles, which
    // is almost always well under one. Rounding that to zero would emit nothing
    // at any sane rate; rounding it to one would make emission scale with the
    // FRAME RATE, so a 120 Hz machine would get twice the spray — the same
    // defect WaterDisturbance::CombineSplat's `max` exists to avoid on the foam
    // side. The fractional part is resolved against a hash of (cell, time
    // bucket), so it is unbiased AND reproducible.
    // =========================================================================

    /// Metres between candidate crest samples. 2 m is a little under the
    /// spacing of individual whitecaps on a built sea, so a breaking crest
    /// lands in two or three cells and reads as a sheet rather than as points.
    inline constexpr f32 kSampleSpacingMetres = 2.0f;

    /// Hard cap on candidate cells visited per frame, whatever the radius asks
    /// for. Each one is a CPU cascade sample, so this is the knob that keeps a
    /// mis-authored radius from turning into a frame-time cliff rather than a
    /// visual one.
    inline constexpr u32 kMaxSampleCells = 640;

    /// Hard cap on particles emitted per frame. Spray is a garnish; a scene
    /// that would emit more than this is mis-tuned, and silently eating the
    /// excess is better than stalling on an upload nobody asked for.
    inline constexpr u32 kMaxEmitPerFrame = 256;

    /// Metres a droplet is spawned ABOVE the surface the CPU proxy reports.
    ///
    /// Not a fudge — a correction for a documented difference between the two
    /// halves of the ocean. In GPU-compute mode OceanFFTField retains
    /// BAND-LIMITED CPU proxies (<=64^2, "the same waves minus sub-metre
    /// detail") so physics can sample without a readback, while the shader
    /// draws the full-resolution field. The proxy therefore UNDER-REPORTS crest
    /// height by up to that missing detail, and a droplet spawned a few
    /// centimetres above it starts BELOW the surface actually drawn — where the
    /// depth test eats it. Every counter reads healthy; the screen shows
    /// nothing.
    ///
    /// Measured the direct way: at a 30 m/s launch (which clears the surface
    /// before the discrepancy matters) droplets render; at the default 2.6 m/s
    /// they did not, until this clearance was added.
    inline constexpr f32 kSpawnClearanceMetres = 0.5f;

    /// Seconds per emission time bucket. The per-cell hash is keyed on the
    /// bucket index, so a cell's coin flip is stable within a bucket and
    /// re-rolled between them — which is what stops a marginal cell flickering
    /// on and off every frame at 60 Hz.
    inline constexpr f32 kTimeBucketSeconds = 0.1f;

    /// Largest multiple of the authored lifetime a droplet can actually be
    /// given. Emit() jitters by `0.7 + 0.6 * r` with r in [0, 1), so a droplet
    /// can outlive its nominal lifetime by 30%.
    ///
    /// Named because the DRAIN window has to be at least this: a shorter one
    /// stops simulating while the longest-lived droplets of the last active
    /// frame are still in the air, and they freeze there.
    inline constexpr f32 kLifetimeJitterMax = 1.3f;

    /// One candidate crest, as the emitter needs it.
    ///
    /// All three fields describe the surface at one PARAMETER position, which
    /// is not where that surface is drawn: the choppy displacement moves it to
    /// `parameter + m_Horizontal`. On a choppiness-1.6 sea that is metres, so a
    /// droplet spawned at the parameter position is at the right height for the
    /// wrong place — under the surface as often as not, where the depth test
    /// eats it and the whole effect renders as nothing while every emission
    /// counter reads healthy.
    struct CrestSample
    {
        f32 m_Fold = 0.0f;              ///< the FFT's saturate(1 - Jacobian) here
        f32 m_Height = 0.0f;            ///< surface height above the water plane, metres
        glm::vec2 m_Horizontal{ 0.0f }; ///< choppy displacement, world axes, metres
    };

    /// Scene-level spray controls, published from the dominant WaterComponent.
    struct WaterSpraySettings
    {
        bool m_Enabled = false;
        /// Fold signal a crest must exceed to spray. Shares its meaning — and
        /// should usually share its value — with
        /// WaterFoamSettings::m_DepositThreshold; they are the same criterion.
        /// 0.22 against a measured storm peak of 0.345 — high enough that only
        /// the hardest folds throw droplets, low enough that they ever do. The
        /// first value here was 0.45, ABOVE anything a real sea reaches, and
        /// the feature silently never fired.
        f32 m_Threshold = 0.22f;
        /// Particles per second from a cell at full fold. The per-cell rate,
        /// not a scene total, so widening the radius adds spray rather than
        /// thinning what is already there.
        f32 m_RatePerCell = 6.0f;
        /// How far from the camera crests are sampled, metres.
        f32 m_RadiusMetres = 38.0f;
        /// Initial upward speed of a spray particle, m/s.
        f32 m_LaunchSpeed = 2.6f;
        /// Seconds a spray particle lives.
        ///
        /// Sized to the ballistic arc it actually flies: at the default launch
        /// speed a droplet is back at the surface in about 0.6 s, so most of a
        /// longer life is spent sitting on the water pretending to be foam.
        f32 m_Lifetime = 0.9f;
        /// Billboard size, metres.
        ///
        /// A PUFF, not a droplet. An individual spray droplet is millimetres
        /// across and subtends well under a pixel at any distance you would see
        /// spray from — 0.12 m at 30 m is already only ~4 px at 720p, which
        /// disappears into the foam it sits on. Each billboard stands for a
        /// cluster, which is what every real-time spray effect draws.
        f32 m_ParticleSize = 0.25f;
        /// Mean wind, world XZ, m/s — spray is thrown downwind, and this is the
        /// same wind the ocean field was built from for the reason
        /// WaterFoamSettings::m_DriftMetresPerSecond gives.
        glm::vec2 m_WindMetresPerSecond{ 0.0f };
        /// World Y of the water plane the crests sit on.
        f32 m_WaterPlaneY = 0.0f;
        /// The artist multiplier already applied to the FFT height
        /// (WaterComponent::m_FFTHeightScale), so a spray particle launches
        /// from the crest the SHADER drew rather than from the raw field.
        f32 m_HeightScale = 1.0f;
    };

    /// Spray threshold that always sits at or above the foam deposit threshold.
    ///
    /// Not merely a clamp: spray is the LOUDER half of the same phenomenon, so
    /// a scene that authored spray to fire on gentler crests than foam is
    /// deposited on would show droplets flying off water that never went white,
    /// which reads as a bug in the foam rather than as a choice about spray.
    [[nodiscard("the threshold is the only effect")]]
    inline f32 EffectiveThreshold(f32 sprayThreshold, f32 foamThreshold) noexcept
    {
        const f32 spray = std::isfinite(sprayThreshold) ? glm::clamp(sprayThreshold, 0.0f, 0.99f) : 0.22f;
        const f32 foam = std::isfinite(foamThreshold) ? glm::clamp(foamThreshold, 0.0f, 0.99f) : 0.10f;
        return glm::max(spray, foam);
    }

    /// Lower corner, in sample cells, of the square of candidates around
    /// `centreXZ`, and the number of cells per axis. World-anchored: the cell
    /// lattice never moves, only the window over it does.
    [[nodiscard("the cell window is the only effect")]]
    inline glm::ivec2 CellMinForCentre(glm::vec2 centreXZ, f32 radiusMetres, i32& outCellsPerAxis) noexcept
    {
        const f32 radius = std::isfinite(radiusMetres) ? glm::clamp(radiusMetres, 1.0f, 400.0f) : 38.0f;
        const i32 half = static_cast<i32>(std::ceil(radius / kSampleSpacingMetres));
        outCellsPerAxis = 2 * half + 1;

        const glm::vec2 safeCentre{ std::isfinite(centreXZ.x) ? centreXZ.x : 0.0f,
                                    std::isfinite(centreXZ.y) ? centreXZ.y : 0.0f };
        // floor, not a truncating cast: world XZ goes negative, and truncation
        // folds the [-spacing, 0) row onto cell 0 — the same defect
        // WaterDisturbance::LatticeForWorld documents.
        return { static_cast<i32>(std::floor(safeCentre.x / kSampleSpacingMetres)) - half,
                 static_cast<i32>(std::floor(safeCentre.y / kSampleSpacingMetres)) - half };
    }

    /// World XZ this cell is sampled at: the cell centre, jittered inside the
    /// cell by a hash of the cell itself. The jitter is FIXED per cell (no time
    /// term) so a crest's spray does not wander around inside its cell.
    [[nodiscard("the sample position is the only effect")]]
    inline glm::vec2 SamplePointForCell(glm::ivec2 cell) noexcept
    {
        const f32 jx = WaterRain::UnitFromHash(WaterRain::HashCell(cell, 0, 11u));
        const f32 jz = WaterRain::UnitFromHash(WaterRain::HashCell(cell, 0, 12u));
        return { (static_cast<f32>(cell.x) + 0.15f + 0.7f * jx) * kSampleSpacingMetres,
                 (static_cast<f32>(cell.y) + 0.15f + 0.7f * jz) * kSampleSpacingMetres };
    }

    /// Step between visited cells when the candidate count exceeds
    /// kMaxSampleCells, as a flat index into the cellsPerAxis^2 square.
    ///
    /// COPRIME WITH `cellsPerAxis`, and that is the whole reason this is a
    /// named function rather than a division. The walk advances a flat index,
    /// so x moves by `stride mod cellsPerAxis` each step; share a factor and it
    /// visits `cellsPerAxis / gcd` columns and NEVER the rest. At the default
    /// 38 m radius the naive division gives exactly that — 39 columns, stride
    /// 3, gcd 3 — and the sea sprays in world-anchored stripes 6 m apart with
    /// 4 m dead gaps, which is the directional artefact striding exists to
    /// avoid. Nudging up to the next coprime value costs a handful of samples.
    [[nodiscard("the stride is the only effect")]]
    inline i32 SampleStride(i32 totalCells, i32 cellsPerAxis) noexcept
    {
        if (totalCells <= static_cast<i32>(kMaxSampleCells) || cellsPerAxis <= 1)
            return 1;
        i32 stride = totalCells / static_cast<i32>(kMaxSampleCells) + 1;
        while (stride < totalCells && std::gcd(stride, cellsPerAxis) != 1)
            ++stride;
        return stride;
    }

    /// Particles this cell emits this frame.
    ///
    /// `deposit` is WaterFoam::DepositFromFold's output — the shared criterion,
    /// not a second one. Zero deposit is zero particles with no hash evaluated,
    /// which is the "does not emit on a calm sea" acceptance criterion: a sea
    /// that is not folding has deposit exactly 0 everywhere.
    [[nodiscard("the emission count is the only effect")]]
    inline u32 EmissionCountForCell(glm::ivec2 cell, f32 deposit, f32 ratePerCell,
                                    f32 deltaSeconds, f32 timeSeconds) noexcept
    {
        if (!(deposit > 0.0f) || !std::isfinite(ratePerCell) || !(ratePerCell > 0.0f))
            return 0u;
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f || !std::isfinite(timeSeconds))
            return 0u;

        const f32 expected = ratePerCell * deposit * deltaSeconds;
        const u32 whole = static_cast<u32>(expected);
        const f32 fraction = expected - static_cast<f32>(whole);

        // Resolved against a hash of (cell, time bucket) rather than an RNG:
        // unbiased like a coin flip, but reproducible, so a golden capture of
        // spray is possible at all. Bucketing the time keeps a marginal cell
        // from re-flipping every frame and strobing.
        const i32 bucket = static_cast<i32>(std::floor(timeSeconds / kTimeBucketSeconds));
        const f32 roll = WaterRain::UnitFromHash(WaterRain::HashCell(cell, bucket, 13u));
        return whole + ((roll < fraction) ? 1u : 0u);
    }

    /// Build this frame's spray particles.
    ///
    /// `sampleCrest` answers "what is the surface doing at this ABSOLUTE world
    /// XZ" — it is the seam that keeps this function testable with no ocean,
    /// no GPU and no renderer, which is what lets the "calm sea emits nothing"
    /// criterion be a CI test rather than a screenshot.
    ///
    /// Returns at most kMaxEmitPerFrame particles and visits at most
    /// kMaxSampleCells cells.
    template<typename SampleCrest>
    [[nodiscard("the emitted particles are the only effect")]]
    std::vector<GPUParticle> Emit(const WaterSpraySettings& settings, f32 foamThreshold,
                                  glm::vec2 cameraXZ, f32 timeSeconds, f32 deltaSeconds,
                                  SampleCrest&& sampleCrest)
    {
        std::vector<GPUParticle> particles;
        if (!settings.m_Enabled)
            return particles;
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f || !std::isfinite(timeSeconds))
            return particles;

        const f32 threshold = EffectiveThreshold(settings.m_Threshold, foamThreshold);

        i32 cellsPerAxis = 0;
        const glm::ivec2 cellMin = CellMinForCentre(cameraXZ, settings.m_RadiusMetres, cellsPerAxis);

        // The cap is applied by STRIDING rather than by stopping early: cutting
        // the walk off partway would spray only the near-in-X half of the
        // sampled disc, which is a directional artefact that follows the camera
        // yaw. Striding thins the whole disc evenly instead — see SampleStride
        // for the coprimality this depends on.
        const i32 totalCells = cellsPerAxis * cellsPerAxis;
        const i32 stride = SampleStride(totalCells, cellsPerAxis);

        const f32 launchSpeed = std::isfinite(settings.m_LaunchSpeed)
                                    ? glm::clamp(settings.m_LaunchSpeed, 0.0f, 30.0f)
                                    : 2.6f;
        const f32 lifetime =
            std::isfinite(settings.m_Lifetime) ? glm::clamp(settings.m_Lifetime, 0.05f, 20.0f) : 0.9f;
        const f32 size = std::isfinite(settings.m_ParticleSize)
                             ? glm::clamp(settings.m_ParticleSize, 0.005f, 2.0f)
                             : 0.25f;
        const f32 heightScale =
            std::isfinite(settings.m_HeightScale) ? glm::clamp(settings.m_HeightScale, 0.0f, 10.0f) : 1.0f;
        const f32 planeY = std::isfinite(settings.m_WaterPlaneY) ? settings.m_WaterPlaneY : 0.0f;
        const glm::vec2 wind{ std::isfinite(settings.m_WindMetresPerSecond.x)
                                  ? settings.m_WindMetresPerSecond.x
                                  : 0.0f,
                              std::isfinite(settings.m_WindMetresPerSecond.y)
                                  ? settings.m_WindMetresPerSecond.y
                                  : 0.0f };
        const i32 bucket = static_cast<i32>(std::floor(timeSeconds / kTimeBucketSeconds));

        for (i32 index = 0; index < totalCells; index += stride)
        {
            const glm::ivec2 cell = cellMin + glm::ivec2(index % cellsPerAxis, index / cellsPerAxis);
            const glm::vec2 worldXZ = SamplePointForCell(cell);

            const CrestSample crest = sampleCrest(worldXZ);
            if (!std::isfinite(crest.m_Fold) || !std::isfinite(crest.m_Height) ||
                !std::isfinite(crest.m_Horizontal.x) || !std::isfinite(crest.m_Horizontal.y))
                continue;

            // THE shared criterion — WaterFoam's, not a second one.
            const f32 deposit = WaterFoam::DepositFromFold(crest.m_Fold, threshold);
            u32 count = EmissionCountForCell(cell, deposit, settings.m_RatePerCell,
                                             deltaSeconds, timeSeconds);
            if (count == 0u)
                continue;

            count = glm::min(count, kMaxEmitPerFrame - static_cast<u32>(particles.size()));
            for (u32 i = 0; i < count; ++i)
            {
                // Three CONSECUTIVE streams per particle, not three fixed
                // offsets: `stream + 64` for particle 0 is `stream` for
                // particle 44, so an offset scheme correlates one droplet's
                // jitter with another's position. Interleaving cannot.
                const u32 stream = 20u + i * 3u;
                const f32 r0 = WaterRain::UnitFromHash(WaterRain::HashCell(cell, bucket, stream));
                const f32 r1 = WaterRain::UnitFromHash(WaterRain::HashCell(cell, bucket, stream + 1u));
                const f32 r2 = WaterRain::UnitFromHash(WaterRain::HashCell(cell, bucket, stream + 2u));

                // The DISPLACED surface point, not the parameter position —
                // see CrestSample. The height is scaled by the shader's own
                // height scale for the reason m_HeightScale records, and the
                // clearance is half a billboard so the puff sits ON the water
                // rather than half inside it.
                const glm::vec2 surfaceXZ = worldXZ + crest.m_Horizontal;
                const glm::vec3 position{ surfaceXZ.x + (r0 - 0.5f) * kSampleSpacingMetres * 0.25f,
                                          planeY + crest.m_Height * heightScale +
                                              kSpawnClearanceMetres + 0.5f * size,
                                          surfaceXZ.y + (r1 - 0.5f) * kSampleSpacingMetres * 0.25f };

                // Ballistic launch: mostly up, thrown downwind, with the
                // stronger folds throwing harder. GPUSimParams supplies the
                // gravity and the drag that bring it back down, so nothing here
                // integrates anything.
                const f32 speed = launchSpeed * (0.55f + 0.45f * deposit) * (0.7f + 0.6f * r2);
                const glm::vec3 velocity{ wind.x * 0.35f + (r0 - 0.5f) * 0.8f, speed,
                                          wind.y * 0.35f + (r1 - 0.5f) * 0.8f };

                GPUParticle p{};
                const f32 life = lifetime * (0.7f + 0.6f * r2);
                // Slightly translucent white — spray is water, not snow, and a
                // fully opaque droplet reads as a speck of dirt on the lens.
                const glm::vec4 color{ 1.0f, 1.0f, 1.0f, 0.55f + 0.35f * deposit };
                p.PositionLifetime = glm::vec4(position, life);
                p.VelocityMaxLifetime = glm::vec4(velocity, life);
                p.Color = color;
                p.InitialColor = color;
                p.InitialVelocitySize = glm::vec4(velocity, size * (0.6f + 0.8f * r2));
                p.Misc = glm::vec4(size * (0.6f + 0.8f * r2), 0.0f, 1.0f, -1.0f);
                particles.push_back(p);
            }

            if (particles.size() >= kMaxEmitPerFrame)
                break;
        }

        return particles;
    }
} // namespace OloEngine::WaterSpray
