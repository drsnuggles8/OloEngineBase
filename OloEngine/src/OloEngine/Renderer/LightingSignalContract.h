#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <array>
#include <string_view>
#include <type_traits>
#include <utility>

// =============================================================================
// LightingSignalContract.h — who owns each term of a lit surface (issue #1336)
//
// The renderer composes a shaded pixel from a handful of physical terms, and
// several techniques can estimate each of them: the raster light loop, ReSTIR
// DI, the ambient ladder, SSGI, ReSTIR GI / PT, the reflection tiers. Two
// estimates of ONE term must never both be added (a double count), and a term
// nobody estimates must not silently vanish (a dropped term). This header is
// the executable form of that rule: given which techniques are live this frame,
// ResolveLightingSignalOwnership says who answers for every term and how their
// answers combine. RenderPipeline enables SSGI from its answer, and applies the
// AO and contact-shadow visibilities through the two selectors below; the
// ReSTIR tiers' stand-downs are decided by their own selectors, which
// LightingSignalContractTest pins to agree with this function.
//
// The prose — what each signal stores, in which units, and why each decision
// was made — is docs/agent-rules/lighting-signal-contract.md. The GLSL side of
// the composition is oloComposeReflectedLighting / oloComposeSurfaceRadiance in
// include/PBRCommon.glsl, the functions every raster path composes through.
//
// constexpr and backend-neutral, in the shape of ReSTIRPTTechnique.h's
// SelectReSTIRPTOwnership, which this composes rather than re-derives: the
// ReSTIR selectors stay the owners of their own hand-offs.
// =============================================================================

namespace OloEngine
{
    // The terms a lit opaque surface is composed from. Media (fog, clouds,
    // volumetrics) are applied after the surface is composed and are not
    // surface terms; see the contract document's media section.
    enum class LightingTerm : u8
    {
        DirectDiffuse,
        DirectSpecular,
        IndirectDiffuse,
        IndirectSpecular,
        Emission,
        SurfaceTransmission,
        Count
    };
    inline constexpr sizet kLightingTermCount = static_cast<sizet>(LightingTerm::Count);

    // What a buffer or a function argument STORES. Every hand-off in the
    // contract document is labelled with one of these, because mixing two of
    // them is how the pi and double-count bugs in #1336 happened.
    enum class LightingSignalKind : u8
    {
        Radiance,             // L, W/(m^2 sr): arriving along a direction, or leaving one
        NormalizedIrradiance, // E / pi: what the IBL irradiance cube stores and the ambient ladder consumes
        Irradiance,           // E, W/m^2: lightmap, DDGI atlas, probe volume, PathTracer::EstimateIrradiance
        Contribution,         // f * L * cos (* weights): outgoing radiance a term adds to the pixel
        Visibility,           // [0,1] fraction of a term that survives (shadow, AO)
        Confidence,           // [0,1] share of a term an estimator is entitled to answer for
    };

    // The estimators. Bit flags, so a term's owner set is a mask.
    enum class LightingEstimator : u32
    {
        None = 0,
        RasterLightLoop = 1u << 0,       // the MultiLight UBO loop (directional lights, or all lights on plain Forward)
        ClusteredTiles = 1u << 1,        // Forward+ / tiled-deferred tile light lists
        ReSTIRDI = 1u << 2,              // resampled direct lighting for punctual / area / emissive lights
        AmbientLadder = 1u << 3,         // lightmap > probe volume > IBL > flat, at the primary hit
        SSGI = 1u << 4,                  // screen-space one-bounce diffuse
        ReSTIRGI = 1u << 5,              // resampled one-bounce diffuse, DDGI as the path tail
        ReSTIRPT = 1u << 6,              // resampled path-traced indirect (diffuse and specular)
        ReflectionProbesIBL = 1u << 7,   // prefiltered environment + distance-impostor probes (bottom tier)
        RayTracedReflection = 1u << 8,   // ray-query reflection tier
        ScreenSpaceReflection = 1u << 9, // SSR tier
        SurfaceEmission = 1u << 10,      // the material's own emission, added once
        SurfaceTransmission = 1u << 11,  // leaf / skin thin-region transmission lobes
    };

    [[nodiscard]] constexpr LightingEstimator operator|(LightingEstimator a, LightingEstimator b)
    {
        return static_cast<LightingEstimator>(std::to_underlying(a) | std::to_underlying(b));
    }
    [[nodiscard]] constexpr bool HasEstimator(LightingEstimator mask, LightingEstimator e)
    {
        return (std::to_underlying(mask) & std::to_underlying(e)) != 0u;
    }
    [[nodiscard]] constexpr u32 CountEstimators(LightingEstimator mask)
    {
        u32 bits = std::to_underlying(mask);
        u32 count = 0;
        while (bits != 0u)
        {
            count += bits & 1u;
            bits >>= 1u;
        }
        return count;
    }

