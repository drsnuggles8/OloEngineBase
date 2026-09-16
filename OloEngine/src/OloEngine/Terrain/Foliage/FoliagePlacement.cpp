#include "OloEnginePCH.h"
#include "FoliagePlacement.h"

#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <glm/gtc/constants.hpp>

namespace OloEngine::FoliagePlacement
{
    namespace
    {
        // A value in [0, 1] describing how deep inside [lo, hi] `v` sits, with
        // `feather` units of linear ramp at each bound. Outside the band it is
        // 0; with a zero feather it is a hard 1 inside, which is what makes a
        // default-constructed layer behave exactly as it did before #1254.
        //
        // `domainLo` / `domainHi` are the range the QUANTITY itself can take —
        // [0, 1] for the moisture proxy, unbounded for a world-space altitude.
        // A band bound sitting at a domain edge gets NO ramp, because there is
        // nothing on the far side of it to transition to. Without that, a
        // species authored as "everywhere wet", MaxMoisture = 1, was feathered
        // DOWN on the wettest ground — thinned to 0.18 exactly where it was
        // supposed to be densest, which is the opposite of what the author
        // asked for and is invisible in anything but a species-by-species count.
        [[nodiscard]] f32 FeatheredBand(f32 v, f32 lo, f32 hi, f32 feather,
                                        f32 domainLo = -std::numeric_limits<f32>::infinity(),
                                        f32 domainHi = std::numeric_limits<f32>::infinity())
        {
            if (!(v >= lo) || !(v <= hi)) // false for NaN too, which must not place a plant
                return 0.0f;
            if (feather <= 0.0f)
                return 1.0f;

            const bool rampLo = lo > domainLo;
            const bool rampHi = hi < domainHi;
            if (!rampLo && !rampHi)
                return 1.0f;

            // Never let the two ramps overlap: a feather wider than half the
            // band would otherwise drive the centre of the band below 1. With
            // only one live ramp the whole band is available to it.
            const f32 span = (rampLo && rampHi) ? (hi - lo) * 0.5f : (hi - lo);
            const f32 width = std::min(feather, span);
            if (width <= 0.0f)
                return 1.0f;

            const f32 rising = rampLo ? std::clamp((v - lo) / width, 0.0f, 1.0f) : 1.0f;
            const f32 falling = rampHi ? std::clamp((hi - v) / width, 0.0f, 1.0f) : 1.0f;
            return std::min(rising, falling);
        }

        [[nodiscard]] f32 FiniteOr(f32 v, f32 fallback)
        {
            return std::isfinite(v) ? v : fallback;
        }

        // One lattice corner of the clump noise.
        [[nodiscard]] f32 LatticeValue(i32 x, i32 z, u32 seed)
        {
            return HashCellUnit(static_cast<u32>(x), static_cast<u32>(z), seed);
        }

        // Smooth (C1) value noise in [0, 1] on a unit lattice.
        [[nodiscard]] f32 ValueNoise(f32 gx, f32 gz, u32 seed)
        {
            const f32 fx = std::floor(gx);
            const f32 fz = std::floor(gz);
            const auto x0 = static_cast<i32>(fx);
            const auto z0 = static_cast<i32>(fz);

            f32 tx = gx - fx;
            f32 tz = gz - fz;
            tx = tx * tx * (3.0f - 2.0f * tx);
            tz = tz * tz * (3.0f - 2.0f * tz);

            const f32 v00 = LatticeValue(x0, z0, seed);
            const f32 v10 = LatticeValue(x0 + 1, z0, seed);
            const f32 v01 = LatticeValue(x0, z0 + 1, seed);
            const f32 v11 = LatticeValue(x0 + 1, z0 + 1, seed);

            const f32 a = v00 + tx * (v10 - v00);
            const f32 b = v01 + tx * (v11 - v01);
            return a + tz * (b - a);
        }

        // Everything GenerateLayer needs from the layer's #1254 rules, already
        // validated. Sanitised once per layer rather than once per grid cell,
        // and here rather than trusted, because the editor inspector writes
        // straight into the component and so bypasses both deserializers —
        // the same reason FoliageRenderer re-validates the leaf material.
        struct SanitizedRules
        {
            bool m_UseAltitude = false;
            f32 m_MinAltitude = 0.0f;
            f32 m_MaxAltitude = 0.0f;
            f32 m_AltitudeFeather = 0.0f;

