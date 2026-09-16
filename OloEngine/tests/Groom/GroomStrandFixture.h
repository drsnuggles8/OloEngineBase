#pragma once

// =============================================================================
// GroomStrandFixture.h — Alembic-free groom generators for the strand
// visibility work (#1246).
//
// WHY NOT GroomAlembicFixture.h. That header is the #1232 fixture and is
// gated on OLO_WITH_ALEMBIC, because its job is to exercise the IMPORTER. The
// coverage comparison has nothing to do with import: it needs curves in object
// space and nothing else, and gating it on an optional dependency would mean
// the measured evidence behind an architectural decision stops being produced
// on any build that happens not to have Alembic. So these go through
// GroomBuilder directly.
//
// The shapes are deliberately the same two the #1232 editor fixtures use — a
// human scalp and an animal pelt — so a number measured here and a picture
// rendered from `Assets/Grooms/reference-*.ologroom` are about the same coat.
// They are generated rather than loaded so the test needs no project mount and
// no asset registry, and so a case can ask for a strand count the committed
// fixtures do not have.
//
// Determinism: every value comes from a fixed-seed integer hash, never from
// <random>, for the reason std-distributions-are-not-portable.md gives — a
// libstdc++ and an MSVC run must agree or the measured comparison stops being
// comparable across the machines that produce it.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"

#include <glm/glm.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Tests::GroomStrandFixture
{
    // Deterministic [0,1) hash. Same construction as
    // generate_reference_animals.py's `noise`, and for the same reason: a
    // committed measurement must not move because a standard library changed
    // its generator stream.
    [[nodiscard]] inline f32 Noise(u32 i, u32 salt) noexcept
    {
        u32 h = (i * 73856093u) ^ ((salt + 1246u) * 83492791u);
        h ^= h >> 13;
        h *= 1274126177u;
        h ^= h >> 16;
        return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
    }

    struct StrandCoat
    {
        Ref<GroomAsset> Groom;
        std::string FailureReason;
    };

    // A human scalp: `strandCount` strands growing off the upper hemisphere of
    // a `skullRadius` sphere centred at the origin, falling under a light
    // curl, each `pointsPerStrand` control points, tapering from `rootDiameter`
    // to a tenth of it at the tip.
    //
    // The diameters are REAL: a human hair is 17-180 um, so 7e-5 m is a fine
    // hair and is what the #1232 fixture uses. That number is what makes this a
    // sub-pixel coverage problem rather than a ribbon-rendering one, and
    // inflating it to something comfortable would quietly remove the whole
    // difficulty the comparison exists to measure.
    [[nodiscard]] inline StrandCoat MakeScalp(u32 strandCount, u32 pointsPerStrand, f32 skullRadius = 0.09f,
                                              f32 strandLength = 0.25f, f32 rootDiameter = 7.0e-5f)
    {
        StrandCoat coat;
        GroomBuilder builder;
        u16 group = 0;
        if (!builder.AddGroup("scalp", group, coat.FailureReason))
        {
            return coat;
        }

        std::vector<glm::vec3> points(pointsPerStrand);
        std::vector<f32> widths(pointsPerStrand);

        for (u32 s = 0; s < strandCount; ++s)
        {
            // Fibonacci-sphere placement restricted to the upper hemisphere:
            // an even cover with no pole clustering, and a pure function of the
            // index.
            const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(strandCount);
            const f32 cosTheta = 1.0f - t * 0.85f; // 0.15..1.0 — a scalp, not a full sphere
            const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            const f32 phi = static_cast<f32>(s) * 2.399963f; // golden angle

            const glm::vec3 normal{ sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi) };
            const glm::vec3 root = normal * skullRadius;

            // Gravity bends the strand down as it leaves the scalp; the jitter
            // keeps neighbouring strands from being exactly parallel, which is
            // what makes the dense-overlap case a real one.
            const f32 jitter = 0.6f + 0.8f * Noise(s, 3u);
            for (u32 p = 0; p < pointsPerStrand; ++p)
            {
                const f32 u = static_cast<f32>(p) / static_cast<f32>(pointsPerStrand - 1u);
                const glm::vec3 along = normal * (strandLength * u * jitter);
                const glm::vec3 droop{ 0.0f, -strandLength * u * u * 0.9f, 0.0f };
                const glm::vec3 curl{ std::sin(u * 6.0f + phi) * 0.01f * u, 0.0f,
                                      std::cos(u * 6.0f + phi) * 0.01f * u };
                points[p] = root + along + droop + curl;
                widths[p] = rootDiameter * (1.0f - 0.9f * u);
            }

            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { phi / 6.2831853f, cosTheta };
            input.GroupId = group;
            input.IsGuide = (s % 64u) == 0u;
            if (!builder.AddCurve(input, coat.FailureReason))
            {
                return coat;
            }
        }

        coat.Groom = builder.Build(coat.FailureReason);
        if (coat.Groom && !GroomCooker::Canonicalize(*coat.Groom, coat.FailureReason))
        {
            coat.Groom = nullptr;
        }
        return coat;
    }

    // An animal pelt: short, dense fur over the whole of a `bodyRadius` sphere,
    // in `subGroupCount` interleaved groups. Coarser than hair — a guard hair
    // is 50-120 um — and much shorter, so its overlap is depth complexity
    // rather than length.
    [[nodiscard]] inline StrandCoat MakePelt(u32 strandCount, u32 pointsPerStrand, u32 subGroupCount = 3u,
                                             f32 bodyRadius = 0.12f, f32 strandLength = 0.025f,
                                             f32 rootDiameter = 1.1e-4f)
    {
        StrandCoat coat;
        GroomBuilder builder;
        std::vector<u16> groups(subGroupCount);
        for (u32 g = 0; g < subGroupCount; ++g)
        {
            if (!builder.AddGroup("pelt" + std::to_string(g), groups[g], coat.FailureReason))
            {
                return coat;
            }
        }

        std::vector<glm::vec3> points(pointsPerStrand);
        std::vector<f32> widths(pointsPerStrand);

        for (u32 s = 0; s < strandCount; ++s)
        {
            const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(strandCount);
            const f32 cosTheta = 1.0f - 2.0f * t; // the whole sphere
            const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            const f32 phi = static_cast<f32>(s) * 2.399963f;

            const glm::vec3 normal{ sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi) };
            const glm::vec3 root = normal * bodyRadius;
            // A lie direction so the fur lies along the body rather than
            // standing on end; without it the pelt is a pin cushion and its
            // overlap statistics are nothing like a coat's.
            const glm::vec3 lie = glm::normalize(glm::vec3{ -normal.z, 0.25f, normal.x } + normal * 0.6f);
            const f32 lengthJitter = 0.7f + 0.6f * Noise(s, 7u);

            for (u32 p = 0; p < pointsPerStrand; ++p)
            {
                const f32 u = static_cast<f32>(p) / static_cast<f32>(pointsPerStrand - 1u);
                points[p] = root + lie * (strandLength * u * lengthJitter);
                widths[p] = rootDiameter * (1.0f - 0.75f * u);
            }

            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { phi / 6.2831853f, cosTheta * 0.5f + 0.5f };
            input.GroupId = groups[s % subGroupCount];
            input.IsGuide = (s % 17u) == 0u;
            if (!builder.AddCurve(input, coat.FailureReason))
            {
                return coat;
            }
        }

        coat.Groom = builder.Build(coat.FailureReason);
        if (coat.Groom && !GroomCooker::Canonicalize(*coat.Groom, coat.FailureReason))
        {
            coat.Groom = nullptr;
        }
        return coat;
    }
} // namespace OloEngine::Tests::GroomStrandFixture
