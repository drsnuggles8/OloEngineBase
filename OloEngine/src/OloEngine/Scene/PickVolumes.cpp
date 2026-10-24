#include "OloEnginePCH.h"
#include "OloEngine/Scene/PickVolumes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OloEngine::PickVolumes
{
    namespace
    {
        // Entry distance of a ray into a sphere, or false.
        [[nodiscard]] bool RaySphere(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& centre, f32 radius,
                                     f32& outDistance)
        {
            const glm::vec3 m = origin - centre;
            const f32 b = glm::dot(m, direction);
            const f32 c = glm::dot(m, m) - radius * radius;
            if (c <= 0.0f)
            {
                outDistance = 0.0f; // starts inside
                return true;
            }
            if (b > 0.0f)
            {
                return false; // outside and pointing away
            }
            const f32 discriminant = b * b - c;
            if (discriminant < 0.0f)
            {
                return false;
            }
            outDistance = -b - std::sqrt(discriminant);
            return outDistance >= 0.0f;
        }

        [[nodiscard]] std::string_view Trim(std::string_view text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
            {
                return {};
            }
            const auto last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }
    } // namespace

    bool RayCapsule(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a, const glm::vec3& b, f32 radius,
                    f32 maxDistance, f32& outDistance)
    {
        if (!(radius > 0.0f) || !std::isfinite(radius))
        {
            return false;
        }

        // The cylinder part (Ericson, Real-Time Collision Detection 5.3.7, with
        // the end caps handled as spheres below).
        f32 best = std::numeric_limits<f32>::max();
        bool hit = false;
        const glm::vec3 axis = b - a;
        const f32 axisLength2 = glm::dot(axis, axis);
        if (axisLength2 > 1.0e-12f)
        {
            const glm::vec3 m = origin - a;
            const f32 md = glm::dot(m, axis);
            const f32 nd = glm::dot(direction, axis);
            const f32 dd = axisLength2;
            const f32 nn = glm::dot(direction, direction);
            const f32 mn = glm::dot(m, direction);
            const f32 aCoef = dd * nn - nd * nd;
            const f32 k = glm::dot(m, m) - radius * radius;
            const f32 c = dd * k - md * md;
            if (std::abs(aCoef) > 1.0e-12f)
            {
                const f32 bCoef = dd * mn - nd * md;
                const f32 discriminant = bCoef * bCoef - aCoef * c;
                if (discriminant >= 0.0f)
                {
                    f32 t = (-bCoef - std::sqrt(discriminant)) / aCoef;
                    if (c <= 0.0f)
                    {
                        t = 0.0f; // the origin is inside the infinite cylinder
                    }
                    const f32 along = md + t * nd;
                    if (t >= 0.0f && along >= 0.0f && along <= dd)
                    {
                        best = t;
                        hit = true;
                    }
                }
            }
        }

        f32 capDistance = 0.0f;
        if (RaySphere(origin, direction, a, radius, capDistance) && capDistance < best)
        {
            best = capDistance;
            hit = true;
        }
        if (RaySphere(origin, direction, b, radius, capDistance) && capDistance < best)
        {
            best = capDistance;
            hit = true;
        }
        if (!hit || best > maxDistance)
        {
            return false;
        }
        outDistance = best;
        return true;
    }

    bool RayTriangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& c, f32 maxDistance, f32& outDistance)
    {
        constexpr f32 kEpsilon = 1.0e-9f;
        const glm::vec3 edge1 = b - a;
        const glm::vec3 edge2 = c - a;
        const glm::vec3 p = glm::cross(direction, edge2);
        const f32 determinant = glm::dot(edge1, p);
        if (std::abs(determinant) < kEpsilon)
        {
            return false; // parallel to the triangle's plane
        }
        const f32 inverse = 1.0f / determinant;
        const glm::vec3 s = origin - a;
        const f32 u = glm::dot(s, p) * inverse;
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }
        const glm::vec3 q = glm::cross(s, edge1);
        const f32 v = glm::dot(direction, q) * inverse;
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }
        const f32 t = glm::dot(edge2, q) * inverse;
        if (!(t >= 0.0f) || t > maxDistance)
        {
            return false;
        }
        outDistance = t;
        return true;
    }

    std::unordered_map<std::string, std::string> ParseRegions(std::string_view text)
    {
        std::unordered_map<std::string, std::string> boneToRegion;
        sizet regionStart = 0;
        while (regionStart <= text.size())
        {
            const sizet regionEnd = std::min(text.find(';', regionStart), text.size());
            const std::string_view region = text.substr(regionStart, regionEnd - regionStart);
            regionStart = regionEnd + 1;

            const sizet colon = region.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }
            const std::string_view name = Trim(region.substr(0, colon));
            if (name.empty())
            {
                continue;
            }
            const std::string_view bones = region.substr(colon + 1);
            sizet boneStart = 0;
            while (boneStart <= bones.size())
            {
                const sizet boneEnd = std::min(bones.find(',', boneStart), bones.size());
                const std::string_view bone = Trim(bones.substr(boneStart, boneEnd - boneStart));
                boneStart = boneEnd + 1;
                if (!bone.empty())
                {
                    boneToRegion.try_emplace(std::string(bone), std::string(name));
                }
            }
        }
        return boneToRegion;
    }
} // namespace OloEngine::PickVolumes