            bool m_UseMoisture = false;
            f32 m_MinMoisture = 0.0f;
            f32 m_MaxMoisture = 1.0f;
            f32 m_MoistureFeather = 0.0f;

            f32 m_SlopeFeather = 0.0f;

            f32 m_ClumpStrength = 0.0f;
            f32 m_ClumpScale = 12.0f;
            f32 m_ClumpFalloff = 1.0f;
            f32 m_ClumpScaleInfluence = 0.0f;
            u32 m_ClumpSeed = 0;

            f32 m_GroundOffset = 0.0f;
            f32 m_SlopeSinkFactor = 0.0f;

            f32 m_ExclusionThreshold = 0.5f;
        };

        [[nodiscard]] SanitizedRules Sanitize(const FoliageLayer& layer, u32 layerSeed)
        {
            SanitizedRules r;

            r.m_MinAltitude = FiniteOr(layer.MinAltitude, 0.0f);
            r.m_MaxAltitude = FiniteOr(layer.MaxAltitude, 1000.0f);
            // An inverted band would reject everything silently. Ordering it is
            // the only reading of "min 60, max 20" that places anything, and a
            // band that places nothing is indistinguishable from a broken layer.
            if (r.m_MaxAltitude < r.m_MinAltitude)
                std::swap(r.m_MinAltitude, r.m_MaxAltitude);
            r.m_AltitudeFeather = std::max(FiniteOr(layer.AltitudeFeather, 0.0f), 0.0f);
            r.m_UseAltitude = layer.UseAltitudeBand;

            r.m_MinMoisture = std::clamp(FiniteOr(layer.MinMoisture, 0.0f), 0.0f, 1.0f);
            r.m_MaxMoisture = std::clamp(FiniteOr(layer.MaxMoisture, 1.0f), 0.0f, 1.0f);
            if (r.m_MaxMoisture < r.m_MinMoisture)
                std::swap(r.m_MinMoisture, r.m_MaxMoisture);
            r.m_MoistureFeather = std::clamp(FiniteOr(layer.MoistureFeather, 0.0f), 0.0f, 1.0f);
            r.m_UseMoisture = layer.UseMoisture;

            r.m_SlopeFeather = std::clamp(FiniteOr(layer.SlopeFeather, 0.0f), 0.0f, 90.0f);

            r.m_ClumpStrength = std::clamp(FiniteOr(layer.ClumpStrength, 0.0f), 0.0f, 1.0f);
            // A zero or negative patch size divides by zero in ClumpField.
            r.m_ClumpScale = std::max(FiniteOr(layer.ClumpScale, 12.0f), 0.01f);
            r.m_ClumpFalloff = std::clamp(FiniteOr(layer.ClumpFalloff, 1.0f), 0.05f, 16.0f);
            r.m_ClumpScaleInfluence = std::clamp(FiniteOr(layer.ClumpScaleInfluence, 0.0f), 0.0f, 1.0f);
            r.m_ClumpSeed = SeedForClumpGroup(layer.ClumpGroup, layerSeed);

            r.m_GroundOffset = FiniteOr(layer.GroundOffset, 0.0f);
            r.m_SlopeSinkFactor = std::clamp(FiniteOr(layer.SlopeSinkFactor, 0.0f), 0.0f, 4.0f);

            r.m_ExclusionThreshold = std::clamp(FiniteOr(layer.ExclusionThreshold, 0.5f), 0.0f, 1.0f);

            return r;
        }

        // The slope gate's four cosines, precomputed once per layer. Cosine
        // space rather than angle space because the hard accept/reject has to
        // stay the EXACT comparison GenerateLayer has always made — a layer
        // with a zero feather must not move a single plant, and
        // degrees(acos(upDot)) does not round-trip back to these bounds
        // bit-for-bit.
        struct SlopeGate
        {
            f32 m_CosLo = 0.0f;      // minimum acceptable upDot — from MaxSlopeAngle
            f32 m_CosHi = 1.0f;      // maximum acceptable upDot — from MinSlopeAngle
            f32 m_CosInnerLo = 0.0f; // where the feathered ramp from m_CosLo reaches 1
            f32 m_CosInnerHi = 1.0f; // where the feathered ramp towards m_CosHi leaves 1
        };

