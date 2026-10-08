#pragma once

#include "MCP/McpSchemaBuilder.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Renderer/Texture.h"
#include <filesystem>
#include <algorithm>
#include <optional>
#include <string>

namespace OloEngine::MCP::AssetPackBuild
{
    using Json = nlohmann::json;

    inline Json InputSchema()
    {
        return Schema::Object().Prop("output", Schema::String()).Prop("compress", Schema::Bool()).Prop("operationId", Schema::String().Desc("Poll or cancel a prior build; omit output/compress.")).Prop("cancel", Schema::Bool()).NoAdditional();
    }
    inline std::optional<std::string> ResolveOutput(const std::filesystem::path& project,
                                                    std::string input, std::filesystem::path& output)
    {
        namespace fs = std::filesystem;
        std::replace(input.begin(), input.end(), '\\', '/');
        if (input.empty() || input.find(':') != std::string::npos || input.find('\0') != std::string::npos)
            return "output must be a project-relative .olopack filename without drive/stream separators.";
        const fs::path relative(input);
        if (relative.is_absolute() || relative.has_root_path())
            return "Absolute output paths are refused.";
        for (const auto& part : relative)
            if (part == "..")
                return "Parent traversal in output is refused.";
        if (relative.extension() != ".olopack")
            return "output must end in .olopack.";
        std::error_code ec;
        const auto root = fs::weakly_canonical(project, ec);
        if (ec || root.empty())
            return "Cannot resolve the project directory.";
        output = fs::weakly_canonical(root / relative, ec);
        if (ec)
            return "Cannot resolve output: " + ec.message();
        auto a = root.begin();
        auto b = output.begin();
        for (; a != root.end(); ++a, ++b)
            if (b == output.end() || *a != *b)
                return "Output resolves outside the project (symlink/junction escape).";
        if (b == output.end() || fs::is_directory(output, ec))
            return "output must name a file inside the project.";
        return std::nullopt;
    }
    inline const char* FormatName(u32 format)
    {
        switch (static_cast<ImageFormat>(format))
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
            case ImageFormat::RGBA16F:
                return "RGBA16F";
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
            case ImageFormat::R32UI:
                return "R32UI";
            case ImageFormat::RGBA32UI:
                return "RGBA32UI";
            case ImageFormat::RGB8:
                return "RGB8";
            case ImageFormat::RGBA8:
                return "RGBA8";
            case ImageFormat::RGBA32F:
                return "RGBA32F";
            case ImageFormat::BC4:
                return "BC4";
            case ImageFormat::BC5:
                return "BC5";
            case ImageFormat::BC6H:
                return "BC6H";
            case ImageFormat::BC6HS:
                return "BC6HS";
            case ImageFormat::BC7:
                return "BC7";
            default:
                return "Other";
        }
    }
    inline Json ResultJson(const AssetPackBuilder::BuildResult& result)
    {
        Json records = Json::array();
        for (const auto& r : result.m_Records)
        {
            Json row{ { "handle", std::to_string(static_cast<u64>(r.Handle)) }, { "type", AssetUtils::AssetTypeToString(r.Type) }, { "path", r.Path.ToStdString() }, { "offset", r.Offset }, { "size", r.Size }, { "serialized", r.Size > 0 } };
            if (r.TextureFormat)
                row["texture"] = Json{ { "format", FormatName(*r.TextureFormat) }, { "formatId", *r.TextureFormat }, { "sRGB", r.TextureSRGB }, { "width", r.TextureWidth }, { "height", r.TextureHeight } };
            records.push_back(std::move(row));
        }
        return Json{ { "success", result.m_Success }, { "error", result.m_ErrorMessage }, { "output", result.m_OutputPath.generic_string() }, { "assetCount", result.m_AssetCount }, { "sceneCount", result.m_SceneCount }, { "failedAssetCount", result.m_FailedAssetCount }, { "assets", records }, { "runtime", Json{ { "launched", false }, { "reason", "Pack building does not create game.manifest, copy shaders/script/runtime binaries, or select a start scene. Use Build Game to assemble a runnable distribution; OloRuntime loads Assets/AssetPack.olopack relative to that game directory. Launching/managing a separate runtime process is outside this editor tool's scope." } } } };
    }
} // namespace OloEngine::MCP::AssetPackBuild
