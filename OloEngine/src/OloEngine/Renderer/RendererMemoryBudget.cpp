#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RendererMemoryBudget.h"

#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRPTGPU.h"

#include <algorithm>
#include <utility>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] u64 Pixels(const u32 width, const u32 height)
        {
            return static_cast<u64>(width) * height;
        }

        // bytes * to / from without overflowing on the way: a measured figure rescaled to
        // another pixel count.
        [[nodiscard]] u64 ScaleBytes(const u64 bytes, const u64 from, const u64 to)
        {
            if (from == 0)
                return 0;
            return (bytes / from) * to + ((bytes % from) * to) / from;
        }

        [[nodiscard]] u64 TexelBytes(const ImageFormat format)
        {
            return RendererMemoryFormat::BytesPerTexel(format).value_or(0u);
        }

        // The band a technique's histories are sized by: the SSGI chain runs at the
        // trace band, everything else at the scene band (RenderPipeline's acquire sites).
        [[nodiscard]] u64 TechniqueBandPixels(const MemoryTechnique technique, const MemoryQualityConfig& config)
        {
            if (technique == MemoryTechnique::SSGI && config.SSGIHalfResolution)
                return Pixels((config.SceneWidth + 1u) / 2u, (config.SceneHeight + 1u) / 2u);
            return Pixels(config.SceneWidth, config.SceneHeight);
        }

        // The band's extent. Two bands of equal area are still different storage: a rotated
        // viewport reallocates every history at the new width and height.
        [[nodiscard]] std::pair<u32, u32> TechniqueBand(const MemoryTechnique technique, const MemoryQualityConfig& config)
        {
            if (technique == MemoryTechnique::SSGI && config.SSGIHalfResolution)
                return { (config.SceneWidth + 1u) / 2u, (config.SceneHeight + 1u) / 2u };
            return { config.SceneWidth, config.SceneHeight };
        }

        [[nodiscard]] bool SameSceneBand(const MemoryQualityConfig& a, const MemoryQualityConfig& b)
        {
            return a.SceneWidth == b.SceneWidth && a.SceneHeight == b.SceneHeight;
        }

        [[nodiscard]] bool SameExtents(const MemoryQualityConfig& a, const MemoryQualityConfig& b)
        {
            return SameSceneBand(a, b) && a.DisplayWidth == b.DisplayWidth && a.DisplayHeight == b.DisplayHeight;
        }
    } // namespace

    bool ApplyMemoryLadderRung(MemoryQualityConfig& config, const MemoryLadderRung& rung)
    {
        switch (rung.Kind)
        {
            case MemoryReductionKind::TransientPoolSlack:
                if (config.TransientPoolSpare == 0u)
                    return false;
                config.TransientPoolSpare = 0u;
                return true;
            case MemoryReductionKind::HistoryResolution:
                if (!config.Has(MemoryTechnique::SSGI) || config.SSGIHalfResolution)
                    return false;
                config.SSGIHalfResolution = true;
                return true;
            case MemoryReductionKind::MsaaSamples:
                if (config.Path != RenderingPath::Deferred || config.MsaaSamples <= rung.Argument)
                    return false;
                config.MsaaSamples = rung.Argument;
                return true;
            case MemoryReductionKind::OptionalDetailPools:
                if (!config.OptionalDetail)
                    return false;
                config.OptionalDetail = false;
                return true;
            case MemoryReductionKind::TechniqueFallback:
            {
                const u32 bit = 1u << rung.Argument;
                if ((config.Techniques & bit) == 0u)
                    return false;
                config.Techniques &= ~bit;
                return true;
            }
        }
        return false;
    }

    MemoryQualityConfig ReduceMemoryQuality(const MemoryQualityConfig& requested, const u32 level, u32* appliedMask)
    {
        MemoryQualityConfig config = requested;
        u32 mask = 0;
        const u32 rungs = std::min(level, kMemoryLadderRungCount);
        for (u32 rung = 0; rung < rungs; ++rung)
        {
            if (ApplyMemoryLadderRung(config, kMemoryLadder[rung]))
                mask |= 1u << rung;
        }
        if (appliedMask)
            *appliedMask = mask;
        return config;
    }

    u64 EstimateGBufferBytes(const MemoryQualityConfig& config)
    {
        if (config.Path != RenderingPath::Deferred)
            return 0;
        return GBuffer::EstimateBytes(config.SceneWidth, config.SceneHeight, config.MsaaSamples).value_or(0u);
    }

    u64 EstimateTechniqueHistoryBytes(const MemoryTechnique technique, const MemoryQualityConfig& config)
    {
        const u64 pixels = TechniqueBandPixels(technique, config);
        const u64 half = TexelBytes(ImageFormat::RGBA16F);
        const u64 full = TexelBytes(ImageFormat::RGBA32F);
        switch (technique)
        {
            case MemoryTechnique::ReSTIRGI:
            case MemoryTechnique::ReSTIRDI:
                // Three reservoir planes plus the surface depth and geometry they are
                // validated against, RGBA32F each.
                return pixels * full * 5u;
            case MemoryTechnique::RayTracedShadows:
                // Visibility signal and its two moment planes.
                return pixels * half * 3u;
            case MemoryTechnique::SSGI:
                // Signal, surface and two moment planes, at the trace band.
                return pixels * half * 4u;
            case MemoryTechnique::SSR:
                return pixels * half;
            case MemoryTechnique::PathTracer:
                // Radiance, moments, albedo and normal sums.
                return pixels * full * 4u;
            case MemoryTechnique::ReSTIRPT:
                // Not registry histories: the pass's own four path-record pools
                // (ReSTIRPTPass::EnsureBuffers), by far the largest owner a technique has.
                return pixels * sizeof(ReSTIR::PT::PathRecord) * 4u;
            case MemoryTechnique::RayTracedReflections: // no registry history
            case MemoryTechnique::MotionBlur:
            case MemoryTechnique::DepthOfField:
            case MemoryTechnique::Bloom:
            case MemoryTechnique::COUNT:
                return 0;
        }
        return 0;
    }

    MemoryTransition EstimateViewTransition(const MemoryQualityConfig* allocated, const MemoryQualityConfig& target,
                                            const ViewMemoryInventory& inventory)
    {
        MemoryTransition transition;
        const bool measured = inventory.IsMeasured();

        // --- G-Buffer: exact from its formats ------------------------------------------
        const bool sameGBuffer = allocated && allocated->Path == target.Path && SameSceneBand(*allocated, target) &&
                                 allocated->MsaaSamples == target.MsaaSamples;
        if (!sameGBuffer)
        {
            transition.NewBytes += EstimateGBufferBytes(target);
            if (allocated)
                transition.ReleasedBytes += EstimateGBufferBytes(*allocated);
        }

        // --- Technique histories -------------------------------------------------------
        for (u32 index = 0; index < kMemoryTechniqueCount; ++index)
        {
            const auto technique = static_cast<MemoryTechnique>(index);
            const bool on = target.Has(technique);
            const bool was = allocated && allocated->Has(technique);
            const bool sameBand = was && on && TechniqueBand(technique, *allocated) == TechniqueBand(technique, target);
            if (on == was && (!on || sameBand))
                continue;

            // What it holds now is measured when the view has drawn: the registry knows
            // whether the technique really runs on this backend and path.
            const u64 held = !was ? 0u : (measured ? inventory.TechniqueHistoryBytes[index] : EstimateTechniqueHistoryBytes(technique, *allocated));
            transition.ReleasedBytes += held;
            if (!on)
                continue;
            if (was && measured)
            {
                // The same technique at another band: its measured bytes, rescaled.
                transition.NewBytes += ScaleBytes(held, TechniqueBandPixels(technique, *allocated), TechniqueBandPixels(technique, target));
            }
            else
            {
                transition.NewBytes += EstimateTechniqueHistoryBytes(technique, target);
            }
        }

        // --- Measured, pixel-proportional owners: pass targets, transients, other histories ---
        if (allocated && SameExtents(*allocated, target))
        {
            if (target.TransientPoolSpare < allocated->TransientPoolSpare)
                transition.ReleasedBytes += inventory.TransientSlackBytes;
            return transition;
        }

        if (!measured)
        {
            // Nothing drawn yet: these owners' size at `target` is unknown until a frame
            // measures them. Said, not guessed.
            transition.Complete = false;
            return transition;
        }

        // Pass targets and transients live partly at the display size (the post chain)
        // and partly at the scene band (the lit scene, the AO chains). They are measured
        // as one figure, so a change is priced by whichever of the two grows more: an
        // upscale preset switched off at an unchanged display is not free.
        const u64 scaled = inventory.NodeTargetBytes + inventory.TransientDemandBytes + inventory.OtherHistoryBytes;
        const u64 byDisplay = ScaleBytes(scaled, Pixels(inventory.DisplayWidth, inventory.DisplayHeight),
                                         Pixels(target.DisplayWidth, target.DisplayHeight));
        const bool sceneMeasured = inventory.SceneWidth != 0u && inventory.SceneHeight != 0u;
        const u64 byScene = sceneMeasured ? ScaleBytes(scaled, Pixels(inventory.SceneWidth, inventory.SceneHeight),
                                                       Pixels(target.SceneWidth, target.SceneHeight))
                                          : byDisplay;
        transition.NewBytes += std::max(byDisplay, byScene);
        if (allocated)
            transition.ReleasedBytes += scaled + inventory.TransientSlackBytes;
        return transition;
    }

    // -------------------------------------------------------------------------------------
    // RendererMemoryGovernor
    // -------------------------------------------------------------------------------------

    void RendererMemoryGovernor::Reset()
    {
        const MemoryAdmissionPolicy policy = m_Policy;
        *this = RendererMemoryGovernor{};
        m_Policy = policy;
    }

    const MemoryQualityConfig* RendererMemoryGovernor::FindRealized(const u64 view) const
    {
        for (const auto& decision : m_Snapshot.Views)
        {
            if (decision.View == view)
                return &decision.Realized;
        }
        return nullptr;
    }

    const MemoryAdmissionSnapshot& RendererMemoryGovernor::Tick(const MemoryAdmissionInput& input)
    {
        return Evaluate(input, /*frameTick=*/true);
    }

    const MemoryAdmissionSnapshot& RendererMemoryGovernor::Readmit(const MemoryAdmissionInput& input)
    {
        return Evaluate(input, /*frameTick=*/false);
    }

    bool RendererMemoryGovernor::RequestChanged(const MemoryAdmissionInput& input) const
    {
        if (input.Views.Num() != m_AdmittedRequest.Num())
            return true;
        for (i32 index = 0; index < input.Views.Num(); ++index)
        {
            if (input.Views[index].View != m_AdmittedRequest[index].View ||
                !(input.Views[index].Requested == m_AdmittedRequest[index].Requested))
                return true;
        }
        return false;
    }

    bool RendererMemoryGovernor::SameRealization(const MemoryAdmissionInput& input, const u32 a, const u32 b) const
    {
        for (const auto& view : input.Views)
        {
            if (!(ReduceMemoryQuality(view.Requested, a) == ReduceMemoryQuality(view.Requested, b)))
                return false;
        }
        return true;
    }

    u32 RendererMemoryGovernor::Canonical(const MemoryAdmissionInput& input, u32 level) const
    {
        level = std::min(level, kMemoryLadderRungCount);
        while (level > 0u && SameRealization(input, level, level - 1u))
            --level;
        return level;
    }

    RendererMemoryGovernor::LevelEstimate RendererMemoryGovernor::EstimateLevel(const MemoryAdmissionInput& input,
                                                                                const u32 level) const
    {
        // A view that has not drawn yet has no measurement of its own. Another view on the
        // same path runs the same passes at another size, so its pixel-proportional owners
        // are priced from that one. A view on another path has other passes: borrowing from
        // it would report a complete estimate that is not, so the estimate stays incomplete.
        const auto donorFor = [&input](const RenderingPath path) -> const ViewMemoryInventory*
        {
            for (const auto& view : input.Views)
            {
                if (view.Inventory.IsMeasured() && view.Allocated && view.Allocated->Path == path)
                    return &view.Inventory;
            }
            return nullptr;
        };

        MemoryTransition total;
        bool optionalAllocated = false;
        bool optionalTarget = false;
        bool anyAllocated = false;
        for (const auto& view : input.Views)
        {
            const MemoryQualityConfig target = ReduceMemoryQuality(view.Requested, level);
            ViewMemoryInventory borrowed;
            const ViewMemoryInventory* inventory = &view.Inventory;
            const ViewMemoryInventory* measuredView = view.Inventory.IsMeasured() ? nullptr : donorFor(target.Path);
            if (measuredView)
            {
                borrowed.DisplayWidth = measuredView->DisplayWidth;
                borrowed.DisplayHeight = measuredView->DisplayHeight;
                borrowed.SceneWidth = measuredView->SceneWidth;
                borrowed.SceneHeight = measuredView->SceneHeight;
                borrowed.NodeTargetBytes = measuredView->NodeTargetBytes;
                borrowed.TransientDemandBytes = measuredView->TransientDemandBytes;
                borrowed.OtherHistoryBytes = measuredView->OtherHistoryBytes;
                inventory = &borrowed;
            }
            total += EstimateViewTransition(view.Allocated ? &*view.Allocated : nullptr, target, *inventory);
            optionalTarget = optionalTarget || target.OptionalDetail;
            if (view.Allocated)
            {
                anyAllocated = true;
                optionalAllocated = optionalAllocated || view.Allocated->OptionalDetail;
            }
        }

        // Optional detail is the world's, not a view's: charged once.
        if (anyAllocated && optionalAllocated && !optionalTarget)
            total.ReleasedBytes += input.OptionalDetailBytes;
        else if (anyAllocated && !optionalAllocated && optionalTarget)
            total.NewBytes += input.KnownOptionalDetailBytes;

        LevelEstimate estimate;
        const u64 afterNew = input.Budget.LiveBytes() + total.NewBytes;
        estimate.NewBytes = total.NewBytes;
        estimate.SteadyBytes = afterNew >= total.ReleasedBytes ? afterNew - total.ReleasedBytes : 0;
        estimate.PeakBytes = input.Budget.ResidentBytes + total.NewBytes;
        estimate.Complete = total.Complete;
        // A replacement waiting for room is charged to every level's steady footprint: the
        // level must leave room for it. Not to the peak: it is not allocated with this
        // transition, but later, once it fits beside what is resident then.
        if (input.PendingNewBytes > input.PendingReleasedBytes)
            estimate.SteadyBytes += input.PendingNewBytes - input.PendingReleasedBytes;
        return estimate;
    }

    u64 RendererMemoryGovernor::LearnedClimbBytes(const u32 level) const
    {
        u64 bytes = 0;
        for (u32 rung = level; rung < std::min(m_Level, kMemoryLadderRungCount); ++rung)
            bytes += m_LearnedRungBytes[rung];
        return bytes;
    }

    void RendererMemoryGovernor::ForgetLearning()
    {
        m_LearnedRungBytes.fill(0u);
        m_RecoveryBackoffShift = 0;
        m_DescentUnmeasured = false;
    }

    void RendererMemoryGovernor::AdmitFresh(const MemoryAdmissionInput& input, const u64 limit, const u64 budget,
                                            const bool keepLearned)
    {
        if (!keepLearned)
            ForgetLearning();
        const u32 floor = Canonical(input, kMemoryLadderRungCount);
        const bool requestedFitsSteady = EstimateLevel(input, 0u).SteadyBytes <= limit;

        // A changed request under an unchanged budget (a resize, a settings edit, a new
        // view) is admitted afresh, but levels above the one in force still pay what the
        // rungs between were measured to cost: otherwise a technique whose memory the
        // model cannot see looks free on every readmission and quality flips back up.
        u32 chosen = floor;
        bool found = false;
        for (u32 level = 0; level <= kMemoryLadderRungCount; ++level)
        {
            if (level > 0u && SameRealization(input, level, level - 1u))
                continue;
            const LevelEstimate estimate = EstimateLevel(input, level);
            const u64 learned = LearnedClimbBytes(level);
            if (estimate.SteadyBytes + learned <= limit && estimate.PeakBytes + learned <= budget)
            {
                chosen = level;
                found = true;
                break;
            }
        }

        // A readmission under an unchanged budget may go down, never up. A resize or a
        // settings edit arrives as a burst of readmissions (an editor panel passes through
        // several sizes), and the measured inventory cannot see the transients of the
        // techniques the current level has off, so a fresh prediction above the current
        // level is optimistic: taking it thrashed 16 -> 0 -> 16 -> 4 live. Climbing is the
        // recovery path's job: a stable run, charged what the rungs were measured to cost.
        if (keepLearned)
            chosen = std::max(chosen, std::min(m_Level, floor));

        m_Level = chosen;
        // The requested configuration would fit once the old resources retire: it is only
        // the overlap that does not.
        m_ReducedCause = requestedFitsSteady ? MemoryAdmissionReason::TransitionOverlap : MemoryAdmissionReason::DeviceBudget;
        // No level fits both its steady footprint and its replacement peak: the floor is
        // kept and reported as over budget, even when the floor is level 0 (a resize with
        // nothing left to reduce whose overlap does not fit).
        m_OverAtFloor = !found;
        m_SettleTicks = m_Policy.SettleFrames;
        m_RecoveryProgress = 0;
        m_RecoveryTarget = 0;
        m_ClimbOutstanding = false;
        m_DescentUnmeasured = false;
        m_Snapshot.PeakResidentBytes = 0;
    }

    u32 RendererMemoryGovernor::ChooseDescent(const MemoryAdmissionInput& input, const u64 limit, const u64 budget,
                                              const u64 reserve) const
    {
        // The quality-optimal target: the first level predicted to fit. When none is,
        // the level that takes every saving the model knows of; when the model knows of
        // none (a rung whose saving is only measurable), the next level, and the
        // governor measures again after it settles.
        u32 next = m_Level;
        u32 target = m_Level;
        u32 leastLevel = m_Level;
        u64 leastSteady = EstimateLevel(input, m_Level).SteadyBytes;
        bool fits = false;
        for (u32 level = m_Level + 1u; level <= kMemoryLadderRungCount; ++level)
        {
            if (SameRealization(input, level, level - 1u))
                continue;
            if (next == m_Level)
                next = level;
            const u64 steady = EstimateLevel(input, level).SteadyBytes;
            if (steady < leastSteady)
            {
                leastSteady = steady;
                leastLevel = level;
            }
            if (steady <= limit)
            {
                target = level;
                fits = true;
                break;
            }
        }
        if (!fits)
            return leastLevel != m_Level ? leastLevel : next;

        // A descent can itself allocate (fewer samples is a new G-Buffer), and the old
        // storage stays resident until it retires. When the target's overlap does not fit,
        // stage through the shallowest deeper level whose overlap does, or failing that
        // one whose new allocations are within a reserve of the least any level needs.
        // Recovery climbs back to the target once the old storage is gone.
        const LevelEstimate direct = EstimateLevel(input, target);
        if (direct.PeakBytes <= budget)
            return target;
        // The shallowest deeper level whose overlap fits beats any that only comes close.
        for (u32 level = target + 1u; level <= kMemoryLadderRungCount; ++level)
        {
            if (SameRealization(input, level, level - 1u))
                continue;
            const LevelEstimate estimate = EstimateLevel(input, level);
            if (estimate.PeakBytes <= budget && estimate.SteadyBytes <= limit)
                return level;
        }
        u64 leastNew = direct.NewBytes;
        for (u32 level = target + 1u; level <= kMemoryLadderRungCount; ++level)
        {
            if (!SameRealization(input, level, level - 1u))
                leastNew = std::min(leastNew, EstimateLevel(input, level).NewBytes);
        }
        for (u32 level = target; level <= kMemoryLadderRungCount; ++level)
        {
            if (level > target && SameRealization(input, level, level - 1u))
                continue;
            const LevelEstimate estimate = EstimateLevel(input, level);
            if (estimate.NewBytes <= leastNew + reserve)
                return level;
        }
        return target;
    }

    const MemoryAdmissionSnapshot& RendererMemoryGovernor::Evaluate(const MemoryAdmissionInput& input, const bool frameTick)
    {
        if (frameTick)
        {
            ++m_Snapshot.Ticks;
            ++m_TicksSinceClimb;
        }

        const MemoryBudgetObservation& observed = input.Budget;
        const bool requestChanged = !m_HasAdmitted || RequestChanged(input);

        // A new budget is new information: the backoff a failed climb earned under the old
        // one no longer applies. What each rung was MEASURED to cost does, since a
        // technique's transients weigh the same under any budget, so that is kept (a
        // climb that holds clears it). An OS-reported budget drifts by a little every
        // frame, so only a move larger than the recovery band counts as new.
        // A budget that becomes known has never judged the request in force: that is a
        // fresh admission, whatever the request did.
        const bool budgetAppeared = observed.BudgetBytes.has_value() && !m_BudgetAnchor.has_value();
        bool budgetMoved = observed.BudgetBytes.has_value() != m_BudgetAnchor.has_value();
        if (!budgetMoved && observed.BudgetBytes)
        {
            const u64 anchor = *m_BudgetAnchor;
            const u64 delta = *observed.BudgetBytes > anchor ? *observed.BudgetBytes - anchor : anchor - *observed.BudgetBytes;
            budgetMoved = delta > static_cast<u64>(static_cast<f64>(anchor) * static_cast<f64>(m_Policy.RecoveryBandFraction));
        }
        if (budgetMoved)
        {
            m_BudgetAnchor = observed.BudgetBytes;
            m_RecoveryBackoffShift = 0;
            m_DescentUnmeasured = false;
            m_RecoveryProgress = 0;
            m_ClimbOutstanding = false;
            m_Snapshot.PeakResidentBytes = 0;
        }

        const u32 previousLevel = m_Level;
        bool levelForced = false;
        const u32 recoveryRequired = m_Policy.RecoveryFrames << std::min(m_RecoveryBackoffShift, m_Policy.MaxRecoveryBackoffShift);
        u64 reserve = 0;

        if (!observed.BudgetBytes)
        {
            // Nothing to admit against. The request is active and the report says the
            // budget is unknown: an unenforced budget is not a satisfied one. Not a
            // recovery: nothing was measured to fit.
            levelForced = m_Level != 0u;
            m_Level = 0;
            m_SettleTicks = 0;
            m_RecoveryProgress = 0;
            m_OverAtFloor = false;
            m_ClimbOutstanding = false;
        }
        else
        {
            const u64 budget = *observed.BudgetBytes;
            reserve = std::min(budget, std::max(m_Policy.ReserveBytes,
                                                static_cast<u64>(static_cast<f64>(budget) * static_cast<f64>(m_Policy.ReserveFraction))));
            const u64 limit = budget - reserve;

            if (requestChanged || budgetAppeared)
            {
                if (m_HasAdmitted && requestChanged)
                    ++m_Snapshot.Readmissions;
                AdmitFresh(input, limit, budget, /*keepLearned=*/m_HasAdmitted && !budgetMoved);
            }
            else if (!frameTick)
            {
                // A re-admission of an unchanged request has nothing to decide.
            }
            else if (m_SettleTicks > 0u)
            {
                --m_SettleTicks;
            }
            else
            {
                // A replacement waiting for room is pressure now: the room it needs has
                // to be made before it can be allocated.
                const u64 pendingGrowth = input.PendingNewBytes > input.PendingReleasedBytes
                                              ? input.PendingNewBytes - input.PendingReleasedBytes
                                              : 0u;
                const u64 live = observed.LiveBytes() + pendingGrowth;
                const u32 floor = Canonical(input, kMemoryLadderRungCount);
                const u64 climbWindow = static_cast<u64>(recoveryRequired) + m_Policy.SettleFrames;
                if (m_DescentUnmeasured)
                {
                    // The last descent has settled. What it gave back beyond the model's
                    // prediction is what climbing past it costs beyond the model's too.
                    m_DescentUnmeasured = false;
                    // Like with like: the prediction without the pending charge it carried,
                    // against the live bytes without what replacements added since. A reload
                    // landing (or queueing) during the settle run then neither inflates nor
                    // hides what the rung gave back.
                    const u64 predicted = m_DescentPredictedSteady > m_DescentPendingBytes ? m_DescentPredictedSteady - m_DescentPendingBytes : 0u;
                    const u64 landed = input.LandedReplacementBytes - std::min(input.LandedReplacementBytes, m_DescentLandedBytes);
                    const u64 observedLive = observed.LiveBytes();
                    const u64 measured = observedLive > landed ? observedLive - landed : 0u;
                    if (predicted > measured && m_DescentLevel > 0u && m_DescentLevel == m_Level)
                        m_LearnedRungBytes[m_DescentLevel - 1u] += predicted - measured;
                }
                if (live > limit)
                {
                    m_RecoveryProgress = 0;
                    if (m_ClimbOutstanding && m_TicksSinceClimb <= climbWindow)
                    {
                        // The last climb cost more than its estimate. Go back to the level
                        // it left, wait twice as long next time, and charge every later
                        // climb the difference.
                        ++m_Snapshot.FailedRecoveries;
                        m_RecoveryBackoffShift = std::min(m_RecoveryBackoffShift + 1u, m_Policy.MaxRecoveryBackoffShift);
                        // Charged to the deepest rung the climb re-enabled, so any later
                        // climb past it pays, and a shallower one does not.
                        if (live > m_ClimbPredictedSteady && m_LevelBeforeClimb > 0u)
                        {
                            u64& learned = m_LearnedRungBytes[std::min(m_LevelBeforeClimb, kMemoryLadderRungCount) - 1u];
                            learned = std::max(learned, live - m_ClimbPredictedSteady);
                        }
                        m_ClimbOutstanding = false;
                        m_Level = std::max(m_Level, std::min(m_LevelBeforeClimb, floor));
                        m_DescentUnmeasured = false; // what this undoes was measured by the climb
                        m_ReducedCause = MemoryAdmissionReason::DeviceBudget;
                        m_SettleTicks = m_Policy.SettleFrames;
                        m_OverAtFloor = false;
                    }
                    else if (m_Level < floor)
                    {
                        m_ClimbOutstanding = false;
                        m_Level = ChooseDescent(input, limit, budget, reserve);
                        m_DescentUnmeasured = true;
                        m_DescentLevel = m_Level;
                        m_DescentPendingBytes = pendingGrowth;
                        m_DescentLandedBytes = input.LandedReplacementBytes;
                        m_DescentPredictedSteady = EstimateLevel(input, m_Level).SteadyBytes;
                        m_ReducedCause = MemoryAdmissionReason::DeviceBudget;
                        m_SettleTicks = m_Policy.SettleFrames;
                        m_OverAtFloor = false;
                    }
                    else
                    {
                        m_OverAtFloor = true;
                    }
                }
                else
                {
                    m_OverAtFloor = false;
                    if (m_ClimbOutstanding && m_TicksSinceClimb > climbWindow)
                    {
                        // The climb held for a whole run: the rungs it passed cost what
                        // the live bytes now show, and nothing more is owed for them.
                        m_ClimbOutstanding = false;
                        for (u32 rung = m_Level; rung < std::min(m_LevelBeforeClimb, kMemoryLadderRungCount); ++rung)
                            m_LearnedRungBytes[rung] = 0;
                    }
                    if (m_Level > 0u)
                    {
                        const u64 band = static_cast<u64>(static_cast<f64>(budget) * static_cast<f64>(m_Policy.RecoveryBandFraction));
                        const u64 climbLimit = limit > band ? limit - band : 0;
                        // The best level whose climb fits with the band to spare.
                        u32 candidate = m_Level;
                        u64 candidateSteady = 0;
                        for (u32 level = 0; level < m_Level; ++level)
                        {
                            if (level > 0u && SameRealization(input, level, level - 1u))
                                continue;
                            const LevelEstimate estimate = EstimateLevel(input, level);
                            const u64 learned = LearnedClimbBytes(level);
                            if (estimate.SteadyBytes + learned <= climbLimit && estimate.PeakBytes + learned <= budget)
                            {
                                candidate = level;
                                candidateSteady = estimate.SteadyBytes;
                                break;
                            }
                        }
                        if (candidate < m_Level)
                        {
                            // The run climbs to the deepest level that fitted on any of its
                            // frames: every counted frame fitted that one, since a deeper
                            // level costs no more than a shallower one.
                            m_RecoveryTarget = m_RecoveryProgress == 0u ? candidate : std::max(m_RecoveryTarget, candidate);
                            ++m_RecoveryProgress;
                            if (m_RecoveryProgress >= recoveryRequired)
                            {
                                if (m_RecoveryTarget != candidate)
                                    candidateSteady = EstimateLevel(input, m_RecoveryTarget).SteadyBytes;
                                candidate = m_RecoveryTarget;
                                m_LevelBeforeClimb = m_Level;
                                m_ClimbPredictedSteady = candidateSteady;
                                m_Level = candidate;
                                m_RecoveryProgress = 0;
                                m_SettleTicks = m_Policy.SettleFrames;
                                m_TicksSinceClimb = 0;
                                m_ClimbOutstanding = true;
                            }
                        }
                        else
                        {
                            m_RecoveryProgress = 0;
                        }
                    }
                }
            }
        }

        if (m_Level != previousLevel && !requestChanged && !budgetAppeared && !levelForced)
        {
            ++m_Snapshot.Transitions;
            if (m_Level > previousLevel)
                ++m_Snapshot.Degradations;
            else
                ++m_Snapshot.Recoveries;
        }
        if (m_Level != previousLevel || requestChanged || budgetAppeared)
            m_Snapshot.TicksAtLevel = 0;
        else if (frameTick)
            ++m_Snapshot.TicksAtLevel;

        Publish(input, reserve, recoveryRequired);
        m_HasAdmitted = true;
        return m_Snapshot;
    }

    void RendererMemoryGovernor::Publish(const MemoryAdmissionInput& input, const u64 reserve, const u32 recoveryRequired)
    {
        const MemoryBudgetObservation& observed = input.Budget;
        // Estimated against what was allocated BEFORE this decision is applied: the
        // prediction a later measurement is compared with.
        const LevelEstimate estimate = EstimateLevel(input, m_Level);

        m_Snapshot.BudgetSource = observed.BudgetBytes ? observed.Source : MemoryBudgetSource::None;
        m_Snapshot.BudgetBytes = observed.BudgetBytes;
        m_Snapshot.ReserveBytes = reserve;
        m_Snapshot.ResidentBytes = observed.ResidentBytes;
        m_Snapshot.RetiringBytes = observed.RetiringBytes;
        m_Snapshot.Level = m_Level;
        m_Snapshot.FloorLevel = Canonical(input, kMemoryLadderRungCount);
        m_Snapshot.PredictedSteadyBytes = estimate.SteadyBytes;
        m_Snapshot.PredictedPeakBytes = estimate.PeakBytes;
        m_Snapshot.EstimateComplete = estimate.Complete;
        m_Snapshot.RecoveryProgressFrames = m_RecoveryProgress;
        m_Snapshot.RecoveryRequiredFrames = recoveryRequired;
        m_Snapshot.PeakResidentBytes = std::max(m_Snapshot.PeakResidentBytes, observed.ResidentBytes);

        if (!observed.BudgetBytes)
            m_Snapshot.Reason = MemoryAdmissionReason::BudgetUnknown;
        else if (m_OverAtFloor)
            m_Snapshot.Reason = MemoryAdmissionReason::OverBudgetAtFloor;
        else if (m_Level == 0u)
            m_Snapshot.Reason = MemoryAdmissionReason::Admitted;
        else if (m_RecoveryProgress > 0u)
            m_Snapshot.Reason = MemoryAdmissionReason::RecoveryHold;
        else
            m_Snapshot.Reason = m_ReducedCause;

        m_Snapshot.ActiveRungMask = 0;
        m_Snapshot.Views.Empty(input.Views.Num());
        m_AdmittedRequest.Empty(input.Views.Num());
        for (const auto& view : input.Views)
        {
            u32 mask = 0;
            MemoryAdmissionViewDecision decision;
            decision.View = view.View;
            decision.Requested = view.Requested;
            decision.Realized = ReduceMemoryQuality(view.Requested, m_Level, &mask);
            m_Snapshot.ActiveRungMask |= mask;
            m_Snapshot.Views.Add(decision);
            m_AdmittedRequest.Add(decision);
        }
    }

    const char* ToString(const MemoryTechnique technique)
    {
        switch (technique)
        {
            case MemoryTechnique::ReSTIRGI:
                return "restirGI";
            case MemoryTechnique::ReSTIRDI:
                return "restirDI";
            case MemoryTechnique::ReSTIRPT:
                return "restirPT";
            case MemoryTechnique::RayTracedReflections:
                return "rayTracedReflections";
            case MemoryTechnique::RayTracedShadows:
                return "rayTracedShadows";
            case MemoryTechnique::SSGI:
                return "ssgi";
            case MemoryTechnique::SSR:
                return "ssr";
            case MemoryTechnique::MotionBlur:
                return "motionBlur";
            case MemoryTechnique::DepthOfField:
                return "depthOfField";
            case MemoryTechnique::Bloom:
                return "bloom";
            case MemoryTechnique::PathTracer:
                return "pathTracer";
            case MemoryTechnique::COUNT:
                break;
        }
        return "unknown";
    }

    const char* ToString(const MemoryBudgetSource source)
    {
        switch (source)
        {
            case MemoryBudgetSource::None:
                return "none";
            case MemoryBudgetSource::OsReported:
                return "osReported";
            case MemoryBudgetSource::AllocatorHeuristic:
                return "allocatorHeuristic";
            case MemoryBudgetSource::Configured:
                return "configured";
            case MemoryBudgetSource::Injected:
                return "injected";
        }
        return "unknown";
    }

    const char* ToString(const MemoryAdmissionReason reason)
    {
        switch (reason)
        {
            case MemoryAdmissionReason::Admitted:
                return "admitted";
            case MemoryAdmissionReason::BudgetUnknown:
                return "budgetUnknown";
            case MemoryAdmissionReason::DeviceBudget:
                return "deviceBudget";
            case MemoryAdmissionReason::TransitionOverlap:
                return "transitionOverlap";
            case MemoryAdmissionReason::RecoveryHold:
                return "recoveryHold";
            case MemoryAdmissionReason::OverBudgetAtFloor:
                return "overBudgetAtFloor";
        }
        return "unknown";
    }
} // namespace OloEngine
