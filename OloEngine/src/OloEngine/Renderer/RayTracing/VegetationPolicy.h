#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

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

        // Whether a foliage layer enters the ray-traced scene (#1533). Shadow
        // rays and reflection rays are vegetation's only readers, and a layer
        // that casts no raster shadow is masked out of shadow rays to match
        // the raster tier. Without reflections nothing would read it: building
        // it costs geometry and acceleration structures for no ray, and past
        // the budget its refusals withhold the whole TLAS -- the showcase
        // dog's non-casting lawn switched off every ray-traced shadow that way.
        [[nodiscard]] static constexpr bool TracesLayer(bool castsShadows, bool reflectionsReadVegetation)
        {
            return castsShadows || reflectionsReadVegetation;
        }

        // THE REFLECTION-ONLY SHARE (#1533). A layer that casts no shadow is
        // traced for reflection rays alone (TracesLayer), and nothing it leaves
        // out can leak light: shadow rays do not see it. So its groups need not
        // all enter the scene, nor at full detail. Every group has a CARD tier
        // (one quad per plant, the mesh's bake) and a group inside the raster's
        // mesh distance also has a MESH tier (the authored plant). Cards are
        // admitted first, nearest first, while their geometry AND their
        // acceleration structures fit what the casting layers left; then the
        // nearest card groups are upgraded to the mesh while the difference
        // fits. Each pass stops at the first group that does not fit, so each
        // tier is a disc around the camera rather than a scatter of small far
        // groups. What is left is out -- counted, not refused, so it does not
        // withhold the TLAS from every ray-traced pass.
        //
        // The showcase lawn: 216k plants inside its 45 m view distance. As
        // authored tufts (1,257 triangles each) the geometry budget held 1,100
        // of them, out to 1.2 m, and their structures overran the 64 MiB
        // acceleration-structure cap at 26 groups, so the backend dropped the
        // rest every frame and the TLAS never came back.
        struct ReflectionGroupCost
        {
            u64 CardGeometryBytes = 0u;
            u64 CardAccelerationBytes = 0u;
            /// Both zero: the group has no mesh tier.
            u64 MeshGeometryBytes = 0u;
            u64 MeshAccelerationBytes = 0u;
        };
        enum class ReflectionTier : u8
        {
            Out,
            Card,
            Mesh
        };
        static void ChooseReflectionTiers(std::span<const ReflectionGroupCost> nearestFirst, u64 geometryTaken,
                                          u64 accelerationTaken, std::span<ReflectionTier> tiers) noexcept
        {
            std::ranges::fill(tiers, ReflectionTier::Out);
            u64 geometryRoom = geometryTaken < GeometryBytes ? GeometryBytes - geometryTaken : 0u;
            u64 accelerationRoom = accelerationTaken < AccelerationStructureBytes ? AccelerationStructureBytes - accelerationTaken : 0u;
            const sizet count = std::min(nearestFirst.size(), tiers.size());
            for (sizet i = 0u; i < count; ++i)
            {
                const ReflectionGroupCost& cost = nearestFirst[i];
                if (cost.CardGeometryBytes > geometryRoom || cost.CardAccelerationBytes > accelerationRoom)
                    break;
                geometryRoom -= cost.CardGeometryBytes;
                accelerationRoom -= cost.CardAccelerationBytes;
                tiers[i] = ReflectionTier::Card;
            }
            for (sizet i = 0u; i < count && tiers[i] == ReflectionTier::Card; ++i)
            {
                const ReflectionGroupCost& cost = nearestFirst[i];
                if (cost.MeshGeometryBytes == 0u && cost.MeshAccelerationBytes == 0u)
                    continue; // a card-only group is not a misfit
                const u64 extraGeometry = cost.MeshGeometryBytes > cost.CardGeometryBytes ? cost.MeshGeometryBytes - cost.CardGeometryBytes : 0u;
                const u64 extraAcceleration = cost.MeshAccelerationBytes > cost.CardAccelerationBytes ? cost.MeshAccelerationBytes - cost.CardAccelerationBytes : 0u;
                if (extraGeometry > geometryRoom || extraAcceleration > accelerationRoom)
                    break;
                geometryRoom -= extraGeometry;
                accelerationRoom -= extraAcceleration;
                tiers[i] = ReflectionTier::Mesh;
            }
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

    // THE BUILD DEBT (#1533): the vegetation acceleration-structure builds the
    // ray-traced scene requested last frame and the backend did not record,
    // its own VegetationFrameBudget spent. The scene requests them again this
    // frame, so the producer charges them to this frame's budget before it
    // refreshes anything. A refresh is a build request too: a producer that
    // spends the whole budget on refreshes leaves a backlog the backend never
    // drains, and the TLAS waits on that backlog for good. The showcase lawn
    // did exactly that: 37 builds requested a frame, 13 recorded, forever.
    struct VegetationBuildDebt
    {
        u32 Builds = 0u;
        u64 Vertices = 0u;
        u64 Triangles = 0u;
    };

    // Reservation is transactional: an oversized request consumes no budget,
    // so one unavailable group cannot starve the remaining work. Subtraction
    // guards addition and multiplication stays in the caller's checked u64.
    struct VegetationFrameBudget
    {
        u32 Updates = 0u;
        u32 Vertices = 0u;
        u32 Triangles = 0u;

        // `updates` is the number of acceleration-structure builds the work
        // will request: one per part, each counting the part's whole vertex
        // stream, which is how the backend charges them.
        [[nodiscard]] bool Reserve(u64 vertices, u64 triangles, u32 updates = 1u)
        {
            if (updates > VegetationPolicy::UpdatesPerFrame - Updates ||
                vertices > VegetationPolicy::VerticesPerFrame - Vertices ||
                triangles > VegetationPolicy::TrianglesPerFrame - Triangles)
                return false;
            Updates += updates;
            Vertices += static_cast<u32>(vertices);
            Triangles += static_cast<u32>(triangles);
            return true;
        }

        // Spends a debt up front, saturating: a debt larger than the frame
        // leaves nothing to reserve, and the backend works it off over frames.
        void Charge(const VegetationBuildDebt& debt)
        {
            Updates = static_cast<u32>(std::min<u64>(VegetationPolicy::UpdatesPerFrame, static_cast<u64>(Updates) + debt.Builds));
            Vertices = static_cast<u32>(std::min<u64>(VegetationPolicy::VerticesPerFrame, static_cast<u64>(Vertices) + std::min<u64>(debt.Vertices, VegetationPolicy::VerticesPerFrame)));
            Triangles = static_cast<u32>(std::min<u64>(VegetationPolicy::TrianglesPerFrame, static_cast<u64>(Triangles) + std::min<u64>(debt.Triangles, VegetationPolicy::TrianglesPerFrame)));
        }
    };
} // namespace OloEngine::RayTracing