        [[nodiscard]] SlopeGate MakeSlopeGate(const FoliageLayer& layer, f32 slopeFeather)
        {
            // NOT reordered when MinSlopeAngle > MaxSlopeAngle. That band
            // rejects every cell today, and quietly swapping it would make
            // plants appear in a scene whose author never asked for them —
            // exactly the silent fallback the house rules forbid. A broken band
            // stays visibly broken.
            const f32 minAngle = FiniteOr(layer.MinSlopeAngle, 0.0f);
            const f32 maxAngle = FiniteOr(layer.MaxSlopeAngle, 45.0f);

            SlopeGate gate;
            gate.m_CosLo = std::cos(glm::radians(maxAngle));
            gate.m_CosHi = std::cos(glm::radians(minAngle));
            gate.m_CosInnerLo = gate.m_CosLo;
            gate.m_CosInnerHi = gate.m_CosHi;

            if (slopeFeather > 0.0f)
            {
                // Clamped to half the band, exactly as FeatheredBand clamps its
                // own: two ramps wider than half the band overlap, and the
                // CENTRE of the band then never reaches suitability 1, so a
                // uniformly-suitable slope loses a random quarter of its plants.
                // This used to be worked around at the one call site that
                // authors a feather (TerrainGenerator), which is precisely the
                // sign that the guard belonged here instead.
                const f32 halfBand = std::max((maxAngle - minAngle) * 0.5f, 0.0f);
                const f32 width = std::min(slopeFeather, halfBand);
                gate.m_CosInnerLo = std::cos(glm::radians(maxAngle - width));
                gate.m_CosInnerHi = std::cos(glm::radians(minAngle + width));
            }
            return gate;
        }

        // The product of the slope, altitude and moisture gates. Takes the
        // already-sanitised rules and the already-built gate so the inner
        // generator loop pays for neither per cell.
        [[nodiscard]] f32 HabitatWeight(const SanitizedRules& rules, const SlopeGate& gate,
                                        f32 altitude, f32 normalizedHeight, f32 upDot)
        {
            // Negated comparisons so a NaN upDot rejects rather than places.
            if (!(upDot >= gate.m_CosLo) || !(upDot <= gate.m_CosHi))
                return 0.0f;

            f32 weight = 1.0f;
            if (rules.m_SlopeFeather > 0.0f)
            {
                const f32 rising = gate.m_CosInnerLo > gate.m_CosLo
                                       ? std::clamp((upDot - gate.m_CosLo) /
                                                        (gate.m_CosInnerLo - gate.m_CosLo),
                                                    0.0f, 1.0f)
                                       : 1.0f;
                const f32 falling = gate.m_CosHi > gate.m_CosInnerHi
                                        ? std::clamp((gate.m_CosHi - upDot) /
                                                         (gate.m_CosHi - gate.m_CosInnerHi),
                                                     0.0f, 1.0f)
                                        : 1.0f;
                weight = std::min(rising, falling);
            }

            if (rules.m_UseAltitude)
            {
                weight *= FeatheredBand(altitude, rules.m_MinAltitude, rules.m_MaxAltitude,
                                        rules.m_AltitudeFeather);
            }

            if (rules.m_UseMoisture)
            {
                // Domain [0, 1]: the moisture proxy cannot leave it, so a band
                // that reaches either end has no outside to feather towards.
                weight *= FeatheredBand(MoistureAt(normalizedHeight, upDot), rules.m_MinMoisture,
                                        rules.m_MaxMoisture, rules.m_MoistureFeather, 0.0f, 1.0f);
            }

            return std::clamp(weight, 0.0f, 1.0f);
        }

        // The CPU splatmap plane a channel index reads from, or nullptr.
        struct SplatSource
        {
            const u8* m_Data = nullptr;
            u32 m_Resolution = 0;
            i32 m_ChannelInPlane = 0;
        };

        [[nodiscard]] SplatSource ResolveSplat(const TerrainMaterial* material, i32 channel)
        {
            if (!material || channel < 0 || channel >= 8 || !material->HasCPUSplatmaps())
                return {};

            const auto& plane = material->GetSplatmapData(static_cast<u32>(channel / 4));
            if (plane.empty())
                return {};

            return SplatSource{ plane.data(), material->GetSplatmapResolution(), channel % 4 };
        }

