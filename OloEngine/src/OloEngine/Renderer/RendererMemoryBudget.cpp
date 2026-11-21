#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RendererMemoryBudget.h"

#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/GBuffer.h"

#include <algorithm>

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
            case MemoryTechnique::ReSTIRPT:            // path records live in the pass's own pools
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
            const bool sameBand = was && on && TechniqueBandPixels(technique, *allocated) == TechniqueBandPixels(technique, target);
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

        const u64 measuredPixels = Pixels(inventory.DisplayWidth, inventory.DisplayHeight);
        const u64 targetPixels = Pixels(target.DisplayWidth, target.DisplayHeight);
        const u64 scaled = inventory.NodeTargetBytes + inventory.TransientDemandBytes + inventory.OtherHistoryBytes;
        transition.NewBytes += ScaleBytes(scaled, measuredPixels, targetPixels);
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
        // A view that has not drawn yet has no measurement of its own. Another view of the
        // same renderer runs the same passes at another size, so its pixel-proportional
        // owners are priced from that one instead of being left out.
        const ViewMemoryInventory* measuredView = nullptr;
        for (const auto& view : input.Views)
        {
            if (view.Inventory.IsMeasured())
            {
                measuredView = &view.Inventory;
                break;
            }
        }

        MemoryTransition total;
        bool optionalAllocated = false;
        bool optionalTarget = false;
        bool anyAllocated = false;
        for (const auto& view : input.Views)
        {
            const MemoryQualityConfig target = ReduceMemoryQuality(view.Requested, level);
            ViewMemoryInventory borrowed;
            const ViewMemoryInventory* inventory = &view.Inventory;
            if (!view.Inventory.IsMeasured() && measuredView)
            {
                borrowed.DisplayWidth = measuredView->DisplayWidth;
                borrowed.DisplayHeight = measuredView->DisplayHeight;
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
        return estimate;
    }

    void RendererMemoryGovernor::AdmitFresh(const MemoryAdmissionInput& input, const u64 limit, const u64 budget)
    {
        const u32 floor = Canonical(input, kMemoryLadderRungCount);
        const bool requestedFitsSteady = EstimateLevel(input, 0u).SteadyBytes <= limit;

        u32 chosen = floor;
        bool found = false;
        for (u32 level = 0; level <= kMemoryLadderRungCount; ++level)
        {
            if (level > 0u && SameRealization(input, level, level - 1u))
                continue;
            const LevelEstimate estimate = EstimateLevel(input, level);
            if (estimate.SteadyBytes <= limit && estimate.PeakBytes <= budget)
            {
                chosen = level;
                found = true;
                break;
            }
        }

        m_Level = chosen;
        // The requested configuration would fit once the old resources retire: it is only
        // the overlap that does not.
        m_ReducedCause = requestedFitsSteady ? MemoryAdmissionReason::TransitionOverlap : MemoryAdmissionReason::DeviceBudget;
        m_OverAtFloor = !found && EstimateLevel(input, floor).SteadyBytes > limit;
        m_SettleTicks = m_Policy.SettleFrames;
        m_RecoveryProgress = 0;
        m_RecoveryBackoffShift = 0;
        m_LearnedClimbBytes = 0;
        m_ClimbOutstanding = false;
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
            if (estimate.PeakBytes <= budget || estimate.NewBytes <= leastNew + reserve)
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

        // A new budget is new information: what a failed climb taught about the old one
        // no longer applies. An OS-reported budget drifts by a little every frame, so
        // only a move larger than the recovery band counts as new.
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
            m_LearnedClimbBytes = 0;
            m_RecoveryProgress = 0;
            m_ClimbOutstanding = false;
            m_Snapshot.PeakResidentBytes = 0;
        }

        const u32 previousLevel = m_Level;
        const u32 recoveryRequired = m_Policy.RecoveryFrames << std::min(m_RecoveryBackoffShift, m_Policy.MaxRecoveryBackoffShift);
        u64 reserve = 0;

        if (!observed.BudgetBytes)
        {
            // Nothing to admit against. The request is active and the report says the
            // budget is unknown: an unenforced budget is not a satisfied one.
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

            if (requestChanged)
            {
                if (m_HasAdmitted)
                    ++m_Snapshot.Readmissions;
                AdmitFresh(input, limit, budget);
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
                const u64 live = observed.LiveBytes();
                const u32 floor = Canonical(input, kMemoryLadderRungCount);
                const u64 climbWindow = static_cast<u64>(recoveryRequired) + m_Policy.SettleFrames;
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
                        if (live > m_ClimbPredictedSteady)
                            m_LearnedClimbBytes = std::max(m_LearnedClimbBytes, live - m_ClimbPredictedSteady);
                        m_ClimbOutstanding = false;
                        m_Level = std::max(m_Level, std::min(m_LevelBeforeClimb, floor));
                        m_ReducedCause = MemoryAdmissionReason::DeviceBudget;
                        m_SettleTicks = m_Policy.SettleFrames;
                        m_OverAtFloor = false;
                    }
                    else if (m_Level < floor)
                    {
                        m_ClimbOutstanding = false;
                        m_Level = ChooseDescent(input, limit, budget, reserve);
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
                        m_ClimbOutstanding = false; // the climb held for a whole run
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
                            if (estimate.SteadyBytes + m_LearnedClimbBytes <= climbLimit &&
                                estimate.PeakBytes + m_LearnedClimbBytes <= budget)
                            {
                                candidate = level;
                                candidateSteady = estimate.SteadyBytes;
                                break;
                            }
                        }
                        if (candidate < m_Level)
                        {
                            ++m_RecoveryProgress;
                            if (m_RecoveryProgress >= recoveryRequired)
                            {
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

        if (m_Level != previousLevel && !requestChanged)
        {
            ++m_Snapshot.Transitions;
            if (m_Level > previousLevel)
                ++m_Snapshot.Degradations;
            else
                ++m_Snapshot.Recoveries;
        }
        if (m_Level != previousLevel || requestChanged)
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
