#pragma once

// Viewport-coordinate ray sources shared by the MCP tools that cast a ray from
// the editor camera: olo_terrain_pick and olo_rt_trace_ray (#607).
//
// ONE convention, stated once: a viewport coordinate is TOP-LEFT origin, +Y
// down — the same space as olo_screenshot's pixels and olo_input_inject's
// 'viewport' / 'viewportNorm'. The camera matrices the CPU holds are GL
// convention (y up) on EVERY backend: RHIProjectionSeam applies the Vulkan y
// flip only to the matrices it uploads. So the unprojection below has no
// backend branch, and must not grow one — a row-order flip belongs to readback
// coordinates (RenderTargetRowsAreBottomUp), never to a ray.
//
// A pixel is resolved through the size the CALLER measured it in, so a pixel
// taken off a downscaled screenshot addresses the same point as its native
// twin, and the render resolution (which a non-native upscale shrinks) never
// enters: the ray depends only on the normalized coordinate and the camera.
//
// Pure: no EditorLayer, no renderer. Everything here is pinned headlessly by
// McpViewportRayTest.

#include "MCP/McpSchemaBuilder.h"

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace OloEngine::MCP::ViewportRay
{
    using Json = nlohmann::json;

    enum class RaySource : u8
    {
        ViewportPixel,
        ViewportNormalized,
        WorldRay,
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

    // A camera-relative ray source, as parsed. WorldRay is never stored here: each
    // tool parses its own world form (terrain pick takes one ray with a reach,
    // rt_trace_ray a batch with tMin/tMax).
    struct ViewportInput
    {
        RaySource Source = RaySource::ViewportPixel;
        glm::vec2 Coordinate{ 0.0f };      // pixel, or normalized [0,1]
        glm::uvec2 ViewportDimensions{ 0u }; // viewportPixel only: the size the pixel was measured in
    };

    // ---- parsing -----------------------------------------------------------

    [[nodiscard]] inline bool IsFinite(const glm::vec2& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    [[nodiscard]] inline bool IsFinite(const glm::vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    [[nodiscard]] inline bool IsNumber(const Json& value)
    {
        return value.is_number_float() || value.is_number_integer() || value.is_number_unsigned();
    }

    [[nodiscard]] inline std::optional<std::string> ParseVec2(const Json& value, std::string_view name, glm::vec2& out)
    {
        if (!value.is_array() || value.size() != 2u)
            return "Invalid '" + std::string(name) + "': expected exactly two finite numbers.";
        for (const Json& component : value)
        {
            if (!IsNumber(component))
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
            if (!IsNumber(component))
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
        if (parsed == 0u || parsed > static_cast<u64>(std::numeric_limits<u32>::max()))
            return "Invalid '" + std::string(name) + "': expected a positive 32-bit integer.";
        out = static_cast<u32>(parsed);
        return std::nullopt;
    }

    [[nodiscard]] inline bool Present(const Json& args, std::string_view key)
    {
        return args.is_object() && args.contains(key) && !args[std::string(key)].is_null();
    }

    // The exactly-one rule, shared so both tools refuse a mixed request with the
    // same words. `worldKey` is the tool's own spelling of its world form.
    [[nodiscard]] inline std::optional<std::string> CheckExactlyOneSource(const Json& args, std::string_view worldKey)
    {
        const u32 count = static_cast<u32>(Present(args, "viewportPixel")) +
                          static_cast<u32>(Present(args, "viewportNormalized")) + static_cast<u32>(Present(args, worldKey));
        if (count != 1u)
        {
            return "Provide exactly one ray source: 'viewportPixel', 'viewportNormalized', or '" + std::string(worldKey) +
                   "'.";
        }
        return std::nullopt;
    }

    // Parse whichever viewport source is present. Call only after
    // CheckExactlyOneSource said the world form is NOT the one given.
    [[nodiscard]] inline std::optional<std::string> ParseViewportInput(const Json& args, ViewportInput& out)
    {
        ViewportInput parsed;
        if (Present(args, "viewportPixel"))
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
        else if (Present(args, "viewportNormalized"))
        {
            if (const auto error = ParseVec2(args["viewportNormalized"], "viewportNormalized", parsed.Coordinate))
                return error;
            if (parsed.Coordinate.x < 0.0f || parsed.Coordinate.x > 1.0f || parsed.Coordinate.y < 0.0f ||
                parsed.Coordinate.y > 1.0f)
            {
                return "Invalid 'viewportNormalized': both coordinates must be in [0, 1].";
            }
            parsed.Source = RaySource::ViewportNormalized;
        }
        else
        {
            return "Provide 'viewportPixel' or 'viewportNormalized'.";
        }
        out = parsed;
        return std::nullopt;
    }

    // ---- resolution --------------------------------------------------------

    // The viewport input as a top-left normalized coordinate. For a pixel this
    // divides by the size the caller measured it in, not by any size of ours.
    [[nodiscard]] inline glm::vec2 Normalized(const ViewportInput& input)
    {
        if (input.Source == RaySource::ViewportPixel)
            return input.Coordinate / glm::vec2(input.ViewportDimensions);
        return input.Coordinate;
    }

    struct CameraRay
    {
        glm::vec3 Origin{ 0.0f };    // on the near plane
        glm::vec3 Direction{ 0.0f }; // normalized
        f32 Length = 0.0f;           // near plane to far plane along the ray
    };

    // What the editor's resolver hands back: a ray, or why there is none.
    struct CameraRayResolution
    {
        std::optional<CameraRay> Ray;
        std::string Error;
    };

    // Top-left normalized coordinate -> world ray through a GL-convention
    // view-projection. y is flipped HERE, unconditionally: top of the viewport is
    // NDC +1 on both backends, because the CPU matrix is GL-shaped on both.
    [[nodiscard]] inline std::optional<CameraRay> UnprojectNormalized(const glm::vec2& normalizedTopLeft,
                                                                      const glm::mat4& viewProjection)
    {
        const f32 ndcX = normalizedTopLeft.x * 2.0f - 1.0f;
        const f32 ndcY = 1.0f - normalizedTopLeft.y * 2.0f;

        const glm::mat4 inverse = glm::inverse(viewProjection);
        glm::vec4 nearWorld = inverse * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 farWorld = inverse * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
        if (!std::isfinite(nearWorld.w) || !std::isfinite(farWorld.w) || std::abs(nearWorld.w) < 1.0e-20f ||
            std::abs(farWorld.w) < 1.0e-20f)
        {
            return std::nullopt;
        }
        nearWorld /= nearWorld.w;
        farWorld /= farWorld.w;

        const glm::vec3 span = glm::vec3(farWorld) - glm::vec3(nearWorld);
        const f32 length = glm::length(span);
        // A degenerate view-projection (uninitialized camera, zero-size viewport
        // mid-resize) yields NaNs or a zero span through the inverse.
        if (!std::isfinite(length) || length <= 1.0e-6f || !IsFinite(glm::vec3(nearWorld)))
            return std::nullopt;
        return CameraRay{ glm::vec3(nearWorld), span / length, length };
    }

    // ---- shaping -----------------------------------------------------------

    [[nodiscard]] inline Json Vec2Json(const glm::vec2& value)
    {
        return Json::array({ value.x, value.y });
    }

    [[nodiscard]] inline Json Vec3Json(const glm::vec3& value)
    {
        return Json::array({ value.x, value.y, value.z });
    }

    // The request as the caller gave it, plus the normalized coordinate it
    // resolved to, so a pixel/size mismatch is visible in the reply.
    [[nodiscard]] inline Json ViewportInputJson(const ViewportInput& input)
    {
        Json json{ { "source", SourceToken(input.Source) }, { "coordinate", Vec2Json(input.Coordinate) } };
        if (input.Source == RaySource::ViewportPixel)
            json["viewport"] = Json{ { "width", input.ViewportDimensions.x }, { "height", input.ViewportDimensions.y } };
        json["normalized"] = Vec2Json(Normalized(input));
        return json;
    }

    // ---- schemas -----------------------------------------------------------

    [[nodiscard]] inline Schema::Node ViewportPixelSchema()
    {
        return Schema::Object()
            .Prop("coordinate", Schema::Array(Schema::Number())
                                    .MinItems(2)
                                    .MaxItems(2)
                                    .Desc("Viewport pixel [x,y], top-left origin, +Y down — olo_screenshot's pixel "
                                          "space. Continuous: pass x+0.5 for a pixel centre."))
            .Prop("width", Schema::Int().Min(1).Desc("Width of the image the pixel was measured in."))
            .Prop("height", Schema::Int().Min(1).Desc("Height of the image the pixel was measured in."))
            .Required({ "coordinate", "width", "height" })
            .NoAdditional();
    }

    [[nodiscard]] inline Schema::Node ViewportNormalizedSchema()
    {
        return Schema::Array(Schema::Number())
            .MinItems(2)
            .MaxItems(2)
            .Desc("Normalized viewport coordinate [x,y], each in [0,1], top-left origin, +Y down.");
    }
} // namespace OloEngine::MCP::ViewportRay