    // How several owners of one term combine. There is no "Additive": two
    // estimates of one quantity are never summed.
    enum class TermComposition : u8
    {
        // One estimator answers for the whole term.
        Exclusive,
        // Several estimators each answer for a DISJOINT SET OF LIGHTS (the loop
        // takes the directional lights, ReSTIR DI or the tiles take the rest).
        PartitionedByLight,
        // Several estimates of the SAME quantity, blended by confidences whose
        // effective weights sum to one (ADR 0020's tier algebra for reflections;
        // SSGI over the ambient ladder's diffuse for indirect diffuse).
        ConfidenceMixture,
    };

    struct LightingTermOwnership
    {
        LightingEstimator Owners = LightingEstimator::None;
        TermComposition Composition = TermComposition::Exclusive;

        [[nodiscard]] auto operator==(const LightingTermOwnership&) const -> bool = default;
    };

    // Where the screen-space AO buffer (SSAO / GTAO, sphere proxies folded in)
    // is multiplied in. It is visibility for the AMBIENT term only.
    enum class ScreenSpaceAOApplication : u8
    {
        // No AO technique produced a buffer this frame.
        None,
        // Deferred: DeferredLighting multiplies the ambient split by it.
        // Exact: direct, emission, transmission and traced indirect are untouched.
        AmbientTermInLighting,
        // Forward / Forward+: the AO buffer is built FROM the forward pass's own
        // normals, after the lighting that would need it, so AOApply multiplies
        // the composed colour. A DECLARED APPROXIMATION: it also darkens direct
        // light and emission. Removing it needs the forward pass to export its
        // ambient term, which every forward writer would have to write.
        ComposedColorApproximation,
    };

    // Where screen-space contact shadows are multiplied in. They are
    // visibility for ONE light — the primary directional light, Lights[0].
    enum class ContactShadowApplication : u8
    {
        None,
        // Deferred: DeferredLighting multiplies Lights[0]'s visibility by the
        // march, beside its shadow map / RT mask / cloud shadow. The post pass
        // used to multiply the whole frame, darkening ambient, emission,
        // reflections and every other light.
        PrimaryDirectionalLightInLighting,
    };

    // How SSGI's estimate reaches the frame.
    enum class SSGIComposition : u8
    {
        None,
        // SSGI answers for the fraction of the cosine hemisphere its rays
        // resolved on screen, and the ambient ladder keeps the rest: the
        // composite adds `hits - resolvedFraction * ladderDiffuse`, never the
        // hits on top of a ladder that already counted those directions.
        ReplacesLadderDiffuseOverResolvedDirections,
    };

    struct LightingFrameConfiguration
    {
        RenderingPath Path = RenderingPath::Forward;
        // Techniques that are LIVE this frame (engaged and producing a signal),
        // not merely requested — the same distinction every tier stat makes.
        bool ReSTIRDIActive = false;
        bool ReSTIRGIActive = false;
        bool ReSTIRPTActive = false;
        bool SSGIRequested = false;
        bool SSRActive = false;
        bool RayTracedReflectionActive = false;
        bool ScreenSpaceAOProduced = false;
        bool ProbeOrIBLSpecularActive = true;
        bool ContactShadowsRequested = false;
    };

    struct LightingSignalOwnership
    {
        std::array<LightingTermOwnership, kLightingTermCount> Terms{};
        ScreenSpaceAOApplication ScreenSpaceAO = ScreenSpaceAOApplication::None;
        ContactShadowApplication ContactShadow = ContactShadowApplication::None;
        SSGIComposition SSGI = SSGIComposition::None;

        [[nodiscard]] constexpr const LightingTermOwnership& Of(LightingTerm term) const
        {
            return Terms[static_cast<sizet>(term)];
        }
        [[nodiscard]] auto operator==(const LightingSignalOwnership&) const -> bool = default;
    };

    [[nodiscard]] constexpr ScreenSpaceAOApplication SelectScreenSpaceAOApplication(bool aoProduced, RenderingPath path)
    {
        if (!aoProduced)
            return ScreenSpaceAOApplication::None;
        return path == RenderingPath::Deferred ? ScreenSpaceAOApplication::AmbientTermInLighting
                                               : ScreenSpaceAOApplication::ComposedColorApproximation;
    }

    // Contact shadows march the G-Buffer depth, so they exist on the deferred
    // path only (the forward paths never declared the pass's output either).
    [[nodiscard]] constexpr ContactShadowApplication SelectContactShadowApplication(bool requested, RenderingPath path)
    {
        return (requested && path == RenderingPath::Deferred) ? ContactShadowApplication::PrimaryDirectionalLightInLighting
                                                              : ContactShadowApplication::None;
    }

