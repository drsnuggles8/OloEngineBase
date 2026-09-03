#pragma once

// Pure request parsing and result shaping for olo_lightmap_bake. The editor owns
// the asynchronous bake, scene save, and operation storage; this header fixes the
// wire contract independently so start/poll and blocking calls cannot drift.

#include "MCP/McpSchemaBuilder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine::MCP::LightmapBake
{
    using Json = nlohmann::json;

    enum class Mode : std::uint8_t
    {
        Start,
        Poll,
        Blocking,
    };

    enum class Status : std::uint8_t
    {
        Queued,
        Running,
        Succeeded,
        Failed,
    };

    struct Request
    {
        Mode RequestMode = Mode::Start;
        std::string OperationId;
        bool Save = false;
    };

    struct Result
    {
        std::uint32_t BakedEntityCount = 0;
        std::uint32_t SkippedEntityCount = 0;
        bool SaveRequested = false;
        bool Saved = false;
    };

    struct Snapshot
    {
        std::string OperationId;
        Status State = Status::Queued;
        double Progress = 0.0;
        std::string Error;
        std::optional<Result> CompletedResult;
    };

    [[nodiscard]] inline std::string_view StatusToken(Status status) noexcept
    {
        switch (status)
        {
            case Status::Queued:
                return "queued";
            case Status::Running:
                return "running";
            case Status::Succeeded:
                return "succeeded";
            case Status::Failed:
                return "failed";
        }
        return "failed";
    }

    // A process-local monotonic sequence supplied by the editor becomes an opaque,
    // stable operation id. Poll replies echo this exact string for the operation's
    // lifetime; callers never have to infer identity from scene or timing data.
    [[nodiscard]] inline std::string MakeOperationId(std::uint64_t sequence)
    {
        return "lightmap-bake-" + std::to_string(sequence);
    }

    [[nodiscard]] inline Json InputSchema()
    {
        const Schema::Node operationId = Schema::Raw(Json{ { "type", "string" }, { "minLength", 1 } })
                                             .Desc("Required only for mode=poll; returned by start/blocking.");
        return Schema::Object()
            .Prop("mode", Schema::String().Enum({ "start", "poll", "blocking" }).Desc("start begins an asynchronous bake; poll reads one operation; blocking starts and waits for completion."))
            .Prop("operationId", operationId)
            .Prop("save", Schema::Bool().Desc("For start/blocking, save the scene after a successful bake (default false)."))
            .Required({ "mode" })
            .NoAdditional();
    }

    [[nodiscard]] inline Json OutputSchema()
    {
        const Schema::Node nullableString = Schema::Raw(Json{ { "type", Json::array({ "string", "null" }) } });
        const Schema::Node operationId = Schema::Raw(Json{ { "type", "string" }, { "minLength", 1 } });
        const Schema::Node result = Schema::Object()
                                        .Prop("bakedEntityCount", Schema::Int().Min(0))
                                        .Prop("skippedEntityCount", Schema::Int().Min(0))
                                        .Prop("saveRequested", Schema::Bool())
                                        .Prop("saved", Schema::Bool())
                                        .Required({ "bakedEntityCount", "skippedEntityCount", "saveRequested", "saved" })
                                        .NoAdditional();
        const Schema::Node nullableResult = Schema::Raw(Json{ { "oneOf", Json::array({ result.ToJson(), Json{ { "type", "null" } } }) } });
        return Schema::Object()
            .Prop("operationId", operationId)
            .Prop("status", Schema::String().Enum({ "queued", "running", "succeeded", "failed" }))
            .Prop("progress", Schema::Number().Min(0).Max(1).Desc("Finite completion fraction in [0,1]."))
            .Prop("error", nullableString)
            .Prop("result", nullableResult)
            .Required({ "operationId", "status", "progress", "error", "result" });
    }

    // JSON Schema handles the basic types at dispatch. This parser deliberately
    // repeats the closed-object and mode-dependent checks so direct callers/tests
    // get the same strict contract and no unsupported combination is ignored.
    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, Request& out)
    {
        if (!args.is_object())
            return "Invalid arguments: expected an object.";
        for (auto it = args.begin(); it != args.end(); ++it)
        {
            if (it.key() != "mode" && it.key() != "operationId" && it.key() != "save")
                return "Unknown argument '" + it.key() + "'.";
        }
        if (!args.contains("mode") || !args["mode"].is_string())
            return "Missing or invalid 'mode': expected start, poll, or blocking.";

        Request parsed;
        const std::string mode = args["mode"].get<std::string>();
        if (mode == "start")
            parsed.RequestMode = Mode::Start;
        else if (mode == "poll")
            parsed.RequestMode = Mode::Poll;
        else if (mode == "blocking")
            parsed.RequestMode = Mode::Blocking;
        else
            return "Invalid 'mode': expected start, poll, or blocking.";

        const bool hasOperationId = args.contains("operationId");
        const bool hasSave = args.contains("save");
        if (hasOperationId && (!args["operationId"].is_string() || args["operationId"].get<std::string>().empty()))
            return "Invalid 'operationId': expected a non-empty string.";
        if (hasSave && !args["save"].is_boolean())
            return "Invalid 'save': expected a boolean.";

        if (parsed.RequestMode == Mode::Poll)
        {
            if (!hasOperationId)
                return "Missing required 'operationId' for mode=poll.";
            if (hasSave)
                return "'save' is only valid for mode=start or mode=blocking.";
            parsed.OperationId = args["operationId"].get<std::string>();
        }
        else
        {
            if (hasOperationId)
                return "'operationId' is only valid for mode=poll.";
            parsed.Save = hasSave && args["save"].get<bool>();
        }

        out = std::move(parsed);
        return std::nullopt;
    }

    [[nodiscard]] inline Json BuildResponse(const Snapshot& snapshot)
    {
        double progress = std::isfinite(snapshot.Progress) ? snapshot.Progress : 0.0;
        progress = std::clamp(progress, 0.0, 1.0);

        Json result = nullptr;
        if (snapshot.State == Status::Succeeded && snapshot.CompletedResult.has_value())
        {
            const Result& value = *snapshot.CompletedResult;
            result = Json{ { "bakedEntityCount", value.BakedEntityCount },
                           { "skippedEntityCount", value.SkippedEntityCount },
                           { "saveRequested", value.SaveRequested },
                           { "saved", value.Saved } };
        }

        Json error = nullptr;
        if (snapshot.State == Status::Failed)
            error = snapshot.Error.empty() ? Json("Lightmap bake failed without an error message.") : Json(snapshot.Error);

        return Json{ { "operationId", snapshot.OperationId },
                     { "status", StatusToken(snapshot.State) },
                     { "progress", snapshot.State == Status::Succeeded ? 1.0 : progress },
                     { "error", std::move(error) },
                     { "result", std::move(result) } };
    }
} // namespace OloEngine::MCP::LightmapBake
