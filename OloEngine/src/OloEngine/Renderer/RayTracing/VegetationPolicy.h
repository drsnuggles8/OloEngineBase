#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OloEngine::RayTracing
{
    // A vegetation BLAS contains a bounded spatial group, never one leaf.
    // Distant groups retain the full authored geometry at a recent wind time;
    // the proxy changes time resolution rather than replacing a canopy by a quad.
    struct VegetationPolicy
    {
        static constexpr u32 PlantsPerGroup = 32u;
        static constexpr u32 CardPlantsPerGroup = 256u;
        static constexpr u32 ResidentGroups = 4096u;
        static constexpr f32 DetailedCardDistance = 12.0f;
        // Sized from the measured reference fixture, FoliageHierarchicalWind:
        // 3274 authored plants of 112 vertices / 48 triangles and 58970 cards,
        // in 533 groups — a full refresh of 602568 vertices, 275092 triangles
        // and 533 updates, holding 24.3 MiB of live geometry. The per-frame
        // caps have to cover a FULL refresh, not a staggered fraction: reuse
        // needs a frame shorter than the 0.25 m error deadline, and a Debug
        // editor frame (measured 114 ms) is longer than MaximumProxyAge, so
        // every group legitimately refreshes every frame there. A cap below
        // the full refresh does not slow the feature down, it switches it off
        // — refusals are fail-closed and withhold the whole TLAS.
        static constexpr u64 GeometryBytes = 64u * 1024u * 1024u;
        static constexpr u64 AccelerationStructureBytes = 64u * 1024u * 1024u;
        static constexpr u32 UpdatesPerFrame = 1024u;
        static constexpr u32 VerticesPerFrame = 1048576u;
        static constexpr u32 TrianglesPerFrame = 524288u;
        static constexpr f32 MaximumProxyAge = 0.05f;
        static constexpr f32 MaximumWorldDisplacementError = 0.25f;

        // A Lipschitz bound for FoliageWind.glsl. The capped field is a
        // projection onto the radius-20 ball (nonexpansive); sine derivatives
        // and the triangle inequality bound all phases, vertices and roots.
        // Callers pass sanitized weights and the norm of the terrain's linear
        // transform, so the result bounds world-space position, not local units.
        [[nodiscard]] static f32 WindVelocityBound(f32 strength, f32 speed,
                                                   f32 branch, f32 leaf, bool hierarchical,
                                                   bool fieldEnabled, f32 fieldSpeed,
                                                   f32 gustAmplitude, f32 gustFrequency,
                                                   f32 transformNorm)
        {
            for (const f32 value : { strength, speed, branch, leaf, fieldSpeed,
                                     gustAmplitude, gustFrequency, transformNorm })
                if (!std::isfinite(value))
                    return std::numeric_limits<f32>::infinity();
            if (transformNorm < 0.0f || branch < 0.0f || branch > 1.0f || leaf < 0.0f || leaf > 1.0f)
                return std::numeric_limits<f32>::infinity();

            const f32 angularSpeed = std::abs(speed);
            f32 trunkRate = 1.118034f * 1.7f * angularSpeed;
            f32 trunkExtent = 1.118034f;
            if (fieldEnabled)
            {
                trunkRate = 0.1f * std::abs(fieldSpeed * gustAmplitude * gustFrequency) * 6.28318530718f;
                trunkExtent = hierarchical ? 2.0f : 0.1f * std::abs(fieldSpeed) * (1.0f + std::abs(gustAmplitude));
            }
            if (hierarchical)
                trunkRate += trunkExtent * 0.2f * 0.8f * angularSpeed;
            const f32 branchRate = 1.118034f * 0.3f * branch * 1.7f * angularSpeed;
            const f32 leafRate = 1.118034f * 0.13f * leaf * 8.3f * angularSpeed;
            return std::abs(strength) * (trunkRate + branchRate + leafRate) * transformNorm;
        }

        [[nodiscard]] static f32 ProxyAgeLimit(f32 velocityBound)
        {
            if (!std::isfinite(velocityBound) || velocityBound < 0.0f)
                return 0.0f;
            return velocityBound > 0.0f
                       ? std::min(MaximumProxyAge, MaximumWorldDisplacementError / velocityBound)
                       : MaximumProxyAge;
        }

        [[nodiscard]] static bool CanReuseSnapshot(f32 currentTime, f32 snapshotTime,
                                                   f32 velocityBound, bool historyContinuous)
        {
            if (!historyContinuous || !std::isfinite(currentTime) || !std::isfinite(snapshotTime))
                return false;
            const f32 age = currentTime - snapshotTime;
            return age >= 0.0f && age < ProxyAgeLimit(velocityBound);
        }
    };

    // Reservation is transactional: an oversized request consumes no budget,
    // so one unavailable group cannot starve the remaining work. Subtraction
    // guards addition and multiplication stays in the caller's checked u64.
    struct VegetationFrameBudget
    {
        u32 Updates = 0u;
        u32 Vertices = 0u;
        u32 Triangles = 0u;

        [[nodiscard]] bool Reserve(u64 vertices, u64 triangles)
        {
            if (Updates >= VegetationPolicy::UpdatesPerFrame ||
                vertices > VegetationPolicy::VerticesPerFrame - Vertices ||
                triangles > VegetationPolicy::TrianglesPerFrame - Triangles)
                return false;
            ++Updates;
            Vertices += static_cast<u32>(vertices);
            Triangles += static_cast<u32>(triangles);
            return true;
        }
    };
} // namespace OloEngine::RayTracing
