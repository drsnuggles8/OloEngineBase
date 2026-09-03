#pragma once

// Shared response envelope for renderer statistics tools whose zero values are
// meaningful. Availability, feature enablement and sample freshness are three
// separate facts: collapsing any of them into "all counters are zero" makes an
// inactive feature indistinguishable from a healthy, idle one.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>
#include <utility>

namespace OloEngine::MCP::StatsSnapshot
{
    using Json = nlohmann::json;

    enum class FreshnessModel : u8
    {
        PreviousFrame,
        SessionCumulative,
        CurrentBlockingReadback,
    };

    struct State
    {
        // Available means the owning renderer/pass exists and can answer.
        bool Available = false;
        // Enabled is the feature's current runtime gate, independent of whether
        // its owner exists.
        bool Enabled = false;
        // HasData means the payload is a real sample. A sample whose every
        // counter is zero still sets this true.
        bool HasData = false;
        FreshnessModel Freshness = FreshnessModel::PreviousFrame;
        std::optional<u64> SampleAgeFrames;
        bool Stale = false;
    };

    [[nodiscard]] inline constexpr std::string_view Status(const State& state) noexcept
    {
        if (!state.Available)
            return "unavailable";
        if (!state.Enabled)
            return "disabled";
        if (!state.HasData)
            return "noData";
        return "ready";
    }

    [[nodiscard]] inline constexpr std::string_view FreshnessName(FreshnessModel model) noexcept
    {
        switch (model)
        {
            case FreshnessModel::PreviousFrame:
                return "previousFrame";
            case FreshnessModel::SessionCumulative:
                return "sessionCumulative";
            case FreshnessModel::CurrentBlockingReadback:
                return "currentBlockingReadback";
        }
        return "unknown";
    }

    [[nodiscard("this builds the response envelope; it does not send it")]] inline Json ToJson(const State& state)
    {
        Json availability{ { "available", state.Available },
                           { "enabled", state.Enabled },
                           { "hasData", state.HasData },
                           { "status", Status(state) } };

        Json freshness{ { "model", FreshnessName(state.Freshness) }, { "stale", state.Stale } };
        if (state.SampleAgeFrames.has_value())
            freshness["sampleAgeFrames"] = *state.SampleAgeFrames;
        else
            freshness["sampleAgeFrames"] = nullptr;

        return Json{ { "availability", std::move(availability) }, { "freshness", std::move(freshness) } };
    }
} // namespace OloEngine::MCP::StatsSnapshot
