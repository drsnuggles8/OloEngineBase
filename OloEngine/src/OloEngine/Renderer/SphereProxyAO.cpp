#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SphereProxyAO.h"

#include <array>
#include <cmath>
#include <limits>

namespace OloEngine::SphereProxyAO
{
    namespace
    {
        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // The volume-matched radius of `count` equal spheres filling a box of the
        // given half-extents: count * (4/3)pi r^3 == 8 * ex * ey * ez.
        [[nodiscard]] f32 VolumeMatchedRadius(const glm::vec3& halfExtents, u32 count)
        {
            const f32 boxVolume = 8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
            const f32 perSphere = boxVolume / static_cast<f32>(count);
            return std::cbrt(perSphere * 3.0f / (4.0f * glm::pi<f32>()));
        }
    } // namespace

    u32 FitProxySpheres(const BoundingBox& bounds, f32 maxRadius, Proxy* out, u32 outCapacity)
    {
        if (out == nullptr || outCapacity == 0u)
            return 0u;

        // The NoBounds sentinel is Min = +FLT_MAX, Max = -FLT_MAX, so the
        // ordering test below rejects it along with any genuinely inverted box.
        if (!IsFinite(bounds.Min) || !IsFinite(bounds.Max))
            return 0u;
        if (!(bounds.Max.x > bounds.Min.x) || !(bounds.Max.y > bounds.Min.y) || !(bounds.Max.z > bounds.Min.z))
            return 0u;

        const glm::vec3 centre = bounds.GetCenter();
        const glm::vec3 halfExtents = bounds.GetExtents();
        if (!IsFinite(centre) || !IsFinite(halfExtents))
            return 0u;

        // Longest axis: the one a multi-sphere fit is laid out along.
        u32 longAxis = 0u;
        for (u32 axis = 1u; axis < 3u; ++axis)
        {
            if (halfExtents[static_cast<i32>(axis)] > halfExtents[static_cast<i32>(longAxis)])
                longAxis = axis;
        }

        // The cross-section the spheres have to cover is the box's other two
        // half-extents; a sphere line whose spheres are that fat needs
        // roughly `long / crossRadius` of them to span the long axis. The
        // area-matched disc radius (pi r^2 == 4 ex ey) is the balanced choice
        // for the same reason the volume match is, one dimension down.
        std::array<f32, 2> cross{};
        for (u32 axis = 0u, next = 0u; axis < 3u; ++axis)
        {
            if (axis != longAxis)
                cross[next++] = halfExtents[static_cast<i32>(axis)];
        }
        const f32 crossRadius = 2.0f * std::sqrt(std::max(cross[0] * cross[1], 0.0f) / glm::pi<f32>());

        u32 count = 1u;
        if (crossRadius > 0.0f)
        {
            const f32 ratio = halfExtents[static_cast<i32>(longAxis)] / crossRadius;
            count = static_cast<u32>(std::lround(std::max(ratio, 1.0f)));
        }
        count = std::clamp(count, 1u, std::min(kMaxSpheresPerBounds, outCapacity));

        // Contained: capped at the smallest half-extent, so the sphere fits
        // inside the box on every axis and cannot poke through the object's own
        // surface. See the header for why that matters more than the extra
        // volume a circumscribing fit would buy.
        const f32 smallestHalfExtent = std::min({ halfExtents.x, halfExtents.y, halfExtents.z });
        const f32 radius = std::min(VolumeMatchedRadius(halfExtents, count), smallestHalfExtent);
        if (!std::isfinite(radius) || !(radius > 0.0f) || radius > maxRadius)
            return 0u;

        glm::vec3 axisDir{ 0.0f };
        axisDir[static_cast<i32>(longAxis)] = 1.0f;
        const f32 halfLength = halfExtents[static_cast<i32>(longAxis)];

        for (u32 i = 0u; i < count; ++i)
        {
            // Centres of `count` equal segments of the long axis: the i-th
            // segment's midpoint, in [-halfLength, +halfLength].
            const f32 t = (2.0f * static_cast<f32>(i) + 1.0f) / static_cast<f32>(count) - 1.0f;
            out[i].Center = centre + axisDir * (halfLength * t);
            out[i].Radius = radius;
        }

        return count;
    }

    std::vector<Proxy> SelectProxies(std::span<const BoundingBox> bounds, const glm::vec3& viewPosition,
                                     u32 maxCount, f32 maxRadius)
    {
        std::vector<Proxy> proxies;
        if (maxCount == 0u || bounds.empty())
            return proxies;

        // The loop below fits EVERY candidate before the budget applies — the
        // ranking needs all of them — so reserve for that, not for the budget.
        // Capped so a pathological scene cannot reserve unboundedly.
        constexpr sizet kReserveCeiling = 8192;
        proxies.reserve(std::min<sizet>(bounds.size() * kMaxSpheresPerBounds, kReserveCeiling));

        std::array<Proxy, kMaxSpheresPerBounds> fitted{};
        for (const auto& box : bounds)
        {
            const u32 count = FitProxySpheres(box, maxRadius, fitted.data(), kMaxSpheresPerBounds);
            for (u32 i = 0u; i < count; ++i)
                proxies.push_back(fitted[i]);
        }

        if (proxies.size() <= maxCount)
            return proxies;

        // Rank by the peak occlusion the proxy can produce: nl / h^2 with
        // nl = 1, i.e. r^2 / d^2. A receiver ON the proxy would read 1, so the
        // distance is floored at the radius to keep the score finite and to stop
        // a proxy the camera is standing inside from outranking everything by
        // several orders of magnitude.
        const auto score = [&viewPosition](const Proxy& p)
        {
            const glm::vec3 delta = p.Center - viewPosition;
            const f32 distanceSq = std::max(glm::dot(delta, delta), p.Radius * p.Radius);
            return (p.Radius * p.Radius) / std::max(distanceSq, kMinCentreDistance);
        };

        std::ranges::nth_element(proxies, proxies.begin() + static_cast<std::ptrdiff_t>(maxCount),
                                 [&score](const Proxy& a, const Proxy& b)
                                 { return score(a) > score(b); });
        proxies.resize(maxCount);
        return proxies;
    }
} // namespace OloEngine::SphereProxyAO
