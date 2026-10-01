#pragma once

// Request parsing, buffer selection and decoding for the `olo_gpu_buffer_read`
// MCP tool (issue #607, gap logged from #1105): read a byte range of a live
// storage buffer.
//
// The handler in McpToolsResources.cpp lists the live buffers from
// StorageBufferRegistry, reads the range with StorageBuffer::GetData (a
// synchronous, fenced copy on both backends) and hands the bytes here. What is
// decided here, and pinned by McpGpuBufferReadTest:
//
//   * A name resolves through kStorageBindings, which takes its values from
//     ShaderBindingLayout itself; the test checks that every SSBO_* binding in
//     that header has an entry, so a new binding cannot be missing silently.
//   * Several live buffers can share a binding (one SSBO_GPU_PARTICLES per
//     particle system). Picking one would be a guess, so more than one candidate
//     without an 'id' is an error that lists them.
//   * A POOLED buffer (TransientPool) is created with a nominal binding it never
//     serves, so binding and name selection skip it; it is reachable by 'id'.
//   * An unknown name, a binding with no live buffer and a range past the end
//     are errors, never an empty or zero-filled read.
//   * lengthBytes is bounded (kMaxReadBytes) and must be a whole number of the
//     decoded element; a non-finite float is reported as a string, since JSON
//     has no NaN.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::GpuBufferRead
{
    using Json = nlohmann::json;

    inline constexpr u32 kMaxReadBytes = 4096;
    inline constexpr u32 kDefaultReadBytes = 256;

    struct NamedBinding
    {
        std::string_view Name;
        u32 Binding;
    };

    // Values come from ShaderBindingLayout, so only coverage can drift, and
    // McpGpuBufferReadTest.EveryStorageBindingHasAName pins that.
#define OLO_SSBO_ENTRY(name)             \
    NamedBinding                         \
    {                                    \
        #name, ShaderBindingLayout::name \
    }
    inline constexpr std::array kStorageBindings{
        OLO_SSBO_ENTRY(SSBO_GPU_PARTICLES),
        OLO_SSBO_ENTRY(SSBO_ALIVE_INDICES),
        OLO_SSBO_ENTRY(SSBO_COUNTERS),
        OLO_SSBO_ENTRY(SSBO_FREE_LIST),
        OLO_SSBO_ENTRY(SSBO_INDIRECT_DRAW),
        OLO_SSBO_ENTRY(SSBO_EMIT_STAGING),
        OLO_SSBO_ENTRY(SSBO_SNOW_DEFORMERS),
        OLO_SSBO_ENTRY(SSBO_LIGHT_PROBES),
        OLO_SSBO_ENTRY(SSBO_FPLUS_POINT_LIGHTS),
        OLO_SSBO_ENTRY(SSBO_FPLUS_SPOT_LIGHTS),
        OLO_SSBO_ENTRY(SSBO_FPLUS_LIGHT_INDICES),
        OLO_SSBO_ENTRY(SSBO_FPLUS_LIGHT_GRID),
        OLO_SSBO_ENTRY(SSBO_FPLUS_GLOBAL_INDEX),
        OLO_SSBO_ENTRY(SSBO_GPU_PARTICLES_PREV),
        OLO_SSBO_ENTRY(SSBO_INSTANCE_DATA),
        OLO_SSBO_ENTRY(SSBO_INSTANCE_CULL_INPUT),
        OLO_SSBO_ENTRY(SSBO_INSTANCE_DRAW_INDIRECT),
        OLO_SSBO_ENTRY(SSBO_FPLUS_SPHERE_AREA_LIGHTS),
        OLO_SSBO_ENTRY(SSBO_AUTO_EXPOSURE_HISTOGRAM),
        OLO_SSBO_ENTRY(SSBO_AUTO_EXPOSURE_STATE),
        OLO_SSBO_ENTRY(SSBO_FLUID_POSITIONS),
        OLO_SSBO_ENTRY(SSBO_FLUID_VELOCITIES),
        OLO_SSBO_ENTRY(SSBO_FLUID_PREDICTED_A),
        OLO_SSBO_ENTRY(SSBO_FLUID_PREDICTED_B),
        OLO_SSBO_ENTRY(SSBO_FLUID_AUX),
        OLO_SSBO_ENTRY(SSBO_FLUID_GRID_HEAD),
        OLO_SSBO_ENTRY(SSBO_FLUID_GRID_NEXT),
        OLO_SSBO_ENTRY(SSBO_FLUID_COUNTERS),
        OLO_SSBO_ENTRY(SSBO_FLUID_EMIT_STAGING),
        OLO_SSBO_ENTRY(SSBO_FLUID_BODY_PROXIES),
        OLO_SSBO_ENTRY(SSBO_FLUID_BODY_IMPULSES),
        OLO_SSBO_ENTRY(SSBO_FLUID_VELOCITIES_ALT),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_CLUSTERS),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_GROUPS),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_INSTANCES),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_DRAW_COMMANDS),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_DRAW_ARGS),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_VISIBLE),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_VERTICES),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_SW_LIST),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_VISBUFFER),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_INDICES),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_GROUP_STATES),
        OLO_SSBO_ENTRY(SSBO_VIRTUAL_REJECTED),
        OLO_SSBO_ENTRY(SSBO_RESOURCE_HEAP),
        OLO_SSBO_ENTRY(SSBO_DEBUG_DRAW_LINE),
        OLO_SSBO_ENTRY(SSBO_DEBUG_DRAW_CIRCLE),
        OLO_SSBO_ENTRY(SSBO_DEBUG_DRAW_RECTANGLE),
        OLO_SSBO_ENTRY(SSBO_DEBUG_DRAW_AABB),
        OLO_SSBO_ENTRY(SSBO_DEBUG_DRAW_BOX),
        OLO_SSBO_ENTRY(SSBO_DEBUG_DRAW_CONE),
        OLO_SSBO_ENTRY(SSBO_DEBUG_DRAW_SPHERE),
        OLO_SSBO_ENTRY(SSBO_REFLECTION_PROBE_GRID),
        OLO_SSBO_ENTRY(SSBO_VSM_PAGE_TABLE),
        OLO_SSBO_ENTRY(SSBO_VSM_META_TABLE),
        OLO_SSBO_ENTRY(SSBO_VSM_HPB),
        OLO_SSBO_ENTRY(SSBO_VSM_REQUESTS),
        OLO_SSBO_ENTRY(SSBO_VSM_FREE_PAGES),
        OLO_SSBO_ENTRY(SSBO_VSM_INVALIDATIONS),
        OLO_SSBO_ENTRY(SSBO_VSM_CULL_INSTANCES),
        OLO_SSBO_ENTRY(SSBO_VSM_DRAW_INSTANCES),
        OLO_SSBO_ENTRY(SSBO_VSM_DRAW_COMMANDS),
        OLO_SSBO_ENTRY(SSBO_VSM_STATS),
        OLO_SSBO_ENTRY(SSBO_VSM_LOCAL_LIGHTS),
        OLO_SSBO_ENTRY(SSBO_DDGI_PROBE_AUX),
        OLO_SSBO_ENTRY(SSBO_PREFIX_SUM_VALUES),
        OLO_SSBO_ENTRY(SSBO_PREFIX_SUM_BLOCK_SUMS),
        OLO_SSBO_ENTRY(SSBO_PREFIX_SUM_TOTAL),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_NODE_BOUNDS),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_NODE_LIST_IN),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_NODE_LIST_OUT),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_CULL_STATE),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_VISIBLE_NODES),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_SPLIT_MAP),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_LOD_MAP),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_DRAW_ARGS),
        OLO_SSBO_ENTRY(SSBO_TERRAIN_VT),
        OLO_SSBO_ENTRY(SSBO_GROOM_DEFORMATION),
        OLO_SSBO_ENTRY(SSBO_GPU_STATS),
        OLO_SSBO_ENTRY(SSBO_VERTEX_PULL),
        OLO_SSBO_ENTRY(SSBO_BONE_PULL),
    };