    [[nodiscard("Configure the passes from the resolved ownership")]] constexpr LightingSignalOwnership
    ResolveLightingSignalOwnership(const LightingFrameConfiguration& frame)
    {
        using enum LightingEstimator;
        LightingSignalOwnership result{};
        const bool deferred = frame.Path == RenderingPath::Deferred;
        // The hybrid tiers exist only where there is a G-Buffer to resample
        // from; a caller claiming otherwise is corrected rather than trusted.
        const bool di = deferred && frame.ReSTIRDIActive;
        const bool pt = deferred && frame.ReSTIRPTActive;
        const bool gi = deferred && frame.ReSTIRGIActive && !pt;
        const bool ssgi = deferred && frame.SSGIRequested && !gi && !pt;

        // ---- direct: partitioned by light -------------------------------
        // ReSTIR DI owns every punctual, area and emissive light; the loop
        // keeps the directional ones (their CSM / VSM / RT mask / cloud shadow
        // live there). Otherwise the tiles take the punctual lights on
        // Forward+ and Deferred, and plain Forward loops over everything.
        LightingTermOwnership direct{};
        if (di)
            direct = { RasterLightLoop | ReSTIRDI, TermComposition::PartitionedByLight };
        else if (frame.Path == RenderingPath::Forward)
            direct = { RasterLightLoop, TermComposition::Exclusive };
        else
            direct = { RasterLightLoop | ClusteredTiles, TermComposition::PartitionedByLight };
        result.Terms[static_cast<sizet>(LightingTerm::DirectDiffuse)] = direct;
        result.Terms[static_cast<sizet>(LightingTerm::DirectSpecular)] = direct;

        // ---- indirect diffuse ------------------------------------------
        LightingTermOwnership indirectDiffuse{ AmbientLadder, TermComposition::Exclusive };
        if (pt)
            indirectDiffuse = { ReSTIRPT, TermComposition::Exclusive };
        else if (gi)
            indirectDiffuse = { ReSTIRGI, TermComposition::Exclusive };
        else if (ssgi)
            indirectDiffuse = { AmbientLadder | SSGI, TermComposition::ConfidenceMixture };
        result.Terms[static_cast<sizet>(LightingTerm::IndirectDiffuse)] = indirectDiffuse;
        result.SSGI = ssgi ? SSGIComposition::ReplacesLadderDiffuseOverResolvedDirections : SSGIComposition::None;

        // ---- indirect specular -----------------------------------------
        // ReSTIR PT answers for the whole specular lobe too; otherwise the
        // reflection tiers of ADR 0020 blend over the probe / IBL bottom tier,
        // which is pinned at confidence one. ReSTIR GI is a DIFFUSE tier and is
        // never an owner here.
        LightingTermOwnership indirectSpecular{};
        if (pt)
        {
            indirectSpecular = { ReSTIRPT, TermComposition::Exclusive };
        }
        else
        {
            LightingEstimator tiers = frame.ProbeOrIBLSpecularActive ? ReflectionProbesIBL : None;
            if (deferred && frame.RayTracedReflectionActive)
                tiers = tiers | RayTracedReflection;
            if (deferred && frame.SSRActive)
                tiers = tiers | ScreenSpaceReflection;
            indirectSpecular = { tiers, CountEstimators(tiers) > 1u ? TermComposition::ConfidenceMixture
                                                                    : TermComposition::Exclusive };
        }
        result.Terms[static_cast<sizet>(LightingTerm::IndirectSpecular)] = indirectSpecular;

        result.Terms[static_cast<sizet>(LightingTerm::Emission)] = { SurfaceEmission, TermComposition::Exclusive };
        result.Terms[static_cast<sizet>(LightingTerm::SurfaceTransmission)] = { SurfaceTransmission,
                                                                                TermComposition::Exclusive };

        result.ScreenSpaceAO = SelectScreenSpaceAOApplication(frame.ScreenSpaceAOProduced, frame.Path);
        result.ContactShadow = SelectContactShadowApplication(frame.ContactShadowsRequested, frame.Path);
        return result;
    }

    [[nodiscard]] constexpr std::string_view ToString(LightingTerm term)
    {
        switch (term)
        {
            case LightingTerm::DirectDiffuse:
                return "DirectDiffuse";
            case LightingTerm::DirectSpecular:
                return "DirectSpecular";
            case LightingTerm::IndirectDiffuse:
                return "IndirectDiffuse";
            case LightingTerm::IndirectSpecular:
                return "IndirectSpecular";
            case LightingTerm::Emission:
                return "Emission";
            case LightingTerm::SurfaceTransmission:
                return "SurfaceTransmission";
            case LightingTerm::Count:
                break;
        }
        return "Unknown";
    }
} // namespace OloEngine
