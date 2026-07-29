#pragma once

// Shared `region: {x, y, w, h}` argument for the capture tools — issue #607's
// native-resolution region-capture gap.
//
// The gap: `olo_render_capture_target` and `olo_screenshot` always return the
// WHOLE target, rescaled so its width is at most `maxWidth` (hard-clamped to
// 4096). A pixel-scale artifact can only be measured on 1:1 pixels. Root-causing
// the GTAO "goosebumps" weave meant taking the spatial autocorrelation of the AO
// buffer to identify the noise's period — but a 4291x2320 target can only come
// back resampled to <= 4096, which moved the diagnostic 64 px Hilbert-LUT tile
// period to 61.1 px and inflated short-lag correlation, so the first measurement
// could not tell the regression from the fix. The workaround was shrinking the
// editor window until the target fit. With a region, a 512x512 crop of any target
// comes back untouched.
//
// Contract, shared by every consumer so the coordinate space can't drift:
//   * TOP-LEFT origin, in texels of the mip being captured (or pixels of the
//     viewport for olo_screenshot) — the same space `olo_render_target_stats`'
//     `rect` and `olo_render_probe_pixel`'s `space:"texel"` already use, and the
//     orientation of the returned PNG. GL's bottom-left row order is handled by
//     the readback, never by the caller.
//   * w/h must be > 0; a zero-sized region is rejected at parse time rather than
//     silently meaning "everything", so a caller that computed an empty rect
//     hears about it.
//   * An out-of-bounds region is an ERROR at the readback, never a silent clamp:
//     a quietly shrunk rect makes a 1:1 measurement report the wrong spatial
//     period without ever saying so — precisely the failure this argument exists
//     to prevent.
//
// This header is renderer/editor/httplib-free (nlohmann::json + the schema DSL
// only), so the MCP test binary can include it directly — the same split
// McpRenderOverrides.h / McpRendererSettings.h use. McpServer.h includes it for
// the EditorMcpContext::CaptureViewportPng signature.

#include "MCP/McpSchemaBuilder.h"

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <string>

namespace OloEngine::MCP
{
    // A sub-rectangle of a capture, in top-left-origin pixel/texel coordinates of
    // the source. A zero Width or Height means "the whole image" — the default,
    // and what every pre-#607 caller gets.
    struct McpCaptureRegion
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Width = 0;
        u32 Height = 0;

        [[nodiscard]] bool IsWholeImage() const
        {
            return Width == 0 || Height == 0;
        }

        [[nodiscard]] bool operator==(const McpCaptureRegion&) const = default;
    };

    namespace CaptureRegionArg
    {
        using Json = nlohmann::json;

        // The `region` property's schema node. Identical wording everywhere it is
        // offered so the tools describe one concept, not three. (Named
        // `SchemaNode`, not `Schema`, so it does not shadow the Schema namespace
        // its own body uses.)
        [[nodiscard]] inline MCP::Schema::Node SchemaNode()
        {
            return MCP::Schema::Object()
                .Prop("x", MCP::Schema::Int().Min(0).Desc("Left edge in source pixels/texels, top-left origin."))
                .Prop("y", MCP::Schema::Int().Min(0).Desc("Top edge in source pixels/texels, top-left origin."))
                .Prop("w", MCP::Schema::Int().Min(1).Desc("Region width in source pixels/texels."))
                .Prop("h", MCP::Schema::Int().Min(1).Desc("Region height in source pixels/texels."))
                .Required({ "x", "y", "w", "h" })
                .NoAdditional()
                .Desc("Capture only this sub-rectangle, at NATIVE resolution (top-left origin, source "
                      "pixels/texels). The maxWidth downscale then applies to the region, so a region "
                      "narrower than maxWidth comes back 1:1 — the only way to measure a pixel-scale "
                      "artifact (noise period, dither pattern, aliasing) on a target larger than 4096. "
                      "A region outside the source is an error, never a silent clamp. Omit for the "
                      "whole target.");
        }

        // Parse an optional `region` argument. Absent / null leaves `out` at the
        // whole-image default and returns no error. Returns a human-readable error
        // for a malformed or degenerate rect; bounds against the actual source are
        // checked at readback time, where the source dimensions are known.
        [[nodiscard]] inline std::optional<std::string> Parse(const Json& args, McpCaptureRegion& out,
                                                              const char* key = "region")
        {
            if (!args.contains(key) || args[key].is_null())
                return std::nullopt;

            const Json& region = args[key];
            if (!region.is_object())
                return std::string("Invalid '") + key + "': expected an object { x, y, w, h }.";
            for (const char* field : { "x", "y", "w", "h" })
            {
                if (!region.contains(field) || !region[field].is_number_integer())
                    return std::string("Invalid '") + key + "': '" + field +
                           "' is missing or not an integer (expected { x, y, w, h } in source pixels/texels, "
                           "top-left origin).";
            }

            // Bound-check BEFORE narrowing to u32. A value above the u32 range
            // wraps on the cast, and a wrapped Width/Height of 0 reads as
            // IsWholeImage() — silently turning a bogus request into a
            // full-target capture the caller then mis-measures, which is exactly
            // the failure this argument exists to prevent. `is_number_integer()`
            // accepts a JSON *unsigned* too, so check that representation on its
            // own terms rather than round-tripping it through a signed get<>.
            constexpr u64 kMaxCoord = std::numeric_limits<u32>::max();
            for (const char* field : { "x", "y", "w", "h" })
            {
                const Json& component = region[field];
                const bool tooLarge = component.is_number_unsigned()
                                          ? component.get<u64>() > kMaxCoord
                                          : component.get<long long>() > static_cast<long long>(kMaxCoord);
                if (tooLarge)
                    return std::string("Invalid '") + key + "': '" + field +
                           "' exceeds the maximum texel coordinate (" + std::to_string(kMaxCoord) + ").";
            }

            const long long x = region["x"].get<long long>();
            const long long y = region["y"].get<long long>();
            const long long w = region["w"].get<long long>();
            const long long h = region["h"].get<long long>();
            if (x < 0 || y < 0)
                return std::string("Invalid '") + key + "': x and y must be >= 0.";
            // A zero/negative extent is rejected rather than folded into "whole
            // image": a caller that computed an empty rect should hear about it, not
            // silently receive a full-target capture it will then mis-measure.
            if (w <= 0 || h <= 0)
                return std::string("Invalid '") + key + "': w and h must be > 0 (omit 'region' for the whole target).";

            out.X = static_cast<u32>(x);
            out.Y = static_cast<u32>(y);
            out.Width = static_cast<u32>(w);
            out.Height = static_cast<u32>(h);
            return std::nullopt;
        }

        // Echo of what was actually read, for a capture reply's meta block. The
        // caller passes the region the readback resolved (the whole mip when none
        // was requested) plus the encoded output size, so `nativeResolution` states
        // plainly whether the PNG is 1:1 — the single fact a measurement depends on.
        [[nodiscard]] inline Json MetaJson(const McpCaptureRegion& region, u32 outWidth, u32 outHeight)
        {
            return Json{
                { "x", region.X },
                { "y", region.Y },
                { "w", region.Width },
                { "h", region.Height },
                { "nativeResolution", outWidth == region.Width && outHeight == region.Height },
            };
        }
    } // namespace CaptureRegionArg
} // namespace OloEngine::MCP