        [[nodiscard]] f32 SampleSplat(const SplatSource& source, f32 nx, f32 nz)
        {
            const u32 sx = std::min(static_cast<u32>(nx * static_cast<f32>(source.m_Resolution)),
                                    source.m_Resolution - 1);
            const u32 sz = std::min(static_cast<u32>(nz * static_cast<f32>(source.m_Resolution)),
                                    source.m_Resolution - 1);
            // Splatmap is RGBA packed, so index = (sz * res + sx) * 4 + channel
            const sizet index = (static_cast<sizet>(sz) * source.m_Resolution + sx) * 4 +
                                static_cast<sizet>(source.m_ChannelInPlane);
            return static_cast<f32>(source.m_Data[index]) / 255.0f;
        }
    } // namespace

    f32 HashPosition(f32 x, f32 z, u32 seed)
    {
        u32 hx = 0;
        u32 hz = 0;
        f32 fx = x * 73856093.0f;
        f32 fz = z * 19349663.0f;
        std::memcpy(&hx, &fx, sizeof(u32));
        std::memcpy(&hz, &fz, sizeof(u32));
        u32 h = hx ^ hz ^ seed;
        h = (h * 2654435761u) >> 16;
        return static_cast<f32>(h & 0xFFFF) / 65536.0f;
    }

    u32 HashCell(u32 cellX, u32 cellZ, u32 seed)
    {
        // Fold all three inputs in first, THEN avalanche — the ordering
        // HashPosition gets wrong, and the whole reason this exists.
        u32 h = cellX * 0x9E3779B1u;
        h ^= cellZ * 0x85EBCA77u;
        h ^= seed * 0xC2B2AE3Du;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        h *= 0x297A2D39u;
        h ^= h >> 15;
        return h;
    }

    f32 HashCellUnit(u32 cellX, u32 cellZ, u32 seed)
    {
        // Top 24 bits: the low bits of a multiply-xorshift finalizer are the
        // least mixed, and 24 is exactly what an f32 mantissa can hold.
        return static_cast<f32>(HashCell(cellX, cellZ, seed) >> 8) / 16777216.0f;
    }

    f32 SpacingForDensity(f32 density)
    {
        return 1.0f / std::sqrt(density);
    }

    f32 ClumpField(f32 worldX, f32 worldZ, f32 scale, u32 seed)
    {
        const f32 safeScale = std::max(std::isfinite(scale) ? scale : 12.0f, 0.01f);
        const f32 gx = worldX / safeScale;
        const f32 gz = worldZ / safeScale;

        // 2.3 rather than 2: an integer ratio makes the two lattices share
        // every other corner, which puts the grid back into the field.
        constexpr f32 kOctaveRatio = 2.3f;
        constexpr f32 kBaseWeight = 2.0f / 3.0f;
        constexpr f32 kDetailWeight = 1.0f / 3.0f;

        const f32 base = ValueNoise(gx, gz, seed);
        const f32 detail = ValueNoise(gx * kOctaveRatio, gz * kOctaveRatio, seed ^ 0x5BD1E995u);
        return std::clamp(base * kBaseWeight + detail * kDetailWeight, 0.0f, 1.0f);
    }

    f32 MoistureAt(f32 normalizedHeight, f32 upDot)
    {
        const f32 lowness = 1.0f - std::clamp(std::isfinite(normalizedHeight) ? normalizedHeight : 0.0f,
                                              0.0f, 1.0f);

        // Flatness measured from 45 degrees, where water stops pooling and
        // starts running off. Below that the terrain contributes no moisture.
        constexpr f32 kRunoffCos = 0.70710678f; // cos(45 degrees)
        const f32 up = std::clamp(std::isfinite(upDot) ? upDot : 1.0f, 0.0f, 1.0f);
        const f32 flatness = std::clamp((up - kRunoffCos) / (1.0f - kRunoffCos), 0.0f, 1.0f);

        return std::clamp(0.5f * lowness + 0.5f * flatness, 0.0f, 1.0f);
    }

