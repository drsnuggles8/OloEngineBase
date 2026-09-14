#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRGITechnique.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <array>
#include <string_view>

namespace OloEngine
{
    enum class ReSTIRPTDebugView : u32
    {
        Radiance,
        Raw,
        HistoryValidity,
        Variance,
        Lineage,
        Conditioning,
        Clamp
    };
    struct ReSTIRPTSettings
    {
        bool Enabled = false;
        bool TemporalReuse = true;
        bool SpatialReuse = true;
        u32 InitialCandidates = 1;
        u32 MappingMask = 7;
        u32 Seed = 1211;
        f32 SpatialRadius = 8.0f;
        f32 ConfidenceCap = 8.0f;
        f32 RayEpsilon = 0.001f;
        f32 NormalBias = 0.01f;
        f32 MaxRayDistance = 1000.0f;
        f32 RadianceClamp = 0.0f;
        ReSTIRPTDebugView DebugView = ReSTIRPTDebugView::Radiance;
        [[nodiscard("Compare authored PT settings")]] auto operator==(const ReSTIRPTSettings& other) const -> bool
        {
            const bool switches = Enabled == other.Enabled && TemporalReuse == other.TemporalReuse && SpatialReuse == other.SpatialReuse;
            const bool counts = InitialCandidates == other.InitialCandidates && MappingMask == other.MappingMask && Seed == other.Seed;
            const bool reuse = Math::BitwiseEqual(SpatialRadius, other.SpatialRadius) && Math::BitwiseEqual(ConfidenceCap, other.ConfidenceCap);
            const bool rays = Math::BitwiseEqual(RayEpsilon, other.RayEpsilon) && Math::BitwiseEqual(NormalBias, other.NormalBias);
            const bool output = Math::BitwiseEqual(MaxRayDistance, other.MaxRayDistance) && Math::BitwiseEqual(RadianceClamp, other.RadianceClamp) && DebugView == other.DebugView;
            const bool sampling = switches && counts && reuse;
            return sampling && rays && output;
        }
    };
    [[nodiscard("Use sanitized PT settings")]] inline ReSTIRPTSettings SanitizeReSTIRPTSettings(ReSTIRPTSettings settings)
    {
        const auto finiteClamp = [](f32 v, f32 lo, f32 hi, f32 fallback)
        { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
        settings.InitialCandidates = std::clamp(settings.InitialCandidates, 1u, 8u);
        settings.MappingMask &= 7u;
        settings.Seed &= 0xFFFFFFu;
        settings.SpatialRadius = finiteClamp(settings.SpatialRadius, 1.0f, 32.0f, 8.0f);
        settings.ConfidenceCap = finiteClamp(settings.ConfidenceCap, 1.0f, 32.0f, 8.0f);
        settings.RayEpsilon = finiteClamp(settings.RayEpsilon, 0.00001f, 0.1f, 0.001f);
        settings.NormalBias = finiteClamp(settings.NormalBias, 0.0f, 0.1f, 0.01f);
        settings.MaxRayDistance = finiteClamp(settings.MaxRayDistance, 1.0f, 100000.0f, 1000.0f);
        settings.RadianceClamp = finiteClamp(settings.RadianceClamp, 0.0f, 1000000.0f, 0.0f);
        if (static_cast<u32>(settings.DebugView) > static_cast<u32>(ReSTIRPTDebugView::Clamp))
            settings.DebugView = ReSTIRPTDebugView::Radiance;
        return settings;
    }
    struct ReSTIRPTStats
    {
        bool Requested = false;
        bool Active = false;
        bool HistoryValid = false;
        bool BiasedClamp = false;
        bool CountersValid = false;
        u32 CounterFrame = 0;
        u64 SceneEpoch = 0;
        u64 ReservoirBytes = 0;
        std::string_view FallbackReason = "disabled";
        std::array<u32, 16> Counters{};
    };
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
