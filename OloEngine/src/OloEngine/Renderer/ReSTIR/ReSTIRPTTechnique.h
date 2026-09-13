#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRGITechnique.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace OloEngine
{
    // Pure contracts for the restricted prototype, not evidence of a live PT pass.
    // Engagement means an actual usable output, not a requested setting.
    struct ReSTIRPTOwnershipInputs
    {
        bool DIActive = false;
        bool GIActive = false;
        bool PTActive = false;
        bool DDGIAvailable = false;
        bool SSGIActive = false;
        bool GICacheTailRequested = true;
        bool SpecularIBLActive = false;
        bool SSRActive = false;
        bool RTReflectionActive = false;
    };

    struct ReSTIRPTOwnership
    {
        bool DIAtPrimary = false;
        bool ConventionalDirectAtPrimary = true;
        bool PTIndirectDiffuse = false;
        bool PTIndirectSpecular = false;
        IndirectDiffuseSources DiffuseFallbacks{};
        bool SpecularIBL = false;
        bool SSR = false;
        bool RTReflection = false;
        u32 PTCacheTailReads = 0;
    };

    [[nodiscard("Apply the validated PT policy result")]] constexpr ReSTIRPTOwnership SelectReSTIRPTOwnership(const ReSTIRPTOwnershipInputs& inputs)
    {
        ReSTIRPTOwnership result{};
        result.DIAtPrimary = inputs.DIActive;
        result.ConventionalDirectAtPrimary = !inputs.DIActive;
        result.PTIndirectDiffuse = inputs.PTActive;
        result.PTIndirectSpecular = inputs.PTActive;
        if (inputs.PTActive)
        {
            result.DiffuseFallbacks = { false, false, false, false };
            return result;
        }
        // DDGIAtPrimary names the existing probe/lightmap/sky ladder, which
        // remains the owner even when the DDGI volume itself is unavailable.
        result.DiffuseFallbacks = SelectIndirectDiffuseSources({
            .ReSTIRGIActive = inputs.GIActive,
            .SSGIRequested = inputs.SSGIActive,
            .DDGITailRequested = inputs.GICacheTailRequested && inputs.DDGIAvailable,
        });
        result.SpecularIBL = inputs.SpecularIBLActive;
        result.SSR = inputs.SSRActive;
        result.RTReflection = inputs.RTReflectionActive;
        return result;
    }

    // One entry per scheduled source reservoir, never per mapping. Confidence
    // includes failed proposals. No survivor, acceptance or map count enters here.
    struct ReSTIRPTSourceConfidence
    {
        u64 SourceSlot = 0;
        f32 Confidence = 0.0f;
        f32 ScheduledBudget = 0.0f;
    };

    struct ReSTIRPTConfidenceResult
    {
        bool Valid = false;
        f32 Confidence = 0.0f;
    };

    [[nodiscard("Apply the validated PT policy result")]] inline ReSTIRPTConfidenceResult CombineReSTIRPTSourceConfidence(
        std::span<const ReSTIRPTSourceConfidence> sources, f32 cap)
    {
        if (!std::isfinite(cap) || cap <= 0.0f)
            return {};
        f32 total = 0.0f;
        const sizet sourceCount = sources.size();
        for (sizet i = 0; i < sourceCount; ++i)
        {
            const auto& source = sources[i];
            if (!std::isfinite(source.Confidence) || source.Confidence < 0.0f ||
                !std::isfinite(source.ScheduledBudget) || source.ScheduledBudget < 0.0f)
                return {};
            for (sizet j = 0; j < i; ++j)
                if (sources[j].SourceSlot == source.SourceSlot)
                    return {}; // A duplicated map must not silently buy extra M.
            const f32 contribution = std::min(source.Confidence, source.ScheduledBudget);
            // Saturate before addition, so even finite FLT_MAX inputs cannot overflow.
            total += std::min(contribution, cap - total);
        }
        return { true, total };
    }

    enum class ReSTIRPTReservoirStage : u32
    {
        Initial,
        Temporal,
        Spatial,
    };

    struct ReSTIRPTHistoryRecord
    {
        u32 LayoutVersion = 0;
        u64 SceneEpoch = 0;
        u64 WrittenFrame = 0;
        u64 BirthFrame = 0;
        u64 SourcePixel = 0;
        u64 BirthPixel = 0;
        u32 ReuseDepth = 0;
        ReSTIRPTReservoirStage Stage = ReSTIRPTReservoirStage::Initial;
    };

    enum class ReSTIRPTHistoryRejection : u32
    {
        None,
        Layout,
        Epoch,
        Frame,
        Stage,
        Lineage,
        Receiver,
    };

    // SceneEpoch must cover geometry, transforms, materials, lighting, origin,
    // sampler layout and path settings. The caller still retraces the suffix.
    [[nodiscard("Apply the validated PT policy result")]] constexpr ReSTIRPTHistoryRejection ValidateReSTIRPTHistory(
        const ReSTIRPTHistoryRecord& record, u32 currentLayout, u64 currentEpoch,
        u64 currentFrame, bool receiverValid)
    {
        if (currentLayout == 0u || record.LayoutVersion != currentLayout)
            return ReSTIRPTHistoryRejection::Layout;
        if (record.SceneEpoch != currentEpoch)
            return ReSTIRPTHistoryRejection::Epoch;
        if (currentFrame == 0u || record.WrittenFrame != currentFrame - 1u)
            return ReSTIRPTHistoryRejection::Frame;
        if (record.Stage != ReSTIRPTReservoirStage::Initial)
            return ReSTIRPTHistoryRejection::Stage;
        if (record.ReuseDepth != 0u || record.BirthFrame != record.WrittenFrame ||
            record.BirthPixel != record.SourcePixel)
            return ReSTIRPTHistoryRejection::Lineage;
        if (!receiverValid)
            return ReSTIRPTHistoryRejection::Receiver;
        return ReSTIRPTHistoryRejection::None;
    }
} // namespace OloEngine
