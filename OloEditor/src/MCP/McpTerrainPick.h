#pragma once

// Pure request parsing and result shaping for olo_terrain_pick. The editor-bound
// handler resolves viewport coordinates through the live editor camera, transforms
// the resulting world ray into terrain-local space, and submits it to
// TerrainGPUPicker. This header deliberately knows neither EditorLayer nor the GPU
// picker, so every externally visible state can be pinned in headless unit tests.

#include "MCP/McpSchemaBuilder.h"

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

    enum class RaySource : u8
    {
        ViewportPixel,
        ViewportNormalized,
        WorldRay,
    };

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

    [[nodiscard]] inline const char* SourceToken(RaySource source)
    {
        switch (source)
        {
            case RaySource::ViewportPixel:
                return "viewportPixel";
            case RaySource::ViewportNormalized:
                return "viewportNormalized";
            case RaySource::WorldRay:
                return "worldRay";
        }
        return "unknown";
    }

    [[nodiscard]] inline bool IsFinite(const glm::vec2& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    [[nodiscard]] inline bool IsFinite(const glm::vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    [[nodiscard]] inline std::optional<std::string> ParseVec2(const Json& value, std::string_view name, glm::vec2& out)
    {
        if (!value.is_array() || value.size() != 2u)
            return "Invalid '" + std::string(name) + "': expected exactly two finite numbers.";
        for (const Json& component : value)
        {
            if (!(component.is_number_float() || component.is_number_integer() || component.is_number_unsigned()))
                return "Invalid '" + std::string(name) + "': expected exactly two finite numbers.";
        }
        const f64 x = value[0].get<f64>();
        const f64 y = value[1].get<f64>();
        if (!std::isfinite(x) || !std::isfinite(y))
            return "Invalid '" + std::string(name) + "': expected exactly two finite numbers.";
        out = { static_cast<f32>(x), static_cast<f32>(y) };
        if (!IsFinite(out))
            return "Invalid '" + std::string(name) + "': values exceed the supported float range.";
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> ParseVec3(const Json& value, std::string_view name, glm::vec3& out)
    {
        if (!value.is_array() || value.size() != 3u)
            return "Invalid '" + std::string(name) + "': expected exactly three finite numbers.";
        for (const Json& component : value)
        {
            if (!(component.is_number_float() || component.is_number_integer() || component.is_number_unsigned()))
                return "Invalid '" + std::string(name) + "': expected exactly three finite numbers.";
        }
        const f64 x = value[0].get<f64>();
        const f64 y = value[1].get<f64>();
        const f64 z = value[2].get<f64>();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return "Invalid '" + std::string(name) + "': expected exactly three finite numbers.";
        out = { static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z) };
        if (!IsFinite(out))
            return "Invalid '" + std::string(name) + "': values exceed the supported float range.";
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> ParsePositiveU32(const Json& value, std::string_view name, u32& out)
    {
        if (!(value.is_number_integer() || value.is_number_unsigned()))
            return "Invalid '" + std::string(name) + "': expected a positive integer.";
        u64 parsed = 0u;
        if (value.is_number_unsigned())
        {
            parsed = value.get<u64>();
        }
        else
        {
            const i64 signedValue = value.get<i64>();
            if (signedValue <= 0)
                return "Invalid '" + std::string(name) + "': expected a positive 32-bit integer.";
            parsed = static_cast<u64>(signedValue);
        }
        if (parsed > static_cast<u64>(std::numeric_limits<u32>::max()))
            return "Invalid '" + std::string(name) + "': expected a positive 32-bit integer.";
        out = static_cast<u32>(parsed);
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, Request& out)
    {
        const bool hasPixel = args.contains("viewportPixel") && !args["viewportPixel"].is_null();
        const bool hasNormalized = args.contains("viewportNormalized") && !args["viewportNormalized"].is_null();
        const bool hasWorldRay = args.contains("worldRay") && !args["worldRay"].is_null();
        if (static_cast<u32>(hasPixel) + static_cast<u32>(hasNormalized) + static_cast<u32>(hasWorldRay) != 1u)
            return "Provide exactly one ray source: 'viewportPixel', 'viewportNormalized', or 'worldRay'.";

        Request parsed;
        if (hasPixel)
        {
            const Json& pixel = args["viewportPixel"];
            if (!pixel.is_object() || !pixel.contains("coordinate") || !pixel.contains("width") || !pixel.contains("height"))
                return "Invalid 'viewportPixel': expected coordinate, width, and height.";
            if (const auto error = ParseVec2(pixel["coordinate"], "viewportPixel.coordinate", parsed.Coordinate))
                return error;
            if (const auto error = ParsePositiveU32(pixel["width"], "viewportPixel.width", parsed.ViewportDimensions.x))
                return error;
            if (const auto error = ParsePositiveU32(pixel["height"], "viewportPixel.height", parsed.ViewportDimensions.y))
                return error;
            if (parsed.Coordinate.x < 0.0f || parsed.Coordinate.y < 0.0f ||
                parsed.Coordinate.x >= static_cast<f32>(parsed.ViewportDimensions.x) ||
                parsed.Coordinate.y >= static_cast<f32>(parsed.ViewportDimensions.y))
            {
                return "Invalid 'viewportPixel.coordinate': coordinate lies outside the supplied viewport dimensions.";
            }
            parsed.Source = RaySource::ViewportPixel;
        }
        else if (hasNormalized)
        {
            if (const auto error = ParseVec2(args["viewportNormalized"], "viewportNormalized", parsed.Coordinate))
                return error;
            if (parsed.Coordinate.x < 0.0f || parsed.Coordinate.x > 1.0f ||
                parsed.Coordinate.y < 0.0f || parsed.Coordinate.y > 1.0f)
            {
                return "Invalid 'viewportNormalized': both coordinates must be in [0, 1].";
            }
            parsed.Source = RaySource::ViewportNormalized;
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

    [[nodiscard]] inline Json Vec2Json(const glm::vec2& value)
    {
        return Json::array({ value.x, value.y });
    }

    [[nodiscard]] inline Json Vec3Json(const glm::vec3& value)
    {
        return Json::array({ value.x, value.y, value.z });
    }

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
        const auto coordinate = [](std::string_view description)
        { return Schema::Array(Schema::Number()).MinItems(2).MaxItems(2).Desc(description); };
        const auto vec3 = [](std::string_view description)
        { return Schema::Array(Schema::Number()).MinItems(3).MaxItems(3).Desc(description); };

        return Schema::Object()
            .Prop("viewportPixel", Schema::Object()
                                       .Prop("coordinate", coordinate("Viewport-relative pixel [x,y]."))
                                       .Prop("width", Schema::Int().Min(1))
                                       .Prop("height", Schema::Int().Min(1))
                                       .Required({ "coordinate", "width", "height" })
                                       .NoAdditional())
            .Prop("viewportNormalized", coordinate("Normalized viewport coordinate [x,y], each in [0,1]."))
            .Prop("worldRay", Schema::Object()
                                  .Prop("origin", vec3("World-space ray origin."))
                                  .Prop("direction", vec3("Finite non-zero world-space direction; normalized by the tool."))
                                  .Prop("maxDistance", Schema::Number().Desc("Finite positive world-space reach (default 2000)."))
                                  .Required({ "origin", "direction" })
                                  .NoAdditional())
            .NoAdditional();
    }
} // namespace OloEngine::MCP::TerrainPick
