#pragma once

// Pure request parsing and result shaping for olo_terrain_pick. The editor-bound
// handler resolves viewport coordinates through the live editor camera, transforms
// the resulting world ray into terrain-local space, and submits it to
// TerrainGPUPicker. This header deliberately knows neither EditorLayer nor the GPU
// picker, so every externally visible state can be pinned in headless unit tests.
//
// The viewport ray sources, their exactly-one rule and the top-left pixel
// convention are shared with olo_rt_trace_ray (McpViewportRay.h), so the two
// tools cannot drift apart on what a viewport coordinate means.

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpViewportRay.h"

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace OloEngine::MCP::TerrainPick
{
    using Json = nlohmann::json;

    using RaySource = ViewportRay::RaySource;

    struct WorldRay
    {
        glm::vec3 Origin{ 0.0f };
        glm::vec3 Direction{ 0.0f, -1.0f, 0.0f };
        f32 MaxDistance = 2000.0f;
    };

    struct Request
    {
        RaySource Source = RaySource::ViewportPixel;
        glm::vec2 Coordinate{ 0.0f };
        glm::uvec2 ViewportDimensions{ 0u };
        WorldRay Ray;
    };

    using ViewportRay::IsFinite;
    using ViewportRay::ParseVec3;
    using ViewportRay::SourceToken;

    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, Request& out)
    {
        if (const auto error = ViewportRay::CheckExactlyOneSource(args, "worldRay"))
            return error;

        Request parsed;
        if (!ViewportRay::Present(args, "worldRay"))
        {
            ViewportRay::ViewportInput viewport;
            if (const auto error = ViewportRay::ParseViewportInput(args, viewport))
                return error;
            parsed.Source = viewport.Source;
            parsed.Coordinate = viewport.Coordinate;
            parsed.ViewportDimensions = viewport.ViewportDimensions;
        }
        else
        {
            const Json& ray = args["worldRay"];
            if (!ray.is_object() || !ray.contains("origin") || !ray.contains("direction"))
                return "Invalid 'worldRay': expected origin and direction.";
            if (const auto error = ParseVec3(ray["origin"], "worldRay.origin", parsed.Ray.Origin))
                return error;
            if (const auto error = ParseVec3(ray["direction"], "worldRay.direction", parsed.Ray.Direction))
                return error;
            const f32 directionLength = glm::length(parsed.Ray.Direction);
            if (!std::isfinite(directionLength) || directionLength <= 1.0e-6f)
                return "Invalid 'worldRay.direction': direction must have finite, non-zero length.";
            parsed.Ray.Direction /= directionLength;

            if (ray.contains("maxDistance"))
            {
                const Json& distance = ray["maxDistance"];
                if (!(distance.is_number_float() || distance.is_number_integer() || distance.is_number_unsigned()))
                    return "Invalid 'worldRay.maxDistance': expected a finite positive number.";
                const f64 value = distance.get<f64>();
                if (!std::isfinite(value) || value <= 0.0)
                    return "Invalid 'worldRay.maxDistance': expected a finite positive number.";
                parsed.Ray.MaxDistance = static_cast<f32>(value);
                if (!std::isfinite(parsed.Ray.MaxDistance))
                    return "Invalid 'worldRay.maxDistance': value exceeds the supported float range.";
            }
            parsed.Source = RaySource::WorldRay;
        }

        out = parsed;
        return std::nullopt;
    }

    using ViewportRay::Vec2Json;
    using ViewportRay::Vec3Json;

    [[nodiscard]] inline Json RayJson(const WorldRay& ray)
    {
        return Json{
            { "origin", Vec3Json(ray.Origin) },
            { "direction", Vec3Json(ray.Direction) },
            { "maxDistance", ray.MaxDistance },
        };
    }

    [[nodiscard]] inline Json InputJson(const Request& request)
    {
        Json input{ { "source", SourceToken(request.Source) } };
        switch (request.Source)
        {
            case RaySource::ViewportPixel:
                input["coordinate"] = Vec2Json(request.Coordinate);
                input["viewport"] = Json{ { "width", request.ViewportDimensions.x }, { "height", request.ViewportDimensions.y } };
                break;
            case RaySource::ViewportNormalized:
                input["coordinate"] = Vec2Json(request.Coordinate);
                break;
            case RaySource::WorldRay:
                input["ray"] = RayJson(request.Ray);
                break;
        }
        return input;
    }

    enum class Status : u8
    {
        Unavailable,
        Pending,
        Answered,
    };

    struct Snapshot
    {
        Status State = Status::Unavailable;
        std::string UnavailableReason;
        u32 RayId = 0;
        Request Input;
        std::optional<WorldRay> ResolvedWorldRay;
        bool Hit = false;
        std::optional<glm::vec3> WorldHit;
        std::optional<glm::vec3> LocalHit;
        u32 LatencyFrames = 0;
        u32 OverflowFlags = 0;
    };

    inline constexpr u32 kOverflowNodes = 1u;
    inline constexpr u32 kOverflowCandidates = 2u;
    inline constexpr u32 kOverflowMarch = 4u;

    [[nodiscard]] inline Json OverflowJson(u32 flags)
    {
        return Json{
            { "any", flags != 0u },
            { "rawFlags", flags },
            { "nodes", (flags & kOverflowNodes) != 0u },
            { "candidates", (flags & kOverflowCandidates) != 0u },
            { "march", (flags & kOverflowMarch) != 0u },
        };
    }

    [[nodiscard]] inline Json BuildResult(const Snapshot& snapshot)
    {
        Json result{
            { "status", snapshot.State == Status::Unavailable ? "unavailable" : snapshot.State == Status::Pending ? "pending"
                                                                                                                  : "answered" },
            { "rayId", snapshot.RayId },
            { "input", InputJson(snapshot.Input) },
            { "overflow", OverflowJson(snapshot.OverflowFlags) },
        };
        if (snapshot.ResolvedWorldRay)
            result["worldRay"] = RayJson(*snapshot.ResolvedWorldRay);

        if (snapshot.State == Status::Unavailable)
        {
            result["reason"] = snapshot.UnavailableReason.empty() ? "Terrain GPU picking is unavailable." : snapshot.UnavailableReason;
            return result;
        }
        if (snapshot.State == Status::Pending)
            return result;

        result["hit"] = snapshot.Hit;
        result["latencyFrames"] = snapshot.LatencyFrames;
        if (snapshot.Hit)
        {
            if (snapshot.WorldHit)
                result["worldHit"] = Vec3Json(*snapshot.WorldHit);
            if (snapshot.LocalHit)
                result["localHit"] = Vec3Json(*snapshot.LocalHit);
        }
        return result;
    }

    [[nodiscard]] inline Json InputSchema()
    {
        const auto vec3 = [](std::string_view description)
        { return Schema::Array(Schema::Number()).MinItems(3).MaxItems(3).Desc(description); };

        return Schema::Object()
            .Prop("viewportPixel", ViewportRay::ViewportPixelSchema())
            .Prop("viewportNormalized", ViewportRay::ViewportNormalizedSchema())
            .Prop("worldRay", Schema::Object()
                                  .Prop("origin", vec3("World-space ray origin."))
                                  .Prop("direction", vec3("Finite non-zero world-space direction; normalized by the tool."))
                                  .Prop("maxDistance", Schema::Number().Desc("Finite positive world-space reach (default 2000)."))
                                  .Required({ "origin", "direction" })
                                  .NoAdditional())
            .NoAdditional();
    }
} // namespace OloEngine::MCP::TerrainPick
