#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomRayTracingProxy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace OloEngine
{
    namespace
    {
        // A unit vector perpendicular to `tangent`, chosen DETERMINISTICALLY
        // and without a branch anyone has to reason about at a use site.
        //
        // The axis is picked by the tangent's SMALLEST component, so the cross
        // product is never near-degenerate: the chosen axis makes an angle of
        // at least 54.7 degrees with the tangent (the worst case is a tangent
        // with three equal components), and sin of that is 0.816. Picking a
        // fixed axis instead would produce a zero-length cross for every strand
        // that happens to run along it — one flank of an animal, silently
        // missing from every shadow.
        [[nodiscard]] glm::vec3 PerpendicularTo(const glm::vec3& tangent) noexcept
        {
            const glm::vec3 magnitude{ std::abs(tangent.x), std::abs(tangent.y), std::abs(tangent.z) };
            glm::vec3 axis{ 0.0f, 0.0f, 1.0f };
            if (magnitude.x <= magnitude.y && magnitude.x <= magnitude.z)
            {
                axis = glm::vec3{ 1.0f, 0.0f, 0.0f };
            }
            else if (magnitude.y <= magnitude.z)
            {
                axis = glm::vec3{ 0.0f, 1.0f, 0.0f };
            }
            return glm::normalize(glm::cross(tangent, axis));
        }

        [[nodiscard]] bool IsFinite(const glm::vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
    } // namespace

    f32 GroomProxyWidthCompensation(f32 achievedFraction, f32 maxScale) noexcept
    {
        // A non-finite or non-positive fraction is a build that retained
        // nothing, or a corrupt one. Either way the honest compensation is
        // NONE: widening on a fraction nobody can name would produce a coat of
        // arbitrary thickness, which is worse than a thin one.
        if (!std::isfinite(achievedFraction) || achievedFraction <= 0.0f)
        {
            return 1.0f;
        }
        const f32 cap = std::isfinite(maxScale) && maxScale >= 1.0f ? maxScale : 1.0f;
        // Never below 1: a build that retained MORE than it asked for (a groom
        // with fewer curves than the budget) must not thin the coat down.
        return std::clamp(1.0f / achievedFraction, 1.0f, cap);
    }

    GroomProxyDecision AdvanceGroomProxyTier(const GroomProxyInputs& inputs, GroomProxyState& state) noexcept
    {
        GroomProxyDecision decision;
        decision.PixelSize = std::isfinite(inputs.PixelSize) ? inputs.PixelSize : 0.0f;

        // THE TIER IS ADVANCED EVEN WHEN THE COAT IS REFUSED, and that is not
        // an oversight. A groom that leaves the ray-traced scene for a few
        // frames — the budget was spent, the frame asked for no hybrid effect —
        // and comes back should come back on the tier its apparent size
        // selects, not on whatever tier it held when it left. Freezing the
        // state on a refusal would make a coat that reappears at two metres do
        // so on the proxy tier and then wait out the hold before refining.
        const f32 band = GroomProxyPolicy::DetailedPixelSize * GroomProxyPolicy::Hysteresis;
        // The band moves so as to HOLD what the coat already has: on the
        // detailed tier you must shrink past the low edge to hand over, and on
        // the proxy tier you must grow past the high edge to come back. ONE
        // offset applied to both edges, so the band moves bodily rather than
        // changing width — GroomLodPolicy::Hysteresis' rule and its reason.
        const f32 threshold = state.Tier == GroomProxyTier::Detailed
                                  ? GroomProxyPolicy::DetailedPixelSize - band
                                  : GroomProxyPolicy::DetailedPixelSize + band;
        const GroomProxyTier requested =
            decision.PixelSize >= threshold ? GroomProxyTier::Detailed : GroomProxyTier::Proxy;

        state.StableFrames = requested == state.Requested ? state.StableFrames + 1u : 1u;
        state.Requested = requested;

        const bool refining = requested < state.Tier;
        const bool coarsening = requested > state.Tier;
        // Refining is immediate; coarsening waits out the hold. See
        // GroomProxyPolicy::HoldFrames for which of the two is a picture.
        if (refining || (coarsening && state.StableFrames >= GroomProxyPolicy::HoldFrames))
        {
            decision.TierChanged = state.Tier != requested;
            state.Tier = requested;
        }

        decision.Tier = state.Tier;
        decision.StrandBudget = GroomProxyPolicy::StrandBudget(state.Tier);

        if (!inputs.Requested)
        {
            decision.Reason = GroomProxyRefusalReason::NotRequested;
            return decision;
        }
        if (inputs.StrandsAvailable == 0u)
        {
            decision.Reason = GroomProxyRefusalReason::GroomHasNoGeometry;
            return decision;
        }

        decision.Reason = GroomProxyRefusalReason::None;
        return decision;
    }

    GroomProxyMeshStats ConvertGroomStrandMeshToProxy(std::span<const GroomStrandVertex> strandVertices,
                                                      const GroomProxyConversionSettings& settings,
                                                      std::vector<Vertex>& outVertices, std::vector<u32>& outIndices,
                                                      glm::vec3* outBoundsMin, glm::vec3* outBoundsMax)
    {
        outVertices.clear();
        outIndices.clear();

        GroomProxyMeshStats stats;
        glm::vec3 boundsMin{ std::numeric_limits<f32>::max() };
        glm::vec3 boundsMax{ std::numeric_limits<f32>::lowest() };

        // A width scale that is not a usable number would produce a coat of
        // arbitrary thickness. Refuse the SCALE, not the coat: 1.0 renders the
        // uncompensated strand set, which is thin rather than wrong.
        const f32 widthScale = std::isfinite(settings.WidthScale) && settings.WidthScale > 0.0f
                                   ? settings.WidthScale
                                   : 1.0f;
        const u32 ribbons = settings.CrossedRibbons ? 2u : 1u;

        const sizet segments = strandVertices.size() / 4u;
        outVertices.reserve(segments * 4u * ribbons);
        outIndices.reserve(segments * 6u * ribbons);

        for (sizet segment = 0; segment < segments; ++segment)
        {
            // Corner 0 is (-side at P0) and corner 2 is (+side at P1) — the
            // order BuildGroomStrandMesh emits and the header names. The two
            // corners at each end carry the same Position and Radius, so
            // either of the pair would do; taking 0 and 2 keeps the read
            // adjacent to the quad's own layout.
            const GroomStrandVertex& start = strandVertices[segment * 4u + 0u];
            const GroomStrandVertex& end = strandVertices[segment * 4u + 2u];

            const glm::vec3 p0 = start.Position;
            const glm::vec3 p1 = end.Position;
            const f32 r0 = start.Radius * widthScale;
            const f32 r1 = end.Radius * widthScale;

            const glm::vec3 delta = p1 - p0;
            const f32 length = glm::length(delta);
            // A degenerate or non-finite segment is DROPPED AND COUNTED, never
            // emitted: a zero-length tangent normalises to NaN, and a NaN
            // vertex in a BLAS build is undefined behaviour at the device with
            // no validation message — the acceleration structure is simply
            // wrong afterwards, for every ray, for the rest of the session.
            if (!IsFinite(p0) || !IsFinite(p1) || !std::isfinite(r0) || !std::isfinite(r1) || r0 < 0.0f ||
                r1 < 0.0f || !(length > 0.0f) || !std::isfinite(length))
            {
                ++stats.SegmentsDropped;
                continue;
            }

            const glm::vec3 tangent = delta / length;
            const glm::vec3 u = PerpendicularTo(tangent);
            const glm::vec3 v = glm::cross(tangent, u);
            if (!IsFinite(u) || !IsFinite(v))
            {
                ++stats.SegmentsDropped;
                continue;
            }

            for (u32 ribbon = 0; ribbon < ribbons; ++ribbon)
            {
                // The ribbon lies in the plane spanned by the tangent and
                // `across`; its NORMAL is the other perpendicular, which is
                // what makes the two ribbons of a pair face at right angles and
                // is the whole reason a pair has a silhouette from everywhere.
                const glm::vec3 across = ribbon == 0u ? u : v;
                const glm::vec3 normal = ribbon == 0u ? v : u;

                const u32 base = static_cast<u32>(outVertices.size());
                outVertices.push_back({ p0 - across * r0, normal, glm::vec2{ 0.0f, 0.0f } });
                outVertices.push_back({ p0 + across * r0, normal, glm::vec2{ 1.0f, 0.0f } });
                outVertices.push_back({ p1 + across * r1, normal, glm::vec2{ 1.0f, 1.0f } });
                outVertices.push_back({ p1 - across * r1, normal, glm::vec2{ 0.0f, 1.0f } });

                outIndices.push_back(base + 0u);
                outIndices.push_back(base + 1u);
                outIndices.push_back(base + 2u);
                outIndices.push_back(base + 0u);
                outIndices.push_back(base + 2u);
                outIndices.push_back(base + 3u);
            }

            const f32 radius = std::max(r0, r1);
            const glm::vec3 expand{ radius };
            boundsMin = glm::min(boundsMin, glm::min(p0, p1) - expand);
            boundsMax = glm::max(boundsMax, glm::max(p0, p1) + expand);
            ++stats.SegmentCount;
        }

        stats.VertexCount = static_cast<u32>(outVertices.size());
        stats.IndexCount = static_cast<u32>(outIndices.size());
        stats.VertexBytes = static_cast<u64>(stats.VertexCount) * sizeof(Vertex);
        stats.IndexBytes = static_cast<u64>(stats.IndexCount) * sizeof(u32);

        if (stats.SegmentCount == 0u)
        {
            boundsMin = glm::vec3{ 0.0f };
            boundsMax = glm::vec3{ 0.0f };
        }
        if (outBoundsMin != nullptr)
        {
            *outBoundsMin = boundsMin;
        }
        if (outBoundsMax != nullptr)
        {
            *outBoundsMax = boundsMax;
        }
        return stats;
    }

    void CollectGroomProxySegments(std::span<const GroomStrandVertex> strandVertices, f32 widthScale,
                                   std::vector<GroomCoatShadow::CoatSegment>& outSegments)
    {
        outSegments.clear();
        const f32 scale = std::isfinite(widthScale) && widthScale > 0.0f ? widthScale : 1.0f;
        const sizet segments = strandVertices.size() / 4u;
        outSegments.reserve(segments);
        for (sizet segment = 0; segment < segments; ++segment)
        {
            const GroomStrandVertex& start = strandVertices[segment * 4u + 0u];
            const GroomStrandVertex& end = strandVertices[segment * 4u + 2u];
            if (!IsFinite(start.Position) || !IsFinite(end.Position) || !std::isfinite(start.Radius) ||
                !std::isfinite(end.Radius))
            {
                continue;
            }
            outSegments.push_back({ start.Position, end.Position, start.Radius * scale, end.Radius * scale });
        }
    }

    f64 DirectionalCoverage(std::span<const GroomCoatShadow::CoatSegment> segments,
                            const glm::vec3& direction) noexcept
    {
        const f32 directionLength = glm::length(direction);
        if (segments.empty() || !std::isfinite(directionLength) || directionLength <= 0.0f)
        {
            return 0.0;
        }
        const glm::dvec3 w = glm::dvec3(direction) / static_cast<f64>(directionLength);

        f64 total = 0.0;
        for (const auto& segment : segments)
        {
            const glm::dvec3 delta = glm::dvec3(segment.B) - glm::dvec3(segment.A);
            const f64 length = glm::length(delta);
            if (!(length > 0.0) || !std::isfinite(length))
            {
                continue;
            }
            // sin of the angle between the segment and the direction, from the
            // cross product rather than from an acos: the acos loses precision
            // exactly where sin is small, which is the grazing case that
            // contributes least and would round to a different small number on
            // a different compiler.
            const f64 sine = glm::length(glm::cross(delta / length, w));
            const f64 meanRadius = 0.5 * (static_cast<f64>(segment.RadiusA) + static_cast<f64>(segment.RadiusB));
            if (!std::isfinite(sine) || !std::isfinite(meanRadius) || meanRadius < 0.0)
            {
                continue;
            }
            total += 2.0 * meanRadius * length * sine;
        }
        return total;
    }

    std::vector<glm::vec3> GroomProxyErrorDirections(u32 count)
    {
        std::vector<glm::vec3> directions;
        if (count == 0u)
        {
            return directions;
        }
        directions.reserve(count);
        // The Fibonacci sphere: z sweeps the interval uniformly and the
        // azimuth advances by the golden angle, which spreads the points
        // without clustering at the poles a latitude/longitude grid produces.
        // Every term is exact in f64 on every machine this builds on, so the
        // set is the same set everywhere — which is what makes an error figure
        // in the analysis document re-derivable rather than reported.
        const f64 goldenAngle = std::numbers::pi_v<f64> * (3.0 - std::sqrt(5.0));
        for (u32 index = 0; index < count; ++index)
        {
            const f64 z = 1.0 - 2.0 * (static_cast<f64>(index) + 0.5) / static_cast<f64>(count);
            const f64 radius = std::sqrt(std::max(0.0, 1.0 - z * z));
            const f64 theta = goldenAngle * static_cast<f64>(index);
            directions.push_back(glm::vec3{ static_cast<f32>(radius * std::cos(theta)),
                                            static_cast<f32>(radius * std::sin(theta)), static_cast<f32>(z) });
        }
        return directions;
    }

    GroomProxyCoverageError CompareGroomProxyCoverage(std::span<const GroomCoatShadow::CoatSegment> detailed,
                                                      std::span<const GroomCoatShadow::CoatSegment> proxy,
                                                      u32 directions)
    {
        GroomProxyCoverageError error;
        const auto set = GroomProxyErrorDirections(directions);
        if (set.empty())
        {
            return error;
        }

        f64 meanSum = 0.0;
        f64 detailedSum = 0.0;
        f64 proxySum = 0.0;
        u32 compared = 0;
        for (const auto& direction : set)
        {
            const f64 reference = DirectionalCoverage(detailed, direction);
            const f64 measured = DirectionalCoverage(proxy, direction);
            detailedSum += reference;
            proxySum += measured;
            // A direction the reference coat projects NOTHING along is not a
            // direction the proxy can be wrong about by a ratio. Skipped rather
            // than scored as zero error or as infinite error — both of those
            // are claims about a measurement that was not taken.
            if (!(reference > 0.0))
            {
                continue;
            }
            const f64 relative = std::abs(measured / reference - 1.0);
            meanSum += relative;
            error.MaxRelativeError = std::max(error.MaxRelativeError, relative);
            ++compared;
        }

        error.Directions = compared;
        error.MeanRelativeError = compared > 0u ? meanSum / static_cast<f64>(compared) : 0.0;
        error.DetailedCoverage = detailedSum / static_cast<f64>(set.size());
        error.ProxyCoverage = proxySum / static_cast<f64>(set.size());
        return error;
    }
} // namespace OloEngine