#undef OLO_SSBO_ENTRY

    // Accepts the constant's name with or without the SSBO_ prefix.
    [[nodiscard]] inline std::optional<u32> BindingForName(std::string_view name)
    {
        for (const auto& entry : kStorageBindings)
        {
            if (entry.Name == name || entry.Name.substr(5) == name)
                return entry.Binding;
        }
        return std::nullopt;
    }

    // Every name for a binding. Several constants can share one number
    // (SSBO_GROOM_DEFORMATION rides SSBO_TERRAIN_VT's slot).
    [[nodiscard]] inline std::vector<std::string> NamesForBinding(u32 binding)
    {
        std::vector<std::string> names;
        for (const auto& entry : kStorageBindings)
        {
            if (entry.Binding == binding)
                names.emplace_back(entry.Name);
        }
        return names;
    }

    enum class Format : u8
    {
        U32,
        I32,
        F32,
        Vec4,
        Hex
    };

    [[nodiscard]] inline u32 ElementBytes(Format format) noexcept
    {
        switch (format)
        {
            case Format::Vec4:
                return 16;
            case Format::Hex:
                return 1;
            case Format::U32:
            case Format::I32:
            case Format::F32:
                return 4;
        }
        return 1;
    }

    [[nodiscard]] inline const char* FormatToken(Format format) noexcept
    {
        switch (format)
        {
            case Format::U32:
                return "u32";
            case Format::I32:
                return "i32";
            case Format::F32:
                return "f32";
            case Format::Vec4:
                return "vec4";
            case Format::Hex:
                return "hex";
        }
        return "hex";
    }

    [[nodiscard]] inline const char* UsageToken(StorageBufferUsage usage) noexcept
    {
        switch (usage)
        {
            case StorageBufferUsage::DynamicDraw:
                return "dynamicDraw";
            case StorageBufferUsage::DynamicDrawExactUpload:
                return "dynamicDrawExactUpload";
            case StorageBufferUsage::DynamicCopy:
                return "dynamicCopy";
            case StorageBufferUsage::StreamCommandOrdered:
                return "streamCommandOrdered";
        }
        return "unknown";
    }

    struct Request
    {
        std::optional<u32> Binding; // from 'binding' or a resolved 'name'
        std::string Name;           // as given
        std::optional<u64> Id;
        u32 OffsetBytes = 0;
        std::optional<u32> LengthBytes; // absent: up to kDefaultReadBytes
        Format DecodeAs = Format::U32;
    };

    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, Request& out)
    {
        const bool hasBinding = args.contains("binding");
        const bool hasName = args.contains("name");
        const bool hasId = args.contains("id");
        if (hasBinding && hasName)
            return "Pass 'binding' or 'name', not both.";
        if (!hasBinding && !hasName && !hasId)
            return "Pass 'binding' (an SSBO binding number), 'name' (an SSBO_* constant from ShaderBindingLayout.h, "
                   "for example SSBO_FPLUS_LIGHT_GRID) or 'id' (from a previous reply's candidates).";

        if (hasBinding)
        {
            const Json& value = args["binding"];
            if (!value.is_number_integer() || value.get<i64>() < 0 || value.get<i64>() > 0xFFFF)
                return "Invalid 'binding': expected a non-negative integer.";
            out.Binding = static_cast<u32>(value.get<i64>());
        }
        if (hasName)
        {
            if (!args["name"].is_string())
                return "Invalid 'name': expected a string such as \"SSBO_FPLUS_LIGHT_GRID\".";
            out.Name = args["name"].get<std::string>();
            out.Binding = BindingForName(out.Name);
            if (!out.Binding)
            {
                std::string known;
                for (const auto& entry : kStorageBindings)
                {
                    if (!known.empty())
                        known += ", ";
                    known += entry.Name;
                }
                return "Unknown storage-buffer name '" + out.Name + "'. Known names: " + known + ".";
            }
        }
        if (hasId)
        {
            const Json& value = args["id"];
            if (!value.is_number_integer() || value.get<i64>() <= 0)
                return "Invalid 'id': expected a positive integer from a previous reply's 'candidates'.";
            out.Id = static_cast<u64>(value.get<i64>());
        }

        if (args.contains("offsetBytes"))
        {
            const Json& value = args["offsetBytes"];
            if (!value.is_number_integer() || value.get<i64>() < 0 || value.get<i64>() > 0x7FFFFFFF)
                return "Invalid 'offsetBytes': expected a non-negative integer.";
            out.OffsetBytes = static_cast<u32>(value.get<i64>());
        }
        if (args.contains("lengthBytes"))
        {
            const Json& value = args["lengthBytes"];
            if (!value.is_number_integer() || value.get<i64>() <= 0 || value.get<i64>() > kMaxReadBytes)
                return "Invalid 'lengthBytes': expected 1.." + std::to_string(kMaxReadBytes) + ".";
            out.LengthBytes = static_cast<u32>(value.get<i64>());
        }
        if (args.contains("format"))
        {
            const Json& value = args["format"];
            if (value == "u32")
                out.DecodeAs = Format::U32;
            else if (value == "i32")
                out.DecodeAs = Format::I32;
            else if (value == "f32")
                out.DecodeAs = Format::F32;
            else if (value == "vec4")
                out.DecodeAs = Format::Vec4;
            else if (value == "hex")
                out.DecodeAs = Format::Hex;
            else
                return "Invalid 'format': expected one of u32, i32, f32, vec4, hex.";
        }
        if (out.OffsetBytes % 4 != 0 && out.DecodeAs != Format::Hex)
            return "'offsetBytes' must be a multiple of 4 for format " + std::string(FormatToken(out.DecodeAs)) +
                   " (use format \"hex\" for an unaligned read).";
        return std::nullopt;
    }

    // One live buffer, flattened by the handler.
    struct Candidate
    {
        u64 Id = 0;
        u32 Binding = 0;
        u32 SizeBytes = 0;
        StorageBufferUsage Usage = StorageBufferUsage::DynamicDraw;
        std::string Owner; // memory-owner scope at creation; empty when none
        bool Pooled = false;
    };

    [[nodiscard]] inline Json CandidateJson(const Candidate& candidate)
    {
        Json j{ { "id", candidate.Id },
                { "binding", candidate.Binding },
                { "sizeBytes", candidate.SizeBytes },
                { "usage", UsageToken(candidate.Usage) } };
        // A pooled buffer's binding is nominal, so naming it would mislead.
        j["names"] = candidate.Pooled ? std::vector<std::string>{} : NamesForBinding(candidate.Binding);
        j["owner"] = candidate.Owner;
        j["pooled"] = candidate.Pooled;
        return j;
    }

    struct Selection
    {
        std::optional<Candidate> Chosen;
        std::string Error;
        Json Candidates = Json::array(); // the ones that matched the binding, for the error
    };

    [[nodiscard]] inline Selection SelectBuffer(const Request& request, const std::vector<Candidate>& live)
    {
        Selection selection;
        std::vector<const Candidate*> matches;
        sizet pooledOnBinding = 0;
        for (const auto& candidate : live)
        {
            if (request.Binding && candidate.Binding != *request.Binding)
                continue;
            if (request.Id && candidate.Id != *request.Id)
                continue;
            if (candidate.Pooled && !request.Id)
            {
                ++pooledOnBinding;
                continue;
            }
            matches.push_back(&candidate);
        }
        for (const Candidate* match : matches)
            selection.Candidates.push_back(CandidateJson(*match));

        if (matches.size() == 1)
        {
            selection.Chosen = *matches.front();
            return selection;
        }

        std::string what;
        if (request.Binding)
        {
            what = "binding " + std::to_string(*request.Binding);
            if (const auto names = NamesForBinding(*request.Binding); !names.empty())
            {
                what += " (";
                for (sizet i = 0; i < names.size(); ++i)
                    what += (i == 0 ? "" : ", ") + names[i];
                what += ")";
            }
        }
        if (request.Id)
            what += (what.empty() ? "id " : " with id ") + std::to_string(*request.Id);

        if (matches.empty())
        {
            selection.Error = "No live storage buffer matches " + what +
                              ". Buffers exist only while their pass or system does: check that the feature is "
                              "enabled and that a frame has rendered.";
            if (pooledOnBinding > 0)
                selection.Error += " " + std::to_string(pooledOnBinding) +
                                   " pooled buffer(s) carry this binding nominally and were skipped; select one by id.";
        }
        else
        {
            selection.Error = std::to_string(matches.size()) + " live storage buffers match " + what +
                              ". Pass 'id' to pick one; 'candidates' lists them.";
        }
        return selection;
    }

    // The range actually read, validated against the chosen buffer.
    struct Range
    {
        bool Ok = false;
        std::string Error;
        u32 OffsetBytes = 0;
        u32 LengthBytes = 0;
    };

    [[nodiscard]] inline Range ResolveRange(const Request& request, u32 bufferSize)
    {
        Range range;
        range.OffsetBytes = request.OffsetBytes;
        if (request.OffsetBytes >= bufferSize)
        {
            range.Error = "offsetBytes " + std::to_string(request.OffsetBytes) + " is past the end of the " +
                          std::to_string(bufferSize) + "-byte buffer.";
            return range;
        }
        const u32 remaining = bufferSize - request.OffsetBytes;
        const u32 element = ElementBytes(request.DecodeAs);
        if (request.LengthBytes)
        {
            if (*request.LengthBytes > remaining)
            {
                range.Error = "offsetBytes + lengthBytes = " +
                              std::to_string(static_cast<u64>(request.OffsetBytes) + *request.LengthBytes) +
                              " exceeds the " + std::to_string(bufferSize) + "-byte buffer.";
                return range;
            }
            if (*request.LengthBytes % element != 0)
            {
                range.Error = "lengthBytes " + std::to_string(*request.LengthBytes) + " is not a whole number of " +
                              FormatToken(request.DecodeAs) + " elements (" + std::to_string(element) + " bytes each).";
                return range;
            }
            range.LengthBytes = *request.LengthBytes;
        }
        else
        {
            // Defaulted: as much as fits, whole elements only.
            const u32 length = std::min(remaining, kDefaultReadBytes);
            range.LengthBytes = length - length % element;
            if (range.LengthBytes == 0)
            {
                range.Error = "Fewer than " + std::to_string(element) + " bytes remain after offsetBytes; nothing to "
                                                                        "decode as " +
                              FormatToken(request.DecodeAs) + ".";
                return range;
            }
        }
        range.Ok = true;
        return range;
    }

    [[nodiscard]] inline Json FloatJson(f32 value)
    {
        if (std::isnan(value))
            return "nan";
        if (std::isinf(value))
            return value > 0 ? "inf" : "-inf";
        return value;
    }

    [[nodiscard]] inline Json Decode(Format format, const std::vector<u8>& bytes)
    {
        Json values = Json::array();
        const sizet element = ElementBytes(format);
        if (format == Format::Hex)
        {
            // 16 bytes per string, the layout of a hex dump, so offsets can be
            // read off the index.
            constexpr char kDigits[] = "0123456789abcdef";
            for (sizet row = 0; row < bytes.size(); row += 16)
            {
                std::string line;
                for (sizet i = row; i < std::min(bytes.size(), row + 16); ++i)
                {
                    if (i != row)
                        line += ' ';
                    line += kDigits[bytes[i] >> 4];
                    line += kDigits[bytes[i] & 0xF];
                }
                values.push_back(std::move(line));
            }
            return values;
        }
        for (sizet offset = 0; offset + element <= bytes.size(); offset += element)
        {
            switch (format)
            {
                case Format::U32:
                {
                    u32 v = 0;
                    std::memcpy(&v, bytes.data() + offset, sizeof(v));
                    values.push_back(v);
                    break;
                }
                case Format::I32:
                {
                    i32 v = 0;
                    std::memcpy(&v, bytes.data() + offset, sizeof(v));
                    values.push_back(v);
                    break;
                }
                case Format::F32:
                {
                    f32 v = 0.0f;
                    std::memcpy(&v, bytes.data() + offset, sizeof(v));
                    values.push_back(FloatJson(v));
                    break;
                }
                case Format::Vec4:
                {
                    std::array<f32, 4> v{};
                    std::memcpy(v.data(), bytes.data() + offset, sizeof(v));
                    values.push_back(Json::array({ FloatJson(v[0]), FloatJson(v[1]), FloatJson(v[2]), FloatJson(v[3]) }));
                    break;
                }
                case Format::Hex:
                    break;
            }
        }
        return values;
    }

    [[nodiscard("this builds the response; it does not send it")]] inline Json
    BuildReport(const Candidate& buffer, const Range& range, Format format, const std::vector<u8>& bytes,
                std::string_view backend)
    {
        Json out;
        out["buffer"] = CandidateJson(buffer);
        out["backend"] = backend;
        out["offsetBytes"] = range.OffsetBytes;
        out["lengthBytes"] = range.LengthBytes;
        out["format"] = FormatToken(format);
        out["elementBytes"] = ElementBytes(format);
        out["values"] = Decode(format, bytes);
        out["reads"] = "The buffer's persistent GPU storage, copied back after the GPU finished the work submitted so "
                       "far (a synchronous, fenced read). Element i starts at offsetBytes + i * elementBytes.";
        return out;
    }
} // namespace OloEngine::MCP::GpuBufferRead