    f32 GroundSinkFor(f32 slopeSinkFactor, f32 upDot, f32 scale)
    {
        if (!(slopeSinkFactor > 0.0f) || !std::isfinite(scale))
            return 0.0f;

        // Floored well above 0 so a near-vertical face cannot produce a plant
        // sunk kilometres into the hill; the slope gate rejects those anyway.
        constexpr f32 kMinUpDot = 0.17364818f; // cos(80 degrees)
        const f32 up = std::clamp(std::isfinite(upDot) ? upDot : 1.0f, kMinUpDot, 1.0f);
        const f32 tanSlope = std::sqrt(std::max(0.0f, 1.0f - up * up)) / up;

        const f32 halfWidth = 0.5f * std::max(scale, 0.0f);
        return -slopeSinkFactor * tanSlope * halfWidth;
    }

    f32 ExclusionWeight(f32 paintedWeight, f32 threshold)
    {
        const f32 painted = std::clamp(std::isfinite(paintedWeight) ? paintedWeight : 0.0f, 0.0f, 1.0f);
        if (!(threshold > 0.0f))
        {
            // The threshold is "the painted weight at which suppression is
            // TOTAL", so zero means any paint at all suppresses. Returning 1
            // here — the obvious way to dodge the division — inverts the
            // control: an author dragging the slider to 0 would silently get no
            // exclusion at the setting that should exclude hardest.
            return painted > 0.0f ? 0.0f : 1.0f;
        }
        return 1.0f - std::clamp(painted / threshold, 0.0f, 1.0f);
    }

    f32 HabitatSuitability(const FoliageLayer& layer, f32 altitude, f32 normalizedHeight, f32 upDot)
    {
        const SanitizedRules rules = Sanitize(layer, SeedForLayer(0));
        return HabitatWeight(rules, MakeSlopeGate(layer, rules.m_SlopeFeather), altitude,
                             normalizedHeight, upDot);
    }

