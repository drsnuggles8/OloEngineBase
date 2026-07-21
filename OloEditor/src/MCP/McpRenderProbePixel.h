#pragma once

// Pure, engine-light decode + JSON shaping for the olo_render_probe_pixel MCP
// tool (issue #607): the NUMERIC per-pixel readout of the G-Buffer.
//
// The motivating failure: a G-Buffer bug that a screenshot cannot show and a
// contract test cannot see — the frame "looks wrong", so an agent starts
// patching shader source, an hour disappears, and the actual defect was one
// mis-packed channel. Reading back the exact numbers under one pixel — albedo,
// metallic, the DECODED world-space normal, roughness, AO, emissive, velocity,
// entity id, raw + linearized depth, and the final tonemapped colour — turns
// that guessing loop into a single lookup.
//
// The handler in McpToolsRender.cpp does the GL-bound work on the main thread
// (resolve each render-graph target to a texture, glGetTextureSubImage a 1x1
// region out of it) and hands the raw texels here. Everything below is free
// functions over PODs with NO GL / renderer / editor dependency, so the decode
// contract (the octahedral normal unpack, the depth linearization, the
// "channel unavailable" degradation) is unit-tested headlessly —
// the same split McpGoldenCompare.h / McpShaderReload.h use.
//
// Only Core/Base.h (the u32/f32/... typedefs), glm and nlohmann::json are
// pulled in; everything else is the standard library.

