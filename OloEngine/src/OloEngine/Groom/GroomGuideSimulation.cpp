#include "OloEnginePCH.h"

#include "OloEngine/Groom/GroomGuideSimulation.h"

#include "OloEngine/Math/Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // A segment shorter than this has no direction, so a projection onto it
        // would normalise a zero vector. Squared, because every caller has the
        // squared length in hand already.
        constexpr f32 kSegmentEpsilon2 = 1.0e-12f;

        [[nodiscard]] bool ParamsAreFinite(const GroomSimulationParams& p) noexcept
        {
            return Math::IsFinite(p.Gravity) && Math::IsFinite(p.Stiffness) && Math::IsFinite(p.Damping) &&
                   Math::IsFinite(p.VelocityCorrection) && Math::IsFinite(p.FixedHz) &&
                   Math::IsFinite(p.StretchTolerance) && Math::IsFinite(p.ColliderPadding) &&
                   Math::IsFinite(p.ColliderFriction);
        }

        // Clamp into the documented bounds. Separate from the finiteness test
        // above on purpose: a NaN stiffness REFUSES the solve (clamping it would
        // pick a side of a comparison that has no sides), while an out-of-range
        // but finite one is an authoring mistake with an obvious nearest legal
        // answer.
        [[nodiscard]] GroomSimulationParams Sanitize(const GroomSimulationParams& in) noexcept
        {
            using namespace GroomSimulationLimits;
            GroomSimulationParams p = in;
            p.Stiffness = std::clamp(p.Stiffness, MinStiffness, MaxStiffness);
            p.Damping = std::clamp(p.Damping, MinDamping, MaxDamping);
            p.VelocityCorrection = std::clamp(p.VelocityCorrection, MinVelocityCorrection, MaxVelocityCorrection);
            p.FixedHz = std::clamp(p.FixedHz, MinFixedHz, MaxFixedHz);
            p.StretchTolerance = std::clamp(p.StretchTolerance, MinStretchTolerance, MaxStretchTolerance);
            p.ColliderPadding = std::clamp(p.ColliderPadding, 0.0f, MaxPadding);
            p.ColliderFriction = std::clamp(p.ColliderFriction, MinFriction, MaxFriction);
            p.MaxSubsteps = std::clamp(p.MaxSubsteps, MinSubsteps, MaxSubsteps);
            p.Iterations = std::clamp(p.Iterations, MinIterations, MaxIterations);
            if (!IsValidGroomSolverModel(static_cast<i32>(p.Model)))
            {
                // REJECT to the default, never saturate: the model is a
                // discriminated index, so clamping a corrupt 7 to 2 silently
                // selects a DIFFERENT valid solver. ComponentReflection.h names
                // exactly this case.
                p.Model = GroomSolverModel::DynamicFollowTheLeader;
            }
            return p;
        }

        // The offset table is a prefix sum with GuideCount + 1 entries, [0] == 0,
        // non-decreasing, ending at the point count. Validated rather than
        // trusted because it addresses every subsequent read.
        [[nodiscard]] bool OffsetsAreWellFormed(std::span<const u32> offsets, sizet pointCount) noexcept
        {
            if (offsets.empty() || offsets.front() != 0u || offsets.back() != pointCount)
            {
                return false;
            }
            for (sizet i = 1; i < offsets.size(); ++i)
            {
                if (offsets[i] < offsets[i - 1])
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    glm::vec3 ClosestPointOnGroomCollider(const GroomCollider& collider, const glm::vec3& point) noexcept
    {
        const glm::vec3 axis = collider.PointB - collider.PointA;
        const f32 axisLength2 = glm::length2(axis);
        if (axisLength2 <= kSegmentEpsilon2)
        {
            // A sphere: the degenerate capsule, taken through the same routine
            // so the cap and the sphere can never disagree about where the
            // surface is.
            return collider.PointA;
        }
        const f32 t = std::clamp(glm::dot(point - collider.PointA, axis) / axisLength2, 0.0f, 1.0f);
        return collider.PointA + axis * t;
    }

    GroomSimulationStats StepGroomGuideSimulation(const GroomSimulationInputs& inputs,
                                                  GroomGuideSimulationState& state)
    {
        OLO_PROFILE_FUNCTION();

        GroomSimulationStats stats;

        // ── Refusals, in most-fundamental-first order ───────────────────────
        //
        // Every one of them CLEARS the state. The alternative — keeping the
        // buffers and returning — leaves the caller interpolating the rendered
        // coat from particles that belong to a different guide set, which is a
        // plausible wrong coat rather than a visibly still one.
        const sizet pointCount = inputs.TargetPoints.size();
        const bool wellFormed = OffsetsAreWellFormed(inputs.GuideOffsets, pointCount) &&
                                inputs.GuideCurves.size() + 1u == inputs.GuideOffsets.size();
        if (!wellFormed || !ParamsAreFinite(inputs.Params) || !Math::IsFinite(inputs.DeltaTime) ||
            inputs.DeltaTime < 0.0f)
        {
            state.Clear();
            stats.Refused = true;
            return stats;
        }

        const u32 guideCount = static_cast<u32>(inputs.GuideOffsets.size() - 1u);
        if (guideCount > GroomSimulationLimits::MaxGuides)
        {
            state.Clear();
            stats.Refused = true;
            return stats;
        }
        if (guideCount == 0u || pointCount == 0u)
        {
            // Not a refusal: a groom whose guide budget is zero, or one with no
            // guides at all, is deliberately not simulated. The caller draws the
            // groomed rest coat, which is what it drew before this feature.
            state.Clear();
            return stats;
        }

        const GroomSimulationParams params = Sanitize(inputs.Params);
        stats.GuidesSimulated = guideCount;
        stats.PointsSimulated = static_cast<u32>(pointCount);

        // ── Re-seed, or continue ────────────────────────────────────────────
        //
        // The guide SET is part of the continuity question, not only the
        // caller's HasHistory: a budget change hands this function a different
        // number of particles in a different order, and carrying the old ones
        // across would attach a guide's momentum to a different strand.
        const bool layoutChanged = !state.Initialized || state.Curr.size() != pointCount ||
                                   state.Prev.size() != pointCount ||
                                   !std::ranges::equal(state.GuideOffsets, inputs.GuideOffsets) ||
                                   !std::ranges::equal(state.GuideCurves, inputs.GuideCurves);
        // A non-finite particle is an integration that has already failed, and
        // one of them poisons every strand that interpolates from its guide. Re-
        // seeding is the only recovery that leaves a coat somebody can look at.
        const bool statePoisoned =
            !layoutChanged && std::ranges::any_of(state.Curr,
                                                  [](const glm::vec3& p)
                                                  {
                                                      return !Math::IsFinite(p) ||
                                                             glm::any(glm::greaterThan(
                                                                 glm::abs(p),
                                                                 glm::vec3(GroomSimulationLimits::MaxCoordinate)));
                                                  });

        if (layoutChanged || statePoisoned || !inputs.HasHistory)
        {
            state.GuideOffsets.assign(inputs.GuideOffsets.begin(), inputs.GuideOffsets.end());
            state.GuideCurves.assign(inputs.GuideCurves.begin(), inputs.GuideCurves.end());
            state.Curr.assign(inputs.TargetPoints.begin(), inputs.TargetPoints.end());
            state.Prev = state.Curr;
            state.Accumulator = 0.0f;
            state.Initialized = true;
            stats.Reseeded = true;
            stats.Accumulator = 0.0f;
            // RETURNS. The frame that re-seeds draws the groomed coat and emits
            // zero motion; integrating on it would apply a whole frame of
            // gravity to a coat that was just teleported, which is the one-frame
            // sag a reset exists to prevent.
            return stats;
        }

        // ── The fixed step, with a bounded catch-up ─────────────────────────
        const f32 step = 1.0f / params.FixedHz;
        state.Accumulator += inputs.DeltaTime;
        if (const f32 maxArrears = static_cast<f32>(params.MaxSubsteps) * step; state.Accumulator > maxArrears)
        {
            // DROPPED and COUNTED. Integrating the arrears would run a
            // four-second alt-tab as 240 steps of a coat that is not on screen.
            state.Accumulator = maxArrears;
            stats.StepsClamped = true;
        }

        const f32 dt = step;
        const f32 dt2 = dt * dt;
        // The per-step velocity retention, the same expression SpringBoneSolver
        // uses: a rate in 1/s turned into a fraction by the step it is applied
        // over, so the same damping means the same settling time at any Hz.
        const f32 velocityRetain = std::clamp(1.0f - params.Damping * dt, 0.0f, 1.0f);
        const bool collide = params.CollisionEnabled && !inputs.Colliders.empty() &&
                             inputs.Colliders.size() <= GroomSimulationLimits::MaxColliders;

        // Re-derived from the TARGET every step rather than cached at bind time,
        // so a length multiplier moved in the inspector is converged on rather
        // than fought. Scratch, sized once per call.
        std::vector<f32> restLengths(pointCount, 0.0f);
        for (u32 g = 0; g < guideCount; ++g)
        {
            const u32 first = inputs.GuideOffsets[g];
            const u32 last = inputs.GuideOffsets[g + 1u];
            for (u32 i = first + 1u; i < last; ++i)
            {
                restLengths[i] = glm::length(inputs.TargetPoints[i] - inputs.TargetPoints[i - 1u]);
            }
        }

        // The correction each particle's projection removed from the particle
        // BEFORE it, which is the quantity DFTL hands back as velocity. One
        // buffer for the whole call; the FTL pass is per strand and reads only
        // its own range.
        std::vector<glm::vec3> corrections(pointCount, glm::vec3(0.0f));

        u32 steps = 0;
        while (state.Accumulator >= step && steps < params.MaxSubsteps)
        {
            state.Accumulator -= step;
            ++steps;
            stats.ContactsResolved = 0; // the LAST step's count is the reported one

            for (u32 g = 0; g < guideCount; ++g)
            {
                const u32 first = inputs.GuideOffsets[g];
                const u32 last = inputs.GuideOffsets[g + 1u];
                if (last <= first)
                {
                    continue;
                }

                // The ROOT is kinematic: it is where the body put it, exactly,
                // with no integration at all. A simulated root is a coat that
                // detaches from the animal, which is the one failure this
                // feature must never produce.
                state.Prev[first] = state.Curr[first];
                state.Curr[first] = inputs.TargetPoints[first];
                corrections[first] = glm::vec3(0.0f);

                // ── 1. Predict ──────────────────────────────────────
                for (u32 i = first + 1u; i < last; ++i)
                {
                    const glm::vec3 curr = state.Curr[i];
                    const glm::vec3 velocity = (curr - state.Prev[i]) * velocityRetain;
                    // The shape term is what makes this fur rather than hair:
                    // the strand is pulled back toward the position the GROOM
                    // put it in, so a coat holds its authored curl instead of
                    // hanging off the body like a wet rope.
                    const glm::vec3 accel = params.Stiffness * (inputs.TargetPoints[i] - curr) + params.Gravity;
                    state.Prev[i] = curr;
                    state.Curr[i] = curr + velocity + accel * dt2;
                }

                // ── 2. Collide, BEFORE the length pass ──────────────
                //
                // That order is deliberate and it is the trade this solver
                // makes: resolving penetration first and projecting length
                // second means the length guarantee is exact and a particle may
                // end the step a fraction of a segment inside the proxy.
                // Colliding last would invert it — exact non-penetration and a
                // visibly stretched strand — and a coat that stretches is the
                // failure criterion 1 names, while a strand whose tip grazes a
                // capsule by a tenth of a millimetre is not visible at all. The
                // shell (ColliderPadding) is what buys back the difference.
                if (collide)
                {
                    for (u32 i = first + 1u; i < last; ++i)
                    {
                        for (const GroomCollider& collider : inputs.Colliders)
                        {
                            const f32 radius = collider.Radius + params.ColliderPadding;
                            if (!(radius > 0.0f))
                            {
                                continue;
                            }
                            const glm::vec3 nearest = ClosestPointOnGroomCollider(collider, state.Curr[i]);
                            const glm::vec3 offset = state.Curr[i] - nearest;
                            const f32 distance2 = glm::length2(offset);
                            if (distance2 >= radius * radius)
                            {
                                continue;
                            }
                            // A particle exactly on the axis has no direction to
                            // be pushed along. Using the strand's own rest
                            // direction is the only answer that is not a
                            // coin-flip: it puts the hair back where the groom
                            // wanted it rather than on an arbitrary side of the
                            // limb.
                            glm::vec3 normal;
                            if (distance2 > kSegmentEpsilon2)
                            {
                                normal = offset * glm::inversesqrt(distance2);
                            }
                            else
                            {
                                const glm::vec3 fallback = inputs.TargetPoints[i] - nearest;
                                const f32 fallback2 = glm::length2(fallback);
                                normal = fallback2 > kSegmentEpsilon2 ? fallback * glm::inversesqrt(fallback2)
                                                                      : glm::vec3(0.0f, 1.0f, 0.0f);
                            }

                            state.Curr[i] = nearest + normal * radius;
                            // FRICTION IS APPLIED TO THE PREVIOUS POSITION, not
                            // to a velocity field, because this integrator's
                            // velocity IS curr - prev. Moving `prev` toward the
                            // contact point along the normal kills the inward
                            // component and keeps the tangential one, scaled by
                            // the friction coefficient — one expression for both
                            // halves, so they cannot disagree about which is
                            // which.
                            const glm::vec3 relative = state.Prev[i] - state.Curr[i];
                            const glm::vec3 alongNormal = normal * glm::dot(relative, normal);
                            const glm::vec3 tangential = relative - alongNormal;
                            state.Prev[i] = state.Curr[i] + tangential * params.ColliderFriction;
                            ++stats.ContactsResolved;
                        }
                    }
                }

                // ── 3. Enforce length ───────────────────────────────
                switch (params.Model)
                {
                    case GroomSolverModel::FollowTheLeader:
                    case GroomSolverModel::DynamicFollowTheLeader:
                    {
                        // ONE root-to-tip pass. Each particle is placed at its
                        // exact rest distance from its already-final parent, so
                        // the pass is a projection and not an iteration: there
                        // is no residual to converge, at any step size. That is
                        // the whole argument for this family.
                        for (u32 i = first + 1u; i < last; ++i)
                        {
                            const glm::vec3 parent = state.Curr[i - 1u];
                            const glm::vec3 delta = state.Curr[i] - parent;
                            const f32 length2 = glm::length2(delta);
                            glm::vec3 direction;
                            if (length2 > kSegmentEpsilon2)
                            {
                                direction = delta * glm::inversesqrt(length2);
                            }
                            else
                            {
                                // Coincident with the parent: fall back to the
                                // groomed direction, which is the only one that
                                // carries information about where this strand is
                                // supposed to point.
                                const glm::vec3 restDelta = inputs.TargetPoints[i] - inputs.TargetPoints[i - 1u];
                                const f32 restLength2 = glm::length2(restDelta);
                                direction = restLength2 > kSegmentEpsilon2
                                                ? restDelta * glm::inversesqrt(restLength2)
                                                : glm::vec3(0.0f, 1.0f, 0.0f);
                            }
                            const glm::vec3 placed = parent + direction * restLengths[i];
                            corrections[i] = state.Curr[i] - placed;
                            state.Curr[i] = placed;
                        }

                        if (params.Model == GroomSolverModel::DynamicFollowTheLeader &&
                            params.VelocityCorrection > 0.0f)
                        {
                            // Müller's correction. The projection above moved
                            // particle i+1 by `corrections[i+1]`, and that
                            // displacement is momentum the pass removed from
                            // particle i. Handing it back through `Prev` —
                            // which is where this integrator keeps velocity —
                            // is what separates DFTL from FTL's syrup.
                            for (u32 i = first + 1u; i + 1u < last; ++i)
                            {
                                state.Prev[i] -= corrections[i + 1u] * params.VelocityCorrection;
                            }
                        }
                        break;
                    }
                    case GroomSolverModel::PositionBasedDistance:
                    {
                        // The reference the other two are measured against.
                        // Gauss-Seidel over the distance constraints, root
                        // pinned: the residual is a function of `Iterations`
                        // AND of how far one step displaced the strand, which
                        // is exactly why it is not the default.
                        for (u32 iteration = 0; iteration < params.Iterations; ++iteration)
                        {
                            for (u32 i = first + 1u; i < last; ++i)
                            {
                                const glm::vec3 delta = state.Curr[i] - state.Curr[i - 1u];
                                const f32 length2 = glm::length2(delta);
                                if (length2 <= kSegmentEpsilon2)
                                {
                                    continue;
                                }
                                const f32 length = std::sqrt(length2);
                                const f32 error = length - restLengths[i];
                                const glm::vec3 direction = delta / length;
                                // The parent of the FIRST simulated particle is
                                // the kinematic root and takes no share; every
                                // other pair splits the error evenly, which is
                                // the equal-mass case this solver assumes
                                // throughout.
                                if (i == first + 1u)
                                {
                                    state.Curr[i] -= direction * error;
                                }
                                else
                                {
                                    state.Curr[i] -= direction * (error * 0.5f);
                                    state.Curr[i - 1u] += direction * (error * 0.5f);
                                }
                            }
                        }
                        break;
                    }
                    case GroomSolverModel::Count:
                        break;
                }
            }
        }

        stats.StepsTaken = steps;
        stats.Accumulator = state.Accumulator;

        // ── The contract, measured ──────────────────────────────────────────
        //
        // Reported rather than asserted, because this runs in a shipping frame:
        // the test is what fails on it. A ratio is used and not an absolute
        // error so the number means the same thing on a whisker and on a tail
        // plume.
        f32 maxStretch = 1.0f;
        f32 maxDeviation = 0.0f;
        for (u32 g = 0; g < guideCount; ++g)
        {
            const u32 first = inputs.GuideOffsets[g];
            const u32 last = inputs.GuideOffsets[g + 1u];
            for (u32 i = first; i < last; ++i)
            {
                maxDeviation = std::max(maxDeviation, glm::length(state.Curr[i] - inputs.TargetPoints[i]));
                if (i > first && restLengths[i] > kSegmentEpsilon2)
                {
                    const f32 ratio = glm::length(state.Curr[i] - state.Curr[i - 1u]) / restLengths[i];
                    if (std::abs(ratio - 1.0f) > std::abs(maxStretch - 1.0f))
                    {
                        maxStretch = ratio;
                    }
                }
            }
        }
        stats.MaxStretchRatio = maxStretch;
        stats.MaxRestDeviation = maxDeviation;
        return stats;
    }
} // namespace OloEngine
