#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace OloEngine::RayTracing
{
    // WHAT PUSHED VEGETATION BELOW THE QUALITY IT ASKED FOR (#1354). One value
    // per limit, so a degraded or withheld frame names its cause instead of
    // reading as an empty acceleration structure. Every source but the last
    // two is a BUDGET: the producer plans a complete lower-cost tier, a
    // bounded hold or an explicit counted fallback before it publishes. The
    // last two are not recoverable by spending less and stay fail-closed.
    enum class VegetationPressure : u8
    {
        None,
        FrameWork,          ///< the per-frame build budget (VegetationFrameBudget)
        BuildDebt,          ///< last frame's unrecorded builds took this frame's budget
        GeometryMemory,     ///< VegetationPolicy::GeometryBytes
        AccelerationMemory, ///< VegetationPolicy::AccelerationStructureBytes
        ResidentGroups,     ///< VegetationPolicy::ResidentGroups
        InvalidContent,     ///< a group the producer could not describe
        ProducerFailure,    ///< shader, allocation or dispatch failure
        Count
    };

    [[nodiscard]] constexpr const char* ToString(VegetationPressure pressure)
    {
        switch (pressure)
        {
            case VegetationPressure::None:
                return "none";
            case VegetationPressure::FrameWork:
                return "frameWork";
            case VegetationPressure::BuildDebt:
                return "buildDebt";
            case VegetationPressure::GeometryMemory:
                return "geometryMemory";
            case VegetationPressure::AccelerationMemory:
                return "accelerationMemory";
            case VegetationPressure::ResidentGroups:
                return "residentGroups";
            case VegetationPressure::InvalidContent:
                return "invalidContent";
            case VegetationPressure::ProducerFailure:
                return "producerFailure";
            case VegetationPressure::Count:
                break;
        }
        return "unknown";
    }

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
        // the full refresh used to switch the feature off, because a refused
        // caster withheld the whole TLAS; since #1354 the casting plan
        // (ChooseCastingTiers) charges the full refresh and traces what does
        // not fit at its complete card tier instead.
        static constexpr u64 GeometryBytes = 64u * 1024u * 1024u;
        static constexpr u64 AccelerationStructureBytes = 64u * 1024u * 1024u;
        static constexpr u32 UpdatesPerFrame = 1024u;
        static constexpr u32 VerticesPerFrame = 1048576u;
        static constexpr u32 TrianglesPerFrame = 524288u;
        static constexpr f32 MaximumProxyAge = 0.05f;
        static constexpr f32 MaximumWorldDisplacementError = 0.25f;
        // A REFLECTION-ONLY snapshot whose refresh did not fit may be held
        // past the proxy deadline (#1533), but not without limit (#1354): a
        // reflection ray does not have to match a raster shadow, so its bound
        // is looser, and it is still a bound. Past it the group is deferred
        // (left out of the scene for the frame and counted), never published
        // as current geometry.
        static constexpr f32 MaximumReflectionHoldAge = 1.0f;
        static constexpr f32 MaximumReflectionHoldError = 1.0f;
        // A withheld vegetation TLAS comes back only after this many
        // consecutive complete frames (#1354). Demand hovering at the budget
        // otherwise switches every ray-traced effect on and off frame to
        // frame; held off, the technique degrades once and stays degraded.
        static constexpr u32 RecoveryFrames = 8u;
        // A casting group the plan did not trace at its requested tier last
        // frame is upgraded only if this fraction of every budget stays free
        // afterwards (#1354), so a group at the margin does not flip tiers,
        // and rebuild, every frame.
        static constexpr u64 UpgradeMarginDivisor = 16u;

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
        // `groupsTaken`: the groups the casting layers queued. Each admitted
        // group is one more against ResidentGroups (#1354), which the cache
        // otherwise enforces by refusing, and a refusal withholds the TLAS.
        static void ChooseReflectionTiers(std::span<const ReflectionGroupCost> nearestFirst, u64 geometryTaken,
                                          u64 accelerationTaken, std::span<ReflectionTier> tiers, u32 groupsTaken = 0u) noexcept
        {
            std::ranges::fill(tiers, ReflectionTier::Out);
            u64 geometryRoom = geometryTaken < GeometryBytes ? GeometryBytes - geometryTaken : 0u;
            u64 accelerationRoom = accelerationTaken < AccelerationStructureBytes ? AccelerationStructureBytes - accelerationTaken : 0u;
            u32 groupRoom = groupsTaken < ResidentGroups ? ResidentGroups - groupsTaken : 0u;
            const sizet count = std::min(nearestFirst.size(), tiers.size());
            for (sizet i = 0u; i < count; ++i)
            {
                const ReflectionGroupCost& cost = nearestFirst[i];
                if (groupRoom == 0u || cost.CardGeometryBytes > geometryRoom || cost.CardAccelerationBytes > accelerationRoom)
                    break;
                --groupRoom;
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

        // THE CASTING SHARE (#1354). A layer that casts a shadow must be traced
        // whole: a missing caster leaks light through it, so a gap withholds
        // the TLAS from shadow rays (and from every other ray). Before #1354 a
        // caster that did not fit was refused, and one overflow in one layer
        // switched off every ray-traced effect. Now the producer plans first.
        //
        // Each group has its REQUESTED tier (the authored mesh inside the
        // raster's mesh distance) and, where the layer has one, a complete
        // lower-cost FALLBACK (its card: one quad per plant, the raster's own
        // far LOD). A tier costs memory (staged geometry, acceleration
        // structures, one resident group) and WORK: its full refresh in the
        // backend's units, times how often it refreshes in steady state (a
        // detailed group every frame, a proxy once per error deadline). The
        // work is the AMORTIZED demand on purpose. The IntegratedRenderer
        // meadow plans 1,681 casting groups; their full refresh is more builds
        // than a frame has, and charging it every frame withheld the TLAS
        // permanently from a scene whose staggered proxies fit easily. The
        // frame after a camera cut, when every group is new, is the cache's:
        // it builds what fits, refuses the rest (counted), and the technique
        // hysteresis brings the TLAS back once the scene has warmed up.
        //
        // Pass one admits every group at its cheapest complete tier, nearest
        // first; a group that does not fit even then is OUT (refused,
        // counted, and the casters are incomplete: the explicit technique
        // fallback). Pass two upgrades the nearest admitted groups to their
        // requested tier while the difference fits, stopping at the first
        // misfit so the full-quality tier is a disc around the camera. A group
        // that was not at its requested tier last frame needs a margin of
        // every budget left after its upgrade (UpgradeMarginDivisor), so
        // demand at the margin does not flip it, and rebuild it, every frame.
        struct GroupCost
        {
            u64 GeometryBytes = 0u;
            u64 AccelerationBytes = 0u;
            u64 RefreshVertices = 0u;
            u64 RefreshTriangles = 0u;
            u32 RefreshBuilds = 0u;
        };
        struct CastingGroupCost
        {
            GroupCost Requested;
            GroupCost Fallback;
            bool HasFallback = false;
            /// It was traced at its requested tier last frame.
            bool WasRequested = false;
            /// Refreshes per frame in steady state, [0, 1]: 1 for a group
            /// refreshed every frame, frameSeconds / ProxyAgeLimit for a proxy
            /// (RefreshRate).
            f32 RequestedRate = 1.0f;
            f32 FallbackRate = 1.0f;
        };
        enum class CastingTier : u8
        {
            Out,
            Fallback,
            Requested
        };
        struct CastingPlan
        {
            u32 Requested = 0u;
            u32 Fallbacks = 0u;
            u32 Out = 0u;
            u32 Groups = 0u;
            /// Memory and full-refresh cost of the admitted tiers.
            GroupCost Spent;
            /// Steady-state refresh demand of the admitted tiers, per frame.
            f64 DemandVertices = 0.0;
            f64 DemandTriangles = 0.0;
            f64 DemandBuilds = 0.0;
            /// What forced the first fallback or exclusion; None when every
            /// group is at its requested tier.
            VegetationPressure Pressure = VegetationPressure::None;
            /// What it would take to trace every group at its requested tier.
            GroupCost Recovery;
        };
        // A tier's charge against the plan: memory in full, work at its rate.
        struct PlanCharge
        {
            u64 Geometry = 0u;
            u64 Acceleration = 0u;
            f64 Vertices = 0.0;
            f64 Triangles = 0.0;
            f64 Builds = 0.0;
            u32 Groups = 0u;

            [[nodiscard]] static PlanCharge Of(const GroupCost& cost, f32 rate, u32 groups) noexcept
            {
                const f64 r = std::isfinite(rate) ? std::clamp(static_cast<f64>(rate), 0.0, 1.0) : 1.0;
                return { cost.GeometryBytes, cost.AccelerationBytes, static_cast<f64>(cost.RefreshVertices) * r,
                         static_cast<f64>(cost.RefreshTriangles) * r, static_cast<f64>(cost.RefreshBuilds) * r, groups };
            }
            // What `to` costs beyond `from`, never negative.
            [[nodiscard]] static PlanCharge Upgrade(const PlanCharge& to, const PlanCharge& from) noexcept
            {
                return { to.Geometry > from.Geometry ? to.Geometry - from.Geometry : 0u,
                         to.Acceleration > from.Acceleration ? to.Acceleration - from.Acceleration : 0u,
                         std::max(to.Vertices - from.Vertices, 0.0), std::max(to.Triangles - from.Triangles, 0.0),
                         std::max(to.Builds - from.Builds, 0.0), to.Groups > from.Groups ? to.Groups - from.Groups : 0u };
            }
        };
        // The room a plan spends, in every unit a limit is enforced in.
        struct PlanRoom
        {
            u64 Geometry = GeometryBytes;
            u64 Acceleration = AccelerationStructureBytes;
            f64 Vertices = static_cast<f64>(VerticesPerFrame);
            f64 Triangles = static_cast<f64>(TrianglesPerFrame);
            f64 Builds = static_cast<f64>(UpdatesPerFrame);
            u32 Groups = ResidentGroups;
            // The limits the margins are fractions of.
            u64 Divisor = 1u;

            // Every limit at 1/divisor: VegetationDiagnostics::SetBudgetDivisor.
            [[nodiscard]] static PlanRoom Scaled(u32 divisor) noexcept
            {
                PlanRoom room;
                room.Divisor = std::max<u64>(divisor, 1u);
                const f64 d = static_cast<f64>(room.Divisor);
                room.Geometry /= room.Divisor;
                room.Acceleration /= room.Divisor;
                room.Vertices /= d;
                room.Triangles /= d;
                room.Builds = std::floor(room.Builds / d);
                room.Groups = static_cast<u32>(room.Groups / room.Divisor);
                return room;
            }

            // What would stop `charge` from fitting while leaving
            // `marginDivisor`ths of each limit free; None if it fits.
            [[nodiscard]] VegetationPressure Misfit(const PlanCharge& charge, u64 marginDivisor) const noexcept
            {
                const auto keep = [marginDivisor, this](f64 limit) -> f64
                { return marginDivisor == 0u ? 0.0 : limit / static_cast<f64>(Divisor) / static_cast<f64>(marginDivisor); };
                const auto fits = [](f64 room, f64 reserve, f64 cost)
                { return room >= reserve && cost <= room - reserve; };
                if (!fits(Groups, std::floor(keep(ResidentGroups)), charge.Groups))
                    return VegetationPressure::ResidentGroups;
                if (!fits(static_cast<f64>(Geometry), keep(GeometryBytes), static_cast<f64>(charge.Geometry)))
                    return VegetationPressure::GeometryMemory;
                if (!fits(static_cast<f64>(Acceleration), keep(AccelerationStructureBytes), static_cast<f64>(charge.Acceleration)))
                    return VegetationPressure::AccelerationMemory;
                if (!fits(Vertices, keep(VerticesPerFrame), charge.Vertices) ||
                    !fits(Triangles, keep(TrianglesPerFrame), charge.Triangles) ||
                    !fits(Builds, keep(UpdatesPerFrame), charge.Builds))
                    return VegetationPressure::FrameWork;
                return VegetationPressure::None;
            }
            void Spend(const PlanCharge& charge) noexcept
            {
                Groups -= charge.Groups;
                Geometry -= charge.Geometry;
                Acceleration -= charge.Acceleration;
                Vertices -= charge.Vertices;
                Triangles -= charge.Triangles;
                Builds -= charge.Builds;
            }
        };
        [[nodiscard]] static GroupCost Difference(const GroupCost& larger, const GroupCost& smaller) noexcept
        {
            const auto minus = [](u64 a, u64 b) -> u64
            { return a > b ? a - b : 0u; };
            return { minus(larger.GeometryBytes, smaller.GeometryBytes), minus(larger.AccelerationBytes, smaller.AccelerationBytes),
                     minus(larger.RefreshVertices, smaller.RefreshVertices), minus(larger.RefreshTriangles, smaller.RefreshTriangles),
                     static_cast<u32>(minus(larger.RefreshBuilds, smaller.RefreshBuilds)) };
        }
        static void Accumulate(GroupCost& total, const GroupCost& cost) noexcept
        {
            const auto add = [](u64 a, u64 b) -> u64
            { return b > std::numeric_limits<u64>::max() - a ? std::numeric_limits<u64>::max() : a + b; };
            total.GeometryBytes = add(total.GeometryBytes, cost.GeometryBytes);
            total.AccelerationBytes = add(total.AccelerationBytes, cost.AccelerationBytes);
            total.RefreshVertices = add(total.RefreshVertices, cost.RefreshVertices);
            total.RefreshTriangles = add(total.RefreshTriangles, cost.RefreshTriangles);
            total.RefreshBuilds = static_cast<u32>(std::min<u64>(add(total.RefreshBuilds, cost.RefreshBuilds), std::numeric_limits<u32>::max()));
        }
        static CastingPlan ChooseCastingTiers(std::span<const CastingGroupCost> nearestFirst, std::span<CastingTier> tiers) noexcept
        {
            return ChooseCastingTiers(nearestFirst, tiers, PlanRoom{});
        }
        static CastingPlan ChooseCastingTiers(std::span<const CastingGroupCost> nearestFirst, std::span<CastingTier> tiers,
                                              PlanRoom room) noexcept
        {
            std::ranges::fill(tiers, CastingTier::Out);
            CastingPlan plan;
            const sizet count = std::min(nearestFirst.size(), tiers.size());
            const auto requested = [](const CastingGroupCost& group)
            { return PlanCharge::Of(group.Requested, group.RequestedRate, 1u); };
            const auto cheapest = [&requested](const CastingGroupCost& group)
            { return group.HasFallback ? PlanCharge::Of(group.Fallback, group.FallbackRate, 1u) : requested(group); };
            sizet admitted = 0u;
            for (; admitted < count; ++admitted)
            {
                const CastingGroupCost& group = nearestFirst[admitted];
                const PlanCharge charge = cheapest(group);
                if (const VegetationPressure misfit = room.Misfit(charge, 0u); misfit != VegetationPressure::None)
                {
                    plan.Pressure = misfit;
                    break;
                }
                room.Spend(charge);
                tiers[admitted] = group.HasFallback ? CastingTier::Fallback : CastingTier::Requested;
            }
            bool upgrading = true;
            for (sizet i = 0u; i < admitted; ++i)
            {
                const CastingGroupCost& group = nearestFirst[i];
                if (!group.HasFallback)
                    continue; // already at its requested tier: no misfit
                const PlanCharge extra = PlanCharge::Upgrade(requested(group), cheapest(group));
                const VegetationPressure misfit = upgrading ? room.Misfit(extra, group.WasRequested ? 0u : UpgradeMarginDivisor)
                                                            : VegetationPressure::FrameWork;
                if (misfit == VegetationPressure::None)
                {
                    room.Spend(extra);
                    tiers[i] = CastingTier::Requested;
                    continue;
                }
                if (upgrading && plan.Pressure == VegetationPressure::None)
                    plan.Pressure = misfit;
                upgrading = false;
                Accumulate(plan.Recovery, Difference(group.Requested, group.Fallback));
            }
            for (sizet i = 0u; i < count; ++i)
            {
                const CastingGroupCost& group = nearestFirst[i];
                const auto spend = [&plan](const GroupCost& cost, f32 rate)
                {
                    Accumulate(plan.Spent, cost);
                    const PlanCharge charge = PlanCharge::Of(cost, rate, 1u);
                    plan.DemandVertices += charge.Vertices;
                    plan.DemandTriangles += charge.Triangles;
                    plan.DemandBuilds += charge.Builds;
                };
                switch (tiers[i])
                {
                    case CastingTier::Requested:
                        ++plan.Requested;
                        spend(group.Requested, group.RequestedRate);
                        break;
                    case CastingTier::Fallback:
                        ++plan.Fallbacks;
                        spend(group.Fallback, group.FallbackRate);
                        break;
                    case CastingTier::Out:
                        ++plan.Out;
                        Accumulate(plan.Recovery, group.Requested);
                        break;
                }
            }
            plan.Groups = plan.Requested + plan.Fallbacks;
            return plan;
        }

        // How often a group refreshes in steady state, per frame: every frame
        // it is detailed (its wind is sampled each frame), else once per
        // proxy deadline. `frameSeconds` is the wind clock's step; a paused
        // clock refreshes nothing.
        [[nodiscard]] static f32 RefreshRate(bool detailed, f32 velocityBound, f32 frameSeconds) noexcept
        {
            if (!std::isfinite(frameSeconds) || frameSeconds <= 0.0f)
                return 0.0f;
            const f32 deadline = ProxyAgeLimit(velocityBound);
            if (detailed || !(deadline > 0.0f))
                return 1.0f;
            return std::min(frameSeconds / deadline, 1.0f);
        }

        [[nodiscard]] static bool CanReuseSnapshot(f32 currentTime, f32 snapshotTime,
                                                   f32 velocityBound, bool historyContinuous)
        {
            if (!historyContinuous || !std::isfinite(currentTime) || !std::isfinite(snapshotTime))
                return false;
            const f32 age = currentTime - snapshotTime;
            return age >= 0.0f && age < ProxyAgeLimit(velocityBound);
        }

        // Whether a snapshot whose refresh did not fit this frame may stand in
        // for the current one (#1354): reduced cadence within a DECLARED error
        // bound, never an arbitrary one. A casting group holds within the same
        // deadline a proxy is reused within, so the shadow error stays the
        // proxies' 0.25 m; a reflection-only group within the looser
        // reflection bound. Past either, the snapshot is obsolete and must not
        // be published as current geometry.
        [[nodiscard]] static bool CanHoldSnapshot(f32 currentTime, f32 snapshotTime, f32 velocityBound, bool castsShadows)
        {
            if (!std::isfinite(currentTime) || !std::isfinite(snapshotTime) || !std::isfinite(velocityBound) || velocityBound < 0.0f)
                return false;
            const f32 age = currentTime - snapshotTime;
            if (!(age >= 0.0f))
                return false;
            if (castsShadows)
                return age < ProxyAgeLimit(velocityBound);
            return age < MaximumReflectionHoldAge && age * velocityBound <= MaximumReflectionHoldError;
        }

        // THE TECHNIQUE'S HYSTERESIS (#1354). `complete` is this frame's raw
        // readiness; `cleanFrames` counts consecutive complete frames and
        // starts at RecoveryFrames (nothing has failed yet). The TLAS is
        // published only once the run reaches RecoveryFrames, so a failure
        // takes effect at once and recovery waits for a stable run.
        [[nodiscard]] static bool Recovered(bool complete, u32& cleanFrames) noexcept
        {
            cleanFrames = complete ? std::min(cleanFrames + 1u, RecoveryFrames) : 0u;
            return cleanFrames >= RecoveryFrames;
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
