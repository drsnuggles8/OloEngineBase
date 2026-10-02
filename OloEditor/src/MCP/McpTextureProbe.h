#pragma once

// Request parsing, read planning and result shaping for the `olo_texture_probe`
// MCP tool (issue #607, gap logged from #1078): read texels of an ASSET
// Texture2D. The capture tools only reach render-graph resources, so until this
// existed nothing could answer "what did the loader actually put on the GPU?".
//
// The handler in McpToolsResources.cpp resolves the asset, reads the region
// through RenderCommand::ReadTextureSubImage (the backend-neutral readback
// spine, so a 4K texture costs the region's bytes, not 64 MB) and hands the
// floats here. Everything that decides what the response SAYS lives in this
// header and is unit-tested (McpTextureProbeTest):
//
//   * Block-compressed formats are REFUSED, by name. Neither backend's readback
//     decodes BCn, and an all-zero answer would read as a black texture.
//   * The response states which value it reports. An 8-bit texel is read as the
//     stored, sRGB-ENCODED value on both backends (glGetTextureSubImage and
//     vkCmdCopyImageToBuffer perform no sRGB decode), so each texel carries the
//     raw byte, the encoded [0,1] value and, for an sRGB texture, the decoded
//     linear value a shader would sample. Nothing is left to guess.
//   * The region is bounded (kMaxTexels) and validated against the mip, so an
//     out-of-range read is an error rather than a clamp.
//   * Rows: every file-backed Texture2D loader flips vertically on load, so
//     storage row 0 is the image's BOTTOM row. origin:"image" (the default for
//     a file-backed texture) addresses the source file top-left, the way an
//     image viewer does; origin:"storage" addresses rows as uploaded.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ColorTransfer.h"
#include "OloEngine/Renderer/Texture.h"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace OloEngine::MCP::TextureProbe
{
    using Json = nlohmann::json;

    // 16 x 16. A probe is for checking values, not for downloading an image;
    // olo_render_capture_target is the tool for pictures.
    inline constexpr u32 kMaxTexels = 256;

    enum class Origin : u8
    {
        Image,  // top-left of the source image (rows flipped back)
        Storage // rows as uploaded: row 0 is the first row in memory
    };

    struct Request
    {
        u64 Handle = 0;
        std::string Path;
        u32 Mip = 0;
        u32 X = 0;
        u32 Y = 0;
        u32 Width = 1;
        u32 Height = 1;
        std::optional<Origin> RequestedOrigin; // absent: decided by the texture
    };

    [[nodiscard]] inline std::optional<u64> ParseHandle(const Json& value)
    {
        if (value.is_number_unsigned())
            return value.get<u64>();
        if (value.is_number_integer())
        {
            const auto signedValue = value.get<i64>();
            return signedValue >= 0 ? std::optional<u64>(static_cast<u64>(signedValue)) : std::nullopt;
        }
        if (value.is_string())
        {
            const auto text = value.get<std::string>();
            u64 parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error == std::errc{} && end == text.data() + text.size() && !text.empty())
                return parsed;
        }
        return std::nullopt;
    }

    // Reads an optional non-negative integer argument. Returns an error string
    // for a present-but-invalid value, so a typo never silently becomes 0.
    [[nodiscard]] inline std::optional<std::string> ReadU32(const Json& args, const char* key, u32 minimum, u32& out)
    {
        if (!args.contains(key))
            return std::nullopt;
        const Json& value = args[key];
        if (!value.is_number_integer() || value.get<i64>() < static_cast<i64>(minimum) ||
            value.get<i64>() > static_cast<i64>(0x7FFFFFFF))
            return std::string("Invalid '") + key + "': expected an integer >= " + std::to_string(minimum) + ".";
        out = static_cast<u32>(value.get<i64>());
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, Request& out)
    {
        const bool hasHandle = args.contains("handle");
        const bool hasPath = args.contains("path");
        if (hasHandle == hasPath)
            return "Pass exactly one of 'handle' (a Texture2D asset handle) or 'path' (the texture file, relative to the "
                   "project's asset directory).";
        if (hasHandle)
        {
            const auto handle = ParseHandle(args["handle"]);
            if (!handle || *handle == 0)
                return "Invalid 'handle': expected a non-zero asset handle (a decimal string such as \"1234567890\").";
            out.Handle = *handle;
        }
        else
        {
            if (!args["path"].is_string() || args["path"].get<std::string>().empty())
                return "Invalid 'path': expected a non-empty file path.";
            out.Path = args["path"].get<std::string>();
        }

        for (const auto& [key, minimum, field] : { std::tuple{ "mip", 0u, &out.Mip }, std::tuple{ "x", 0u, &out.X },
                                                   std::tuple{ "y", 0u, &out.Y }, std::tuple{ "w", 1u, &out.Width },
                                                   std::tuple{ "h", 1u, &out.Height } })
        {
            if (auto error = ReadU32(args, key, minimum, *field))
                return error;
        }
        if (static_cast<u64>(out.Width) * out.Height > kMaxTexels)
            return "Region " + std::to_string(out.Width) + "x" + std::to_string(out.Height) + " is " +
                   std::to_string(static_cast<u64>(out.Width) * out.Height) + " texels; the probe reads at most " +
                   std::to_string(kMaxTexels) + " (for example 16x16). Use olo_render_capture_target for an image.";

        if (args.contains("origin"))
        {
            const Json& origin = args["origin"];
            if (origin == "image")
                out.RequestedOrigin = Origin::Image;
            else if (origin == "storage")
                out.RequestedOrigin = Origin::Storage;
            else
                return "Invalid 'origin': expected \"image\" or \"storage\".";
        }
        return std::nullopt;
    }

    // ---- read planning -------------------------------------------------------

    enum class ValueKind : u8
    {
        UNorm8, // stored bytes; reported raw + encoded (+ linear when sRGB)
        Float,  // half / float storage; reported as the linear value
        Int     // single-channel integer storage; reported as an integer
    };

    struct ReadPlan
    {
        bool Ok = false;
        std::string Refusal; // set when !Ok
        ValueKind Kind = ValueKind::Float;
        u32 Channels = 4;     // channels that mean something
        u32 ReadChannels = 4; // components the readback writes per texel (1, 2 or 4)
    };

    [[nodiscard]] inline const char* FormatToken(ImageFormat format) noexcept
    {
        switch (format)
        {
            case ImageFormat::None:
                return "None";
            case ImageFormat::R8:
                return "R8";
            case ImageFormat::R8UI:
                return "R8UI";
            case ImageFormat::R16UI:
                return "R16UI";
            case ImageFormat::RG16UI:
                return "RG16UI";
            case ImageFormat::RGB8:
                return "RGB8";
            case ImageFormat::RGBA8:
                return "RGBA8";
            case ImageFormat::RGBA16F:
                return "RGBA16F";
            case ImageFormat::RGBA32F:
                return "RGBA32F";
            case ImageFormat::R32F:
                return "R32F";
            case ImageFormat::RG32F:
                return "RG32F";
            case ImageFormat::RGB32F:
                return "RGB32F";
            case ImageFormat::DEPTH24STENCIL8:
                return "DEPTH24STENCIL8";
            case ImageFormat::RG16F:
                return "RG16F";
            case ImageFormat::R32I:
                return "R32I";
            case ImageFormat::RG8:
                return "RG8";
            case ImageFormat::BC7:
                return "BC7";
            case ImageFormat::BC5:
                return "BC5";
            case ImageFormat::BC6H:
                return "BC6H";
            case ImageFormat::R32UI:
                return "R32UI";
            case ImageFormat::BC6HS:
                return "BC6HS";
            case ImageFormat::RGBA32UI:
                return "RGBA32UI";
            case ImageFormat::BC4:
                return "BC4";
        }
        return "Unknown";
    }

    [[nodiscard]] inline ReadPlan PlanRead(ImageFormat format)
    {
        ReadPlan plan;
        const auto accept = [&plan](ValueKind kind, u32 channels)
        {
            plan.Ok = true;
            plan.Kind = kind;
            plan.Channels = channels;
            plan.ReadChannels = channels <= 2 ? channels : 4;
            return plan;
        };
        // The engine's own predicates decide the two refusals, so a format added
        // to either family is refused here without touching this function.
        if (IsCompressedFormat(format))
        {
            plan.Refusal = std::string("The texture is block-compressed (") + FormatToken(format) +
                           "). Neither backend's readback decodes BCn blocks, and returning their bytes as texels "
                           "would read as garbage or black. Probe the loose source texture instead, or capture a "
                           "render target that samples it.";
            return plan;
        }
        if (IsIntegerFormat(format))
        {
            if (format == ImageFormat::R32I || format == ImageFormat::R32UI)
                return accept(ValueKind::Int, 1);
            plan.Refusal = std::string("Integer format ") + FormatToken(format) +
                           " has no readback destination on the backend-neutral spine (only single-channel 32-bit "
                           "integers do).";
            return plan;
        }
        switch (format)
        {
            case ImageFormat::R8:
                return accept(ValueKind::UNorm8, 1);
            case ImageFormat::RG8:
                return accept(ValueKind::UNorm8, 2);
            case ImageFormat::RGB8:
                return accept(ValueKind::UNorm8, 3);
            case ImageFormat::RGBA8:
                return accept(ValueKind::UNorm8, 4);
            case ImageFormat::R32F:
                return accept(ValueKind::Float, 1);
            case ImageFormat::RG16F:
            case ImageFormat::RG32F:
                return accept(ValueKind::Float, 2);
            case ImageFormat::RGB32F:
                return accept(ValueKind::Float, 3);
            case ImageFormat::RGBA16F:
            case ImageFormat::RGBA32F:
                return accept(ValueKind::Float, 4);
            case ImageFormat::DEPTH24STENCIL8:
                plan.Refusal = "A depth-stencil texture is not an asset texture; probe it as a render target with "
                               "olo_render_probe_pixel.";
                return plan;
            default:
                break;
        }
        plan.Refusal = std::string("The texture has no storage format this probe can read (") + FormatToken(format) + ").";
        return plan;
    }

    // ---- region mapping ------------------------------------------------------

    struct MappedRegion
    {
        bool Ok = false;
        std::string Error;
        u32 StorageX = 0;
        u32 StorageY = 0; // first STORAGE row the readback starts at
    };

    // The read is always a storage-space rectangle; only which rows it covers
    // and the order texels are reported in depend on the origin.
    [[nodiscard]] inline MappedRegion MapRegion(const Request& request, Origin origin, u32 mipWidth, u32 mipHeight)
    {
        MappedRegion mapped;
        if (static_cast<u64>(request.X) + request.Width > mipWidth ||
            static_cast<u64>(request.Y) + request.Height > mipHeight)
        {
            mapped.Error = "Region x=" + std::to_string(request.X) + " y=" + std::to_string(request.Y) + " w=" +
                           std::to_string(request.Width) + " h=" + std::to_string(request.Height) +
                           " does not fit mip " + std::to_string(request.Mip) + " (" + std::to_string(mipWidth) + "x" +
                           std::to_string(mipHeight) + ").";
            return mapped;
        }
        mapped.Ok = true;
        mapped.StorageX = request.X;
        mapped.StorageY = origin == Origin::Image ? mipHeight - request.Y - request.Height : request.Y;
        return mapped;
    }

    // ---- value shaping -------------------------------------------------------

    struct Texture
    {
        ImageFormat Format = ImageFormat::None;
        bool SRGB = false;
        u32 Width = 0; // mip 0
        u32 Height = 0;
        u32 MipLevels = 1;
        std::string StorageFormat; // the backend's own token for the storage
        std::string Path;          // empty for a memory-only texture
        u64 Handle = 0;
        std::string Backend;
    };

    // `floats` holds ReadChannels components per texel in STORAGE row order
    // (row StorageY first), the layout ReadTextureSubImage writes; `ints` the
    // same for an Int plan.
    [[nodiscard("this builds the response; it does not send it")]] inline Json
    BuildReport(const Texture& texture, const Request& request, const ReadPlan& plan, Origin origin, u32 mipWidth,
                u32 mipHeight, const std::vector<f32>& floats, const std::vector<i32>& ints)
    {
        Json out;
        out["handle"] = std::to_string(texture.Handle);
        out["path"] = texture.Path;
        out["backend"] = texture.Backend;
        out["format"] = FormatToken(texture.Format);
        out["storageFormat"] = texture.StorageFormat;
        out["srgb"] = texture.SRGB;
        out["width"] = texture.Width;
        out["height"] = texture.Height;
        out["mipLevels"] = texture.MipLevels;
        out["mip"] = request.Mip;
        out["mipWidth"] = mipWidth;
        out["mipHeight"] = mipHeight;
        out["origin"] = origin == Origin::Image ? "image" : "storage";
        out["region"] = Json{ { "x", request.X }, { "y", request.Y }, { "w", request.Width }, { "h", request.Height } };
        out["channels"] = plan.Channels;

        switch (plan.Kind)
        {
            case ValueKind::UNorm8:
                out["valueKind"] = "unorm8";
                out["reads"] = texture.SRGB
                                   ? "raw: the stored byte; encoded: raw/255, the sRGB-encoded value as stored; "
                                     "linear: the sRGB decode a shader's sampler applies"
                                   : "raw: the stored byte; encoded: raw/255, which is also what a shader samples "
                                     "(the texture is not sRGB, so there is no decode)";
                break;
            case ValueKind::Float:
                out["valueKind"] = "float";
                out["reads"] = "value: the stored float (half-float storage widened to f32); linear, no decode";
                break;
            case ValueKind::Int:
                out["valueKind"] = "int";
                out["reads"] = texture.Format == ImageFormat::R32UI
                                   ? "value: the stored 32-bit unsigned integer"
                                   : "value: the stored 32-bit signed integer";
                break;
        }

        Json texels = Json::array();
        for (u32 row = 0; row < request.Height; ++row)
        {
            // Report in the caller's row order. Image origin reads storage
            // bottom-up, so the first reported row is the LAST storage row read.
            const u32 storageRow = origin == Origin::Image ? request.Height - 1 - row : row;
            for (u32 column = 0; column < request.Width; ++column)
            {
                const sizet index = static_cast<sizet>(storageRow) * request.Width + column;
                Json texel;
                texel["x"] = request.X + column;
                texel["y"] = request.Y + row;
                if (plan.Kind == ValueKind::Int)
                {
                    const i32 value = ints[index];
                    if (texture.Format == ImageFormat::R32UI)
                        texel["value"] = static_cast<u32>(value);
                    else
                        texel["value"] = value;
                    texels.push_back(std::move(texel));
                    continue;
                }

                Json raw = Json::array();
                Json encoded = Json::array();
                Json linear = Json::array();
                Json value = Json::array();
                for (u32 c = 0; c < plan.Channels; ++c)
                {
                    const f32 component = floats[index * plan.ReadChannels + c];
                    if (plan.Kind == ValueKind::Float)
                    {
                        value.push_back(component);
                        continue;
                    }
                    raw.push_back(static_cast<int>(std::lround(component * 255.0f)));
                    encoded.push_back(component);
                    // Alpha is never sRGB-encoded.
                    if (texture.SRGB)
                        linear.push_back(c == 3 ? component : ColorTransfer::SrgbToLinear(component));
                }
                if (plan.Kind == ValueKind::Float)
                {
                    texel["value"] = std::move(value);
                }
                else
                {
                    texel["raw"] = std::move(raw);
                    texel["encoded"] = std::move(encoded);
                    if (texture.SRGB)
                        texel["linear"] = std::move(linear);
                }
                texels.push_back(std::move(texel));
            }
        }
        out["texels"] = std::move(texels);
        return out;
    }
} // namespace OloEngine::MCP::TextureProbe