#include "OloEngine/Core/Base.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::ProbePixel
{
    using Json = nlohmann::json;

    // How a probed texel's channels are valued. Integer targets (the R32I
    // entity-id attachment) must not be reported as floats — an agent
    // comparing an entity id against a scene entity needs the exact integer.
    enum class SampleKind : u8
    {
        Float = 0,
        Int = 1
    };

    // ---- Coordinate mapping (issue #607: space:"texel" + mappedCoord) -----
    //
    // The GTAO hunt's motivating failure: probing the HZB pow2 pyramid with
    // viewport coordinates silently read the wrong texel, because the tool
    // applied the requested x/y RAW to a resource whose size differs from the
    // viewport. Two explicit spaces replace that guesswork, and every probe
    // reply now echoes the exact texel it read (mappedCoord).
    enum class ProbeSpace : u8
    {
        // x/y are viewport pixels (top-left origin, the screenshot
        // convention), mapped PROPORTIONALLY onto the target mip:
        // texel = floor((coord + 0.5) / refSize * mipSize). Identity for a
        // full-res mip-0 target (the G-Buffer), "same visual location" for a
        // half-res AO buffer. WRONG for padded resources (the HZB pyramid,
        // where content sits 1:1 in a pow2-padded extent) — use Texel there.
        Viewport = 0,
        // x/y are EXACT texel coordinates of the target at the requested mip,
        // top-left origin — the same orientation as a capture PNG of that
        // resource+mip, so a pixel picked off a capture image addresses the
        // texel the agent actually pointed at. No scaling of any kind.
        Texel = 1,
    };

    [[nodiscard]] inline const char* ProbeSpaceToken(ProbeSpace space) noexcept
    {
        return space == ProbeSpace::Texel ? "texel" : "viewport";
    }

    [[nodiscard]] inline bool ParseProbeSpace(std::string_view token, ProbeSpace& out) noexcept
    {
        if (token == "viewport")
            out = ProbeSpace::Viewport;
        else if (token == "texel")
            out = ProbeSpace::Texel;
        else
            return false;
        return true;
    }

    // The resolved texel a probe actually read — echoed in every reply so the
    // requested-coordinate -> texel mapping is never guesswork.
    struct CoordMapping
    {
        bool Valid = false;
        std::string Error; // set when !Valid (out of bounds / bad mip / degenerate reference)
        ProbeSpace Space = ProbeSpace::Viewport;
        u32 RequestedX = 0; // the caller's x/y, in `Space`
        u32 RequestedY = 0;
        u32 TexelX = 0;        // texel column at `Mip`, top-left origin
        u32 TexelY = 0;        // texel row at `Mip`, top-left origin
        u32 GLRowBottomUp = 0; // the GL row actually read (GL rows run bottom-up)
        u32 Mip = 0;
        u32 MipWidth = 0;
        u32 MipHeight = 0;
    };

    // Map a requested (x, y) in `space` onto an exact texel of a `mipWidth` x
    // `mipHeight` mip level. `refWidth`/`refHeight` are the viewport-space
    // reference dimensions (the render size the screenshot shows); unused in
    // texel space. Pure math — unit-tested headlessly.
    [[nodiscard]] inline CoordMapping MapProbeCoord(ProbeSpace space, u32 x, u32 y,
                                                    u32 refWidth, u32 refHeight,
                                                    u32 mipWidth, u32 mipHeight, u32 mip) noexcept
    {
        CoordMapping m;
        m.Space = space;
        m.RequestedX = x;
        m.RequestedY = y;
        m.Mip = mip;
        m.MipWidth = mipWidth;
        m.MipHeight = mipHeight;

        if (mipWidth == 0 || mipHeight == 0)
        {
            m.Error = "The target has no storage at mip " + std::to_string(mip) + ".";
            return m;
        }

        if (space == ProbeSpace::Texel)
        {
            if (x >= mipWidth || y >= mipHeight)
            {
                m.Error = "Texel (" + std::to_string(x) + ", " + std::to_string(y) + ") is outside mip " +
                          std::to_string(mip) + " (" + std::to_string(mipWidth) + "x" + std::to_string(mipHeight) +
                          ").";
                return m;
            }
            m.TexelX = x;
            m.TexelY = y;
        }
        else
        {
            if (refWidth == 0 || refHeight == 0)
            {
                m.Error = "The viewport reference size is unknown; probe with space:\"texel\" instead.";
                return m;
            }
            if (x >= refWidth || y >= refHeight)
            {
                m.Error = "Viewport pixel (" + std::to_string(x) + ", " + std::to_string(y) +
                          ") is outside the viewport (" + std::to_string(refWidth) + "x" +
                          std::to_string(refHeight) + ").";
                return m;
            }
            // Pixel-centre proportional mapping; clamped so the last viewport
            // row/column never rounds past the mip edge.
            const f64 u = (static_cast<f64>(x) + 0.5) / static_cast<f64>(refWidth);
            const f64 v = (static_cast<f64>(y) + 0.5) / static_cast<f64>(refHeight);
            m.TexelX = std::min(static_cast<u32>(u * static_cast<f64>(mipWidth)), mipWidth - 1u);
            m.TexelY = std::min(static_cast<u32>(v * static_cast<f64>(mipHeight)), mipHeight - 1u);
        }

        m.GLRowBottomUp = mipHeight - 1u - m.TexelY;
        m.Valid = true;
        return m;
    }

    [[nodiscard]] inline Json CoordMappingJson(const CoordMapping& m)
    {
        Json j;
        j["space"] = ProbeSpaceToken(m.Space);
        j["requested"] = Json::array({ m.RequestedX, m.RequestedY });
        if (m.Valid)
        {
            j["texel"] = Json::array({ m.TexelX, m.TexelY });
            j["glRowBottomUp"] = m.GLRowBottomUp;
        }
        j["mip"] = m.Mip;
        j["mipWidth"] = m.MipWidth;
        j["mipHeight"] = m.MipHeight;
        j["origin"] = "top-left";
        return j;
    }

    // One 1x1 readback of one render-graph target. `Available` false means the
    // target did not exist this frame (wrong rendering path, effect disabled,
    // no frame rendered yet) — `Unavailable` then carries the reason and the
    // tool reports it instead of failing the whole call.
    struct TexelSample
    {
        bool Available = false;
        std::string Target;      // render-graph resource name it was read from
        std::string Format;      // GL internal-format token ("RGBA8", "RGBA16F", "R32I", "DEPTH32F", ...)
        std::string Unavailable; // why it is missing (empty when Available)
        SampleKind Kind = SampleKind::Float;
        i32 Channels = 0;
        std::array<f32, 4> F{ 0.0f, 0.0f, 0.0f, 0.0f };
        std::array<i32, 4> I{ 0, 0, 0, 0 };
        u32 SourceWidth = 0; // dims of the mip actually probed
        u32 SourceHeight = 0;
        // The exact texel this sample read (issue #607) — echoed in the reply
        // as "mappedCoord" so the requested-coord -> texel mapping is never
        // guesswork. Layer is the GL z-offset applied (a CSM cascade view's
        // own layer, or the caller's layer selector).
        CoordMapping Mapped;
        u32 Layer = 0;
    };

    // Octahedral decode — the EXACT inverse of octEncodeGB() in PBR_GBuffer.glsl
    // (and the octDecode() the deferred/SSAO consumers use). RT1.xy holds the
    // encoded pair; anything else here would silently report a wrong normal,
    // which is precisely the class of bug this tool exists to catch.
    [[nodiscard]] inline glm::vec3 OctDecodeGB(f32 x, f32 y) noexcept
    {
        glm::vec3 n(x, y, 1.0f - std::abs(x) - std::abs(y));
        const f32 t = std::max(-n.z, 0.0f);
        n.x += (n.x >= 0.0f) ? -t : t;
        n.y += (n.y >= 0.0f) ? -t : t;
        const f32 lengthSq = glm::dot(n, n);
        if (lengthSq < 1e-12f)
            return glm::vec3(0.0f, 0.0f, 1.0f); // degenerate encode (never written by the shader)
        return n / std::sqrt(lengthSq);
    }

    // Device depth (the [0,1] window-space value glGetTextureSubImage yields for
    // a GL_DEPTH_COMPONENT read) -> positive view-space distance, for the usual
    // GL perspective projection with the [-1,1] NDC z convention. Returns
    // `farClip` for a cleared background depth of 1.0 and 0 for degenerate clip
    // planes, so a caller never divides by zero or reports an infinity.
    [[nodiscard]] inline f32 LinearizeDepth(f32 deviceDepth, f32 nearClip, f32 farClip) noexcept
    {
        if (!(farClip > nearClip) || nearClip <= 0.0f)
            return 0.0f;
        const f32 ndc = deviceDepth * 2.0f - 1.0f;
        const f32 denominator = (farClip + nearClip) - ndc * (farClip - nearClip);
        if (std::abs(denominator) < 1e-9f)
            return farClip;
        return (2.0f * nearClip * farClip) / denominator;
    }

    // Everything the handler gathered for one pixel. A sample left Available=false
    // degrades that channel to `{ "available": false, "reason": ... }` rather than
    // failing the call — the Forward / Forward+ paths have no G-Buffer at all, and
    // that must still be a useful answer, not an error.
    struct GBufferProbeInput
    {
        u32 X = 0; // viewport pixel, top-left origin (the screenshot convention)
        u32 Y = 0;
        std::string RenderingPath; // "Deferred" / "Forward" / "Forward+"
        bool CameraKnown = false;
        f32 NearClip = 0.0f;
        f32 FarClip = 0.0f;

        TexelSample Albedo;     // RT0 RGBA8   — albedo.rgb + metallic.a
        TexelSample Normal;     // RT1 RGBA16F — octNormal.xy + roughness.z + ao.w
        TexelSample Emissive;   // RT2 RGBA16F — emissive.rgb + flags.a
        TexelSample Velocity;   // RT3 RG16F   — screen-space motion vector
        TexelSample EntityId;   // RT4 R32I    — picking id
        TexelSample Depth;      // depth attachment
        TexelSample FinalColor; // the post-tonemap chain output actually presented
    };

    namespace Detail
    {
        // A present channel: value + the exact target.component it came from, so
        // an agent can go capture that target if the number looks wrong.
        [[nodiscard]] inline Json Present(const TexelSample& sample, std::string_view component, Json value)
        {
            Json j;
            j["available"] = true;
            j["source"] = sample.Target + "." + std::string(component);
            j["format"] = sample.Format;
            j["value"] = std::move(value);
            return j;
        }

        [[nodiscard]] inline Json Missing(const TexelSample& sample, std::string_view fallbackReason)
        {
            Json j;
            j["available"] = false;
            j["reason"] = sample.Unavailable.empty() ? std::string(fallbackReason) : sample.Unavailable;
            if (!sample.Target.empty())
                j["target"] = sample.Target;
            return j;
        }

        [[nodiscard]] inline Json Vec(const f32* values, i32 count)
        {
            Json a = Json::array();
            for (i32 i = 0; i < count; ++i)
                a.push_back(values[static_cast<std::size_t>(i)]);
            return a;
        }
    } // namespace Detail

    // The raw (undecoded) texels, echoed alongside the decoded channels so a
    // suspect decode can be checked against the bytes it came from.
    [[nodiscard]] inline Json RawSampleJson(const TexelSample& sample)
    {
        Json j;
        j["available"] = sample.Available;
        j["target"] = sample.Target;
        // The exact texel read (or the mapping failure) — issue #607. Present
        // even for unavailable samples whenever a mapping was attempted, so
        // an out-of-bounds probe shows WHERE it would have read.
        if (sample.Mapped.MipWidth > 0 || sample.Mapped.MipHeight > 0 || sample.Mapped.Valid)
        {
            Json mapped = CoordMappingJson(sample.Mapped);
            if (sample.Layer != 0)
                mapped["layer"] = sample.Layer;
            j["mappedCoord"] = std::move(mapped);
        }
        if (!sample.Available)
        {
            j["reason"] = sample.Unavailable;
            return j;
        }
        j["format"] = sample.Format;
        j["width"] = sample.SourceWidth;
        j["height"] = sample.SourceHeight;
        if (sample.Kind == SampleKind::Int)
        {
            Json a = Json::array();
            for (i32 i = 0; i < sample.Channels; ++i)
                a.push_back(sample.I[static_cast<std::size_t>(i)]);
            j["value"] = std::move(a);
        }
        else
        {
            j["value"] = Detail::Vec(sample.F.data(), sample.Channels);
        }
        return j;
    }

    // Full decoded G-Buffer readout for one pixel.
    [[nodiscard]] inline Json BuildGBufferProbe(const GBufferProbeInput& in)
    {
        Json channels = Json::object();
        std::vector<std::string> unavailable;
        const auto note = [&unavailable](const char* name)
        { unavailable.emplace_back(name); };

        // RT0 — albedo.rgb (linear base colour as written) + metallic.a.
        if (in.Albedo.Available && in.Albedo.Channels >= 4)
        {
            channels["albedo"] = Detail::Present(in.Albedo, "rgb", Detail::Vec(in.Albedo.F.data(), 3));
            channels["metallic"] = Detail::Present(in.Albedo, "a", in.Albedo.F[3]);
        }
        else
        {
            channels["albedo"] = Detail::Missing(in.Albedo, "GBufferAlbedo (RT0) is not available on this rendering path.");
            channels["metallic"] = Detail::Missing(in.Albedo, "GBufferAlbedo (RT0) is not available on this rendering path.");
            note("albedo");
            note("metallic");
        }

        // RT1 — octahedral normal .xy, roughness .z, material AO .w.
        if (in.Normal.Available && in.Normal.Channels >= 4)
        {
            const glm::vec3 n = OctDecodeGB(in.Normal.F[0], in.Normal.F[1]);
            Json normal = Detail::Present(in.Normal, "xy (octahedral)",
                                          Json::array({ n.x, n.y, n.z }));
            normal["encoded"] = Json::array({ in.Normal.F[0], in.Normal.F[1] });
            normal["space"] = "world";
            channels["normal"] = std::move(normal);
            channels["roughness"] = Detail::Present(in.Normal, "z", in.Normal.F[2]);
            channels["ao"] = Detail::Present(in.Normal, "w", in.Normal.F[3]);
        }
        else
        {
            channels["normal"] = Detail::Missing(in.Normal, "GBufferNormal (RT1) is not available on this rendering path.");
            channels["roughness"] = Detail::Missing(in.Normal, "GBufferNormal (RT1) is not available on this rendering path.");
            channels["ao"] = Detail::Missing(in.Normal, "GBufferNormal (RT1) is not available on this rendering path.");
            note("normal");
            note("roughness");
            note("ao");
        }

        // RT2 — emissive.rgb (HDR) + reserved flags.a.
        if (in.Emissive.Available && in.Emissive.Channels >= 3)
        {
            channels["emissive"] = Detail::Present(in.Emissive, "rgb", Detail::Vec(in.Emissive.F.data(), 3));
            if (in.Emissive.Channels >= 4)
                channels["flags"] = Detail::Present(in.Emissive, "a", in.Emissive.F[3]);
        }
        else
        {
            channels["emissive"] = Detail::Missing(in.Emissive, "GBufferEmissive (RT2) is not available on this rendering path.");
            note("emissive");
        }

        // RT3 — screen-space velocity in [-1,1] NDC units.
        if (in.Velocity.Available && in.Velocity.Channels >= 2)
        {
            channels["velocity"] = Detail::Present(in.Velocity, "rg", Detail::Vec(in.Velocity.F.data(), 2));
        }
        else
        {
            channels["velocity"] = Detail::Missing(in.Velocity, "Velocity is not available on this rendering path.");
            note("velocity");
        }

        // RT4 — integer picking id (NOT a float; reported exactly).
        if (in.EntityId.Available && in.EntityId.Channels >= 1)
        {
            channels["entityID"] = Detail::Present(in.EntityId, "r", in.EntityId.I[0]);
        }
        else
        {
            channels["entityID"] = Detail::Missing(in.EntityId, "The entity-id attachment is not available this frame.");
            note("entityID");
        }

        // Depth — raw device value plus the linearized view-space distance.
        if (in.Depth.Available && in.Depth.Channels >= 1)
        {
            Json depth = Detail::Present(in.Depth, "r", in.Depth.F[0]);
            depth["device"] = in.Depth.F[0];
            if (in.CameraKnown)
            {
                depth["linearViewDepth"] = LinearizeDepth(in.Depth.F[0], in.NearClip, in.FarClip);
                depth["nearClip"] = in.NearClip;
                depth["farClip"] = in.FarClip;
            }
            else
            {
                depth["linearViewDepth"] = nullptr;
                depth["note"] = "The active camera's near/far clip is unknown, so the device depth could not be linearized.";
            }
            channels["depth"] = std::move(depth);
        }
        else
        {
            channels["depth"] = Detail::Missing(in.Depth, "SceneDepth is not available this frame.");
            note("depth");
        }

        // Final presented colour (post-tonemap), so the numbers above can be
        // related to what the screenshot shows at the same pixel.
        if (in.FinalColor.Available && in.FinalColor.Channels >= 3)
        {
            channels["finalColor"] = Detail::Present(in.FinalColor, "rgb", Detail::Vec(in.FinalColor.F.data(), 3));
        }
        else
        {
            channels["finalColor"] = Detail::Missing(in.FinalColor, "No post-tonemap colour target resolved this frame.");
            note("finalColor");
        }

        Json raw = Json::object();
        raw["GBufferAlbedo"] = RawSampleJson(in.Albedo);
        raw["GBufferNormal"] = RawSampleJson(in.Normal);
        raw["GBufferEmissive"] = RawSampleJson(in.Emissive);
        raw["Velocity"] = RawSampleJson(in.Velocity);
        raw["EntityID"] = RawSampleJson(in.EntityId);
        raw["Depth"] = RawSampleJson(in.Depth);
        raw["FinalColor"] = RawSampleJson(in.FinalColor);

        Json j;
        j["x"] = in.X;
        j["y"] = in.Y;
        j["origin"] = "top-left (same convention as olo_screenshot)";
        j["renderingPath"] = in.RenderingPath;
        j["channels"] = std::move(channels);
        j["raw"] = std::move(raw);
        j["unavailableChannels"] = unavailable;
        if (!unavailable.empty() && in.RenderingPath != "Deferred")
            j["note"] = "The G-Buffer channels only exist on the Deferred rendering path (current path: " +
                        in.RenderingPath +
                        "). Switch with olo_renderer_settings_set { setting: 'renderpath', value: 'deferred' }.";
        return j;
    }

    // Single-target probe (the optional `target` argument): the raw channel
    // values of one named render-graph resource, so the tool works for ANY
    // capturable target, not just the G-Buffer.
    [[nodiscard]] inline Json BuildRawProbe(const TexelSample& sample, u32 x, u32 y)
    {
        Json j = RawSampleJson(sample);
        j["x"] = x;
        j["y"] = y;
        j["origin"] = "top-left (same convention as olo_screenshot)";
        return j;
    }
} // namespace OloEngine::MCP::ProbePixel