    void GenerateLayer(const FoliageLayer& layer, u32 layerIndex,
                       const std::vector<f32>& heights, u32 resolution,
                       const TerrainMaterial* material,
                       f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                       std::vector<Placement>& out)
    {
        OLO_PROFILE_FUNCTION();

        out.clear();

        if (!layer.Enabled || layer.Density <= 0.0f || resolution == 0)
        {
            return;
        }

        const f32 spacing = SpacingForDensity(layer.Density);
        const auto countX = static_cast<u32>(std::ceil(worldSizeX / spacing));
        const auto countZ = static_cast<u32>(std::ceil(worldSizeZ / spacing));

        const SplatSource density = ResolveSplat(material, layer.SplatmapChannel);
        const SplatSource exclusion = ResolveSplat(material, layer.ExclusionSplatmapChannel);

        out.reserve(static_cast<sizet>(countX) * countZ / 4); // Estimate ~25% coverage

        const u32 seed = SeedForLayer(layerIndex);
        const SanitizedRules rules = Sanitize(layer, seed);
        const SlopeGate slopeGate = MakeSlopeGate(layer, rules.m_SlopeFeather);

        // Which hash the per-cell draws come from (issue #1254). Off by
        // default, because switching it MOVES every plant in the layer — the
        // registry's placement signature covers the flag for exactly that
        // reason.
        const bool decorrelated = layer.DecorrelatedVariation;
        const auto draw = [decorrelated](u32 ix, u32 iz, u32 drawSeed) -> f32
        {
            return decorrelated ? HashCellUnit(ix, iz, drawSeed)
                                : HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), drawSeed);
        };

        for (u32 iz = 0; iz < countZ; ++iz)
        {
            for (u32 ix = 0; ix < countX; ++ix)
            {
                // Jittered position
                const f32 jx = draw(ix, iz, seed);
                const f32 jz = draw(ix, iz, seed + 7);

                const f32 worldX = (static_cast<f32>(ix) + jx) * spacing;
                const f32 worldZ = (static_cast<f32>(iz) + jz) * spacing;

                if (worldX >= worldSizeX || worldZ >= worldSizeZ)
                    continue;

                const f32 nx = worldX / worldSizeX;
                const f32 nz = worldZ / worldSizeZ;

                // Slope check — dot(normal, up)
                const glm::vec3 normal = TerrainData::SampleNormal(heights, resolution, nx, nz,
                                                                   worldSizeX, worldSizeZ, heightScale);
                const f32 upDot = normal.y;
                if (upDot < slopeGate.m_CosLo || upDot > slopeGate.m_CosHi)
                    continue;

                const f32 normalizedHeight = TerrainData::SampleHeight(heights, resolution, nx, nz);
                const f32 height = normalizedHeight * heightScale;

                // ── Suitability (issue #1254) ────────────────────────────────
                //
                // Everything that says "how much does this species want to be
                // here" multiplies into ONE number, and one stochastic
                // comparison decides. That is what makes a boundary feather
                // instead of ending on a line, and it is the same comparison
                // the splatmap mask has always used.
                //
                // A suitability of exactly 1 skips the comparison entirely,
                // which is what keeps a pre-#1254 layer bit-identical: with no
                // splatmap, no habitat rules and no clumping, every gate below
                // is 1 and no cell is ever rejected.
                f32 suitability = HabitatWeight(rules, slopeGate, height, normalizedHeight, upDot);
                if (suitability <= 0.0f)
                    continue;

                if (density.m_Data && density.m_Resolution > 0)
                    suitability *= SampleSplat(density, nx, nz);

                if (exclusion.m_Data && exclusion.m_Resolution > 0)
                    suitability *= ExclusionWeight(SampleSplat(exclusion, nx, nz), rules.m_ExclusionThreshold);

                // The clump field is evaluated when EITHER consumer wants it.
                // A layer with a scale influence and no density clumping is a
                // legitimate authoring choice — size variation in patches,
                // uniform coverage — and leaving `clump` at its 1 placeholder
                // would have silently grown every plant in it by 50%.
                f32 clump = 1.0f;
                if (rules.m_ClumpStrength > 0.0f || rules.m_ClumpScaleInfluence > 0.0f)
                {
                    clump = std::pow(ClumpField(worldX, worldZ, rules.m_ClumpScale, rules.m_ClumpSeed),
                                     rules.m_ClumpFalloff);
                    if (rules.m_ClumpStrength > 0.0f)
                        suitability *= glm::mix(1.0f, clump, rules.m_ClumpStrength);
                }

                if (suitability < 1.0f)
                {
                    if (suitability <= 0.0f)
                        continue;
                    const f32 threshold = decorrelated
                                              ? HashCellUnit(ix, iz, seed + 13)
                                              : HashPosition(static_cast<f32>(ix) + 0.5f,
                                                             static_cast<f32>(iz) + 0.5f, seed + 13);
                    if (threshold > suitability)
                        continue;
                }

                // Randomize scale and height
                const f32 scaleRand = draw(ix, iz, seed + 3);
                const f32 heightRand = draw(ix, iz, seed + 5);
                f32 scale = glm::mix(layer.MinScale, layer.MaxScale, scaleRand);
                const f32 instanceHeight = glm::mix(layer.MinHeight, layer.MaxHeight, heightRand);

                // A patch's core grows bigger plants than its fringe, which is
                // what stops a clumped layer reading as the same plant stamped
                // at varying spacing.
                if (rules.m_ClumpScaleInfluence > 0.0f)
                    scale *= glm::mix(1.0f, 0.5f + clump, rules.m_ClumpScaleInfluence);

                // Random rotation
                f32 rotation = 0.0f;
                if (layer.RandomRotation)
                {
                    rotation = draw(ix, iz, seed + 11) * glm::two_pi<f32>();
                }

                // Ground contact. Sinking is derived from the FINAL scale, so a
                // clump-boosted plant sinks by its own half-width rather than
                // the layer's nominal one.
                const f32 groundY = height + rules.m_GroundOffset +
                                    GroundSinkFor(rules.m_SlopeSinkFactor, upDot, scale);

                Placement placement;
                placement.m_CellX = ix;
                placement.m_CellZ = iz;
                placement.m_Row.PositionScale = glm::vec4(worldX, groundY, worldZ, scale);
                placement.m_Row.RotationHeight = glm::vec4(rotation, instanceHeight, 1.0f, 0.0f); // fade=1 (full)
                placement.m_Row.ColorAlpha = glm::vec4(layer.BaseColor, layer.AlphaCutoff);
                out.push_back(placement);
            }
        }
    }
} // namespace OloEngine::FoliagePlacement
