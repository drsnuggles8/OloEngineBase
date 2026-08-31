#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Ocean/OceanCascades.h"

#include <cmath>
#include <limits>

namespace OloEngine::Ocean
{
    namespace
    {
        constexpr f32 kTwoPi = 6.28318530717958647692f;
    } // namespace

    u32 RoundUpCascadeResolution(f32 v, u32 maxResolution) noexcept
    {
        const u32 ceiling = std::max(maxResolution, kMinCascadeResolution);
        if (!std::isfinite(v) || v <= static_cast<f32>(kMinCascadeResolution))
            return std::min(kMinCascadeResolution, ceiling);

        u32 n = kMinCascadeResolution;
        while (static_cast<f32>(n) < v && n < ceiling)
            n *= 2u;
        return std::min(n, ceiling);
    }

    CascadePreset MakeCascadePreset(u32 cascadeCount, f32 patchSize, u32 resolution)
    {
        CascadePreset preset;
        const f32 L = (std::isfinite(patchSize) && patchSize > 0.0f) ? patchSize : 1.0f;

        // Single cascade: the legacy field, verbatim. No band limit (KMax is
        // infinite, so ApplyBandLimit keeps every bin), no rotation, and the
        // authored L and N — NOT floored to kMinCascadeResolution, which would
        // quietly double the grid of a scene authored at 16. That floor exists
        // for a DERIVED band, which needs enough distinct wave vectors to look
        // like a spectrum; an authored resolution is the author's call. A scene
        // that has not opted in must not be able to tell this file exists.
        if (cascadeCount <= kSingleCascadeCount)
        {
            preset.Count = kSingleCascadeCount;
            preset.ArrayResolution = resolution;
            preset.Bands[0].PatchSize = L;
            preset.Bands[0].Resolution = resolution;
            preset.Bands[0].KMin = 0.0f;
            preset.Bands[0].KMax = std::numeric_limits<f32>::infinity();
            preset.Bands[0].DomainRotation = 0.0f;
            // Unbounded above, so the proxy keeps the cap — exactly the size the
            // pre-#969 field used, which is what leaves the fallback path's CPU
            // cost where it was.
            preset.Bands[0].PhysicsResolution = std::min(resolution, kPhysicsProxyResolution);
            return preset;
        }

        // The fixed three-band preset. The authored L is the MID tile; the
        // broad and fine tiles sit either side of it at non-commensurate ratios.
        const u32 N = std::max(resolution, kMinCascadeResolution);
        const f32 broadL = L * kBroadTileRatio;
        const f32 midL = L;
        const f32 fineL = L * kFineTileRatio;

        // Band boundaries: the lowest |k| the NEXT (finer) tile can represent,
        // i.e. its own fundamental. Half-open ranges sharing an endpoint ⇒ no
        // gap and no double-counted energy at the handoff.
        const f32 kBroadMid = kTwoPi / midL;
        const f32 kMidFine = kTwoPi / fineL;

        // Highest signed bin index each bounded band actually populates:
        // n_max = kMax * L / (2*pi), which for these boundaries is just the tile
        // ratio. Resolution follows from kMinSamplesPerWavelength samples across
        // the band's shortest wavelength (= 2 * n_max samples across the tile,
        // times the oversample factor / 2 — i.e. kMinSamplesPerWavelength *
        // n_max grid points).
        const f32 broadNMax = kBroadMid * broadL / kTwoPi;
        const f32 midNMax = kMidFine * midL / kTwoPi;

        preset.Count = kThreeBandCascadeCount;

        preset.Bands[0].PatchSize = broadL;
        preset.Bands[0].Resolution = RoundUpCascadeResolution(kMinSamplesPerWavelength * broadNMax, N);
        preset.Bands[0].KMin = 0.0f;
        preset.Bands[0].KMax = kBroadMid;
        preset.Bands[0].DomainRotation = 0.0f;
        preset.Bands[0].PhysicsResolution =
            std::min(RoundUpCascadeResolution(kProxySamplesPerWavelength * broadNMax, N), kPhysicsProxyResolution);

        preset.Bands[1].PatchSize = midL;
        preset.Bands[1].Resolution = RoundUpCascadeResolution(kMinSamplesPerWavelength * midNMax, N);
        preset.Bands[1].KMin = kBroadMid;
        preset.Bands[1].KMax = kMidFine;
        preset.Bands[1].DomainRotation = kMidCascadeRotationRadians;
        preset.Bands[1].PhysicsResolution =
            std::min(RoundUpCascadeResolution(kProxySamplesPerWavelength * midNMax, N), kPhysicsProxyResolution);

        // The top band has no upper boundary of its own — its grid Nyquist is
        // the limit, exactly as in the single-cascade field — so it is the one
        // band that keeps the authored resolution.
        preset.Bands[2].PatchSize = fineL;
        preset.Bands[2].Resolution = N;
        preset.Bands[2].KMin = kMidFine;
        preset.Bands[2].KMax = std::numeric_limits<f32>::infinity();
        preset.Bands[2].DomainRotation = 0.0f;
        // The only band with no upper boundary, so the only one that needs the
        // full proxy grid.
        preset.Bands[2].PhysicsResolution = std::min(N, kPhysicsProxyResolution);

        preset.ArrayResolution =
            std::max({ preset.Bands[0].Resolution, preset.Bands[1].Resolution, preset.Bands[2].Resolution });
        return preset;
    }

    glm::vec4 PackCascadeShaderParams(const CascadePreset& preset)
    {
        if (preset.Count <= kSingleCascadeCount)
            return glm::vec4(0.0f, 0.0f, 1.0f, 0.0f); // no extra cascades, identity rotation

        const f32 invMid = (preset.Bands[1].PatchSize > 0.0f) ? (1.0f / preset.Bands[1].PatchSize) : 0.0f;
        const f32 invFine = (preset.Bands[2].PatchSize > 0.0f) ? (1.0f / preset.Bands[2].PatchSize) : 0.0f;
        return glm::vec4(invMid, invFine, std::cos(preset.Bands[1].DomainRotation),
                         std::sin(preset.Bands[1].DomainRotation));
    }
} // namespace OloEngine::Ocean
