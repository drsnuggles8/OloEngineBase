
#include "OloEnginePCH.h"
#include "AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCompression.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSerializer.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPrototype.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Animation/AnimationAsset.h"
#include "OloEngine/Animation/AnimationGraphAsset.h"
#include "OloEngine/Animation/AnimationGraphSerializer.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Cinematic/CinematicSequenceSerializer.h"
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Asset/SoundConfigAsset.h"
#include "OloEngine/Core/YAMLConverters.h"
#include "OloEngine/Particle/ParticleSystemAsset.h"
#include "OloEngine/Particle/EmissionShapeUtils.h"
#include "OloEngine/Particle/ParticleCurveSerializer.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Asset/MeshCache.h"
#include <yaml-cpp/yaml.h>
#include <stb_image/stb_image.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // True for the offline block-compressed container (.olotex, #440), matched
        // case-insensitively on the extension.
        bool IsOloTexPath(const std::filesystem::path& path)
        {
            std::string ext = path.extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return ext == ".olotex";
        }
    } // namespace

    //////////////////////////////////////////////////////////////////////////////////
    // TextureSerializer
    //////////////////////////////////////////////////////////////////////////////////

    std::atomic<bool> TextureSerializer::s_AssetPackCompressionEnabled{ false };

    void TextureSerializer::SetAssetPackCompressionEnabled(bool enabled)
    {
        s_AssetPackCompressionEnabled.store(enabled, std::memory_order_relaxed);
    }

    bool TextureSerializer::IsAssetPackCompressionEnabled()
    {
        return s_AssetPackCompressionEnabled.load(std::memory_order_relaxed);
    }

    bool TextureSerializer::IsLikelyColorTextureByName(std::string_view filename)
    {
        // Single source of truth for the colour/linear heuristic, shared with the
        // offline cook (Renderer/TextureCompression) so the two can't diverge (#440).
        return TextureCompression::IsLikelyColorTexture(filename);
    }

    bool TextureSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // Offline block-compressed container (#440): read the BCn mip chain and upload
        // it straight into a GPU-compressed texture.
        if (IsOloTexPath(metadata.FilePath))
        {
            CompressedTextureImage image;
            if (!TextureCompression::ReadFile(metadata.FilePath.string(), image))
            {
                OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to read .olotex: {}", metadata.FilePath.string());
                return false;
            }
            image.SourcePath = metadata.FilePath.string(); // so GetPath() -> pack re-read works
            Ref<Texture2D> texture = Texture2D::Create(image);
            if (!texture || !texture->IsLoaded())
            {
                OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to create compressed texture: {}", metadata.FilePath.string());
                return false;
            }
            texture->m_Handle = metadata.Handle;
            asset = texture;
            return true;
        }

        const std::string filename = metadata.FilePath.filename().string();
        const bool srgb = IsLikelyColorTextureByName(filename);
        Ref<Texture2D> texture = Texture2D::Create(metadata.FilePath.string(), srgb);
        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to create texture: {}", metadata.FilePath.string());
            return false;
        }

        texture->m_Handle = metadata.Handle;
        bool result = texture->IsLoaded();
        if (!result)
        {
            texture->SetFlag(AssetFlag::Invalid, true);
            OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to load texture: {}", metadata.FilePath.string());
        }

        asset = texture; // Direct assignment - Ref<Texture2D> should convert to Ref<Asset>
        return result;
    }

    bool TextureSerializer::TryLoadRawData(const AssetMetadata& metadata, RawAssetData& outRawData) const
    {
        OLO_PROFILE_FUNCTION();

        // This method is safe to call from any thread - no GPU/GL calls here

        // Offline block-compressed container (#440): reading the .olotex blob is pure
        // CPU work, so it belongs on the worker thread; GPU upload happens in Finalize.
        if (IsOloTexPath(metadata.FilePath))
        {
            CompressedTextureImage image;
            if (!TextureCompression::ReadFile(metadata.FilePath.string(), image))
            {
                OLO_CORE_ERROR("TextureSerializer::TryLoadRawData - Failed to read .olotex: {}", metadata.FilePath.string());
                return false;
            }
            image.SourcePath = metadata.FilePath.string(); // so GetPath() -> pack re-read works
            outRawData = std::move(image);
            return true;
        }

        std::string path = metadata.FilePath.string();

        int width = 0;
        int height = 0;
        int channels = 0;

        // Load image data using stb_image (thread-safe with thread-local flip)
        ::stbi_set_flip_vertically_on_load_thread(1);
        stbi_uc* data = nullptr;
        {
            OLO_PROFILE_SCOPE("stbi_load - TextureSerializer::TryLoadRawData");
            data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
        }
        ::stbi_set_flip_vertically_on_load_thread(0); // reset thread-local flag to avoid polluting later stbi calls

        if (!data)
        {
            OLO_CORE_ERROR("TextureSerializer::TryLoadRawData - Failed to load image: {}", path);
            return false;
        }

        // Copy pixel data to RawTextureData
        RawTextureData rawData;
        rawData.Width = static_cast<u32>(width);
        rawData.Height = static_cast<u32>(height);
        rawData.Channels = static_cast<u32>(channels);
        rawData.Handle = metadata.Handle;
        rawData.DebugName = metadata.FilePath.filename().string();
        rawData.GenerateMipmaps = true;
        // Filename heuristic: model loaders (Model.cpp / AnimatedModel.cpp)
        // pass sRGB explicitly per aiTextureType, but the asset pipeline
        // round-trips textures by path with no type hint. The shipped naming
        // convention is consistent enough that suffix matching is reliable:
        // diffuse/albedo/basecolor/emissive → sRGB, everything else linear.
        // If a future asset metadata schema adds an explicit IsColorTexture
        // bit, prefer that over this heuristic.
        rawData.SRGB = IsLikelyColorTextureByName(rawData.DebugName);

        // Copy the pixel data
        sizet dataSize = static_cast<sizet>(width) * height * channels;
        rawData.PixelData.resize(dataSize);
        std::memcpy(rawData.PixelData.data(), data, dataSize);

        // Free stb_image data
        ::stbi_image_free(data);

        OLO_CORE_TRACE("TextureSerializer::TryLoadRawData - Loaded raw texture data: {} ({}x{}, {} channels)",
                       rawData.DebugName, width, height, channels);

        outRawData = std::move(rawData);
        return true;
    }

    bool TextureSerializer::FinalizeFromRawData(const RawAssetData& rawData, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        // This method MUST be called from the main thread - creates GPU resources

        // Offline block-compressed container (#440): upload the BCn mip chain.
        if (std::holds_alternative<CompressedTextureImage>(rawData))
        {
            const auto& image = std::get<CompressedTextureImage>(rawData);
            Ref<Texture2D> texture = Texture2D::Create(image);
            if (!texture || !texture->IsLoaded())
            {
                OLO_CORE_ERROR("TextureSerializer::FinalizeFromRawData - Failed to create compressed texture");
                return false;
            }
            asset = texture;
            return true;
        }

        if (!std::holds_alternative<RawTextureData>(rawData))
        {
            OLO_CORE_ERROR("TextureSerializer::FinalizeFromRawData - Invalid raw data type");
            return false;
        }

        const auto& texData = std::get<RawTextureData>(rawData);

        if (!texData.IsValid())
        {
            OLO_CORE_ERROR("TextureSerializer::FinalizeFromRawData - Invalid texture data");
            return false;
        }

        // Create texture specification
        TextureSpecification spec;
        spec.Width = texData.Width;
        spec.Height = texData.Height;
        spec.GenerateMips = texData.GenerateMipmaps;
        // Honour the sRGB flag set by the loader. The OpenGL backend ignores
        // it for single/dual-channel data formats and for float/integer
        // formats, so it's safe to forward unconditionally here.
        spec.SRGB = texData.SRGB;

        // Determine format based on channel count
        // Note: The engine currently only supports R8, RG8, RGB8, RGBA8 and a few other formats
        switch (texData.Channels)
        {
            case 1:
                spec.Format = ImageFormat::R8;
                break;
            case 2:
                spec.Format = ImageFormat::RG8;
                break;
            case 3:
                spec.Format = ImageFormat::RGB8;
                break;
            case 4:
            default:
                spec.Format = ImageFormat::RGBA8;
                break;
        }

        // Create the texture on the main thread (GL calls happen here)
        Ref<Texture2D> texture = Texture2D::Create(spec);

        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::FinalizeFromRawData - Failed to create texture: {}", texData.DebugName);
            return false;
        }

        // Set the pixel data on the texture
        // Note: SetData expects size in bytes
        u32 dataSize = static_cast<u32>(texData.PixelData.size());
        texture->SetData(const_cast<u8*>(texData.PixelData.data()), dataSize);

        texture->m_Handle = texData.Handle;

        OLO_CORE_TRACE("TextureSerializer::FinalizeFromRawData - Created texture: {} ({}x{}, {} channels)",
                       texData.DebugName, texData.Width, texData.Height, texData.Channels);

        asset = texture;
        return true;
    }

    bool TextureSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto texture = AssetManager::GetAsset<Texture2D>(handle);
        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::SerializeToAssetPack - Invalid texture asset");
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();

        const auto& spec = texture->GetSpecification();
        const std::string& path = texture->GetPath();

        // Auto-cook (#440): when pack compression is enabled and this is an UNcompressed
        // source texture (not already an .olotex), BC-compress it in-memory and embed the
        // resulting container — so shipped textures get BCn + mips automatically, with no
        // separate cook step. Format is auto-picked (BC7 for LDR colour/linear, BC6H for
        // HDR; BC5 stays a deliberate opt-in via a pre-cooked .olotex). A cook failure
        // (no source file on disk, unsupported image) falls back to the raw record below,
        // so the build never fails just because one texture couldn't be compressed.
        CompressedTextureImage cooked;
        bool haveCooked = false;
        // Non-throwing existence check: a filesystem error (permissions, bad path) must
        // fall through to the uncompressed raw record, never propagate an exception up to
        // BuildImpl and abort the whole pack build over one texture.
        std::error_code existsEc;
        if (IsAssetPackCompressionEnabled() && !IsCompressedFormat(spec.Format) &&
            !path.empty() && !IsOloTexPath(path) && std::filesystem::exists(path, existsEc))
        {
            TextureCompression::CompressOptions opts;
            opts.GenerateMips = spec.GenerateMips;
            if (TextureCompression::CompressImageFile(path, opts, cooked) && cooked.IsValid())
                haveCooked = true;
            else
                OLO_CORE_WARN("TextureSerializer::SerializeToAssetPack - cook failed for '{}'; shipping it uncompressed", path);
        }

        // The record's ImageFormat: the cooked BCn format if we cooked, else the live
        // texture's format. The runtime deserialize keys the embedded-blob read path off
        // IsCompressedFormat(format), so a cooked record reuses the exact same read path
        // as a pre-compressed .olotex — no runtime change needed.
        ImageFormat recordFormat = spec.Format;
        bool recordSRGB = spec.SRGB;
        bool recordHasAlpha = texture->HasAlphaChannel();
        bool recordGenerateMips = spec.GenerateMips;
        if (haveCooked)
        {
            recordFormat = (cooked.Format == TextureCompressionFormat::BC5)    ? ImageFormat::BC5
                           : (cooked.Format == TextureCompressionFormat::BC6H) ? ImageFormat::BC6H
                                                                               : ImageFormat::BC7;
            recordSRGB = cooked.SRGB;
            recordHasAlpha = cooked.HasAlpha;
            recordGenerateMips = cooked.MipLevels() > 1u;
        }

        // Write texture metadata (layout shared with the pre-compressed path; the srgb
        // byte and any embedded blob are appended last to keep the uncompressed record
        // byte-identical to the legacy layout).
        stream.WriteRaw<u32>(spec.Width);
        stream.WriteRaw<u32>(spec.Height);
        stream.WriteRaw<u32>(static_cast<u32>(std::to_underlying(recordFormat)));
        stream.WriteRaw<bool>(recordGenerateMips);
        stream.WriteString(path); // source path (unused by the compressed read path)
        stream.WriteRaw<bool>(recordHasAlpha);
        stream.WriteRaw<bool>(texture->IsLoaded());

        // Add texture creation timestamp for dependency tracking
        auto now = std::chrono::steady_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        stream.WriteRaw<i64>(timestamp);

        // Persist the sRGB flag so the pack round-trip preserves colour-space. Without
        // this the deserializer falls back to the linear default and every albedo /
        // emissive shipped through an .olopack loses its GL_SRGB8_ALPHA8 conversion.
        stream.WriteRaw<bool>(recordSRGB);

        // Block-compressed textures (#440) cannot be re-created from the path via
        // stb_image, so embed the whole .olotex container blob here, making the pack
        // self-contained. For a just-cooked texture that blob is the in-memory container;
        // for a pre-compressed (.olotex) texture it is the exact on-disk bytes re-read
        // from the source path. Appended AFTER srgb so the legacy record layout for
        // uncompressed textures is byte-identical.
        if (IsCompressedFormat(recordFormat))
        {
            std::vector<u8> blob;
            if (haveCooked)
            {
                blob = TextureCompression::SerializeToBlob(cooked);
            }
            else if (!path.empty())
            {
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (file)
                {
                    const std::streamsize sz = file.tellg();
                    if (sz > 0)
                    {
                        blob.resize(static_cast<sizet>(sz));
                        file.seekg(0);
                        file.read(reinterpret_cast<char*>(blob.data()), sz);
                        if (!file)
                            blob.clear();
                    }
                }
            }
            if (blob.empty())
            {
                OLO_CORE_ERROR("TextureSerializer::SerializeToAssetPack - no .olotex blob for '{}'; packed texture would be empty", path);
                return false;
            }
            stream.WriteRaw<u64>(static_cast<u64>(blob.size()));
            stream.WriteData(reinterpret_cast<const char*>(blob.data()), blob.size());
        }

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> TextureSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read texture metadata
        u32 width;
        u32 height;
        u32 formatInt;
        bool generateMips;

        stream.ReadRaw(width);
        stream.ReadRaw(height);
        stream.ReadRaw(formatInt);
        stream.ReadRaw(generateMips);

        ImageFormat format = static_cast<ImageFormat>(formatInt);
        std::string path;
        stream.ReadString(path);

        // Read additional metadata to maintain cursor consistency
        bool hasAlphaChannel;
        bool isLoaded;
        i64 timestamp;

        stream.ReadRaw(hasAlphaChannel);
        stream.ReadRaw(isLoaded);
        stream.ReadRaw(timestamp);

        // Read the sRGB flag. End-of-stream tolerance: packs written before
        // this field existed don't carry the byte at all; fall back to the
        // filename heuristic so legacy packs still get colour textures
        // tagged correctly instead of silently demoting every albedo to
        // linear. AssetPackFile::AssetInfo carries the packed byte length,
        // so we only read when the cursor hasn't already consumed the whole
        // record.
        bool srgb = false;
        // Guard against u64 overflow when computing the end-of-record offset:
        // a corrupt pack could have PackedOffset + PackedSize wrap around and
        // spuriously satisfy the bounds check below. If the addition would
        // overflow, treat the byte as absent and fall back to the heuristic.
        const bool packedEndOverflows = assetInfo.PackedSize > std::numeric_limits<u64>::max() - assetInfo.PackedOffset;
        if (!packedEndOverflows && stream.GetStreamPosition() + sizeof(bool) <= assetInfo.PackedOffset + assetInfo.PackedSize)
        {
            stream.ReadRaw(srgb);
        }
        else if (!path.empty())
        {
            srgb = IsLikelyColorTextureByName(std::filesystem::path(path).filename().string());
        }
        else
        {
            // No additional handling required.
        }

        // Block-compressed textures (#440) embed the whole .olotex container blob after
        // the srgb byte (see SerializeToAssetPack). Reconstruct the GPU texture straight
        // from the embedded blob — self-contained, no dependency on the loose file.
        if (IsCompressedFormat(format))
        {
            u64 blobSize = 0;
            stream.ReadRaw(blobSize);
            const u64 recordEnd = packedEndOverflows ? std::numeric_limits<u64>::max()
                                                     : assetInfo.PackedOffset + assetInfo.PackedSize;
            if (blobSize == 0 || stream.GetStreamPosition() + blobSize > recordEnd)
            {
                OLO_CORE_ERROR("TextureSerializer::DeserializeFromAssetPack - compressed blob size {} out of record bounds", blobSize);
                return nullptr;
            }
            std::vector<u8> blob(static_cast<sizet>(blobSize));
            stream.ReadData(reinterpret_cast<char*>(blob.data()), blob.size());

            CompressedTextureImage image;
            if (!TextureCompression::DeserializeFromBlob(blob, image))
            {
                OLO_CORE_ERROR("TextureSerializer::DeserializeFromAssetPack - failed to parse embedded .olotex blob");
                return nullptr;
            }
            Ref<Texture2D> compressed = Texture2D::Create(image);
            if (!compressed || !compressed->IsLoaded())
            {
                OLO_CORE_ERROR("TextureSerializer::DeserializeFromAssetPack - failed to create compressed texture");
                return nullptr;
            }
            compressed->SetHandle(assetInfo.Handle);
            return compressed;
        }

        // Create texture specification
        TextureSpecification spec;
        spec.Width = width;
        spec.Height = height;
        spec.Format = format;
        spec.GenerateMips = generateMips;
        spec.SRGB = srgb;

        // Create texture from path if available, otherwise from specification.
        // The path-based load takes srgb as an explicit argument; the
        // spec-based load reads it off the spec we just populated.
        Ref<Texture2D> texture;
        if (!path.empty())
        {
            texture = Texture2D::Create(path, srgb);
        }
        else
        {
            texture = Texture2D::Create(spec);
        }

        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::DeserializeFromAssetPack - Failed to create texture");
            return nullptr;
        }

        texture->SetHandle(assetInfo.Handle);
        return texture;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // FontSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool FontSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        asset = Font::Create(metadata.FilePath);
        asset->m_Handle = metadata.Handle;

        // Note: Font loading validation could be added here if needed
        // bool result = asset.As<Font>()->Loaded();
        // if (!result) { ... }

        return true;
    }

    // Asset-pack record layout for a font (in stream order):
    //   string  name                  — display name
    //   u32     rangeCount            — number of codepoint ranges
    //   [rangeCount × {u32 First, u32 Last}]
    //   u64     byteCount + bytes      — the raw font-file (.ttf/.otf) image
    // The bytes are embedded (not path-referenced) so a shipped .olopack is
    // self-contained — it loads fonts even when the original .ttf is gone.
    // Fonts are small (KB–low-MB), so embedding is cheap.

    bool FontSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        outInfo.Offset = stream.GetStreamPosition();

        Ref<Font> font = AssetManager::GetAsset<Font>(handle);
        if (!font)
        {
            OLO_CORE_ERROR("FontSerializer::SerializeToAssetPack - Invalid font asset");
            return false;
        }

        // Read the original font-file bytes from disk so they can be embedded.
        const std::string& path = font->GetPath();
        if (path.empty())
        {
            OLO_CORE_ERROR("FontSerializer::SerializeToAssetPack - Font '{}' has no source path to read bytes from", font->GetName());
            return false;
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("FontSerializer::SerializeToAssetPack - Failed to open font file: {}", path);
            return false;
        }

        const std::streamoff fileSize = file.tellg();
        if (fileSize <= 0)
        {
            OLO_CORE_ERROR("FontSerializer::SerializeToAssetPack - Empty or unreadable font file: {}", path);
            return false;
        }

        file.seekg(0, std::ios::beg);

        Buffer fontData(static_cast<u64>(fileSize));
        file.read(reinterpret_cast<char*>(fontData.Data), fileSize);
        if (file.fail())
        {
            OLO_CORE_ERROR("FontSerializer::SerializeToAssetPack - Failed to read font file: {}", path);
            fontData.Release();
            return false;
        }

        stream.WriteString(font->GetName());
        stream.WriteArray(font->GetRanges());
        stream.WriteBuffer(fontData);

        fontData.Release();

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> FontSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string name;
        stream.ReadString(name);

        // Read the codepoint ranges (layout matches StreamWriter::WriteArray:
        // u32 count followed by count × {u32 First, u32 Last}). Read manually
        // rather than via ReadArray: ReadArray treats an explicit size of 0 as
        // "size not supplied" and would re-read the count, desyncing the
        // stream for a legitimately empty range list. A sanity cap keeps a
        // corrupt pack from driving a huge allocation.
        constexpr u32 kMaxFontRanges = 1024;
        u32 rangeCount = 0;
        stream.ReadRaw<u32>(rangeCount);
        if (rangeCount > kMaxFontRanges)
        {
            OLO_CORE_ERROR("FontSerializer::DeserializeFromAssetPack - Implausible range count {} (max {}); pack likely corrupt", rangeCount, kMaxFontRanges);
            return nullptr;
        }

        std::vector<FontCodepointRange> ranges(rangeCount);
        for (u32 i = 0; i < rangeCount; ++i)
        {
            stream.ReadRaw<u32>(ranges[i].First);
            stream.ReadRaw<u32>(ranges[i].Last);
        }

        Buffer fontData;
        stream.ReadBuffer(fontData); // reads u64 size, validates against the 1 GB cap, allocates
        if (!fontData.Data || fontData.Size == 0)
        {
            OLO_CORE_ERROR("FontSerializer::DeserializeFromAssetPack - Font '{}' has no embedded bytes", name);
            fontData.Release();
            return nullptr;
        }

        const std::span<const u8> fontSpan(fontData.Data, static_cast<sizet>(fontData.Size));
        Ref<Font> font = Font::Create(name, fontSpan, ranges);
        fontData.Release();

        if (!font || !font->IsLoaded())
        {
            OLO_CORE_ERROR("FontSerializer::DeserializeFromAssetPack - Failed to load font '{}' from embedded bytes", name);
            return nullptr;
        }

        font->SetHandle(assetInfo.Handle);
        return font;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MaterialAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void MaterialAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<MaterialAsset> materialAsset = asset.As<MaterialAsset>();
        if (!materialAsset)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::Serialize - Invalid material asset");
            return;
        }

        std::string yamlString = SerializeToYAML(materialAsset);

        // metadata.FilePath is project-root-relative (see
        // EditorAssetManager::GetRelativePath) — it already includes "Assets/".
        // Joining onto GetAssetDirectory() would double the Assets segment.
        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;
        std::ofstream fout(filepath);
        fout << yamlString;
        fout.close();
    }

    bool MaterialAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        Ref<MaterialAsset> materialAsset;
        if (!DeserializeFromYAML(GetYAML(metadata), materialAsset, metadata.Handle))
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::TryLoadData - Failed to deserialize material: {}", metadata.FilePath.string());
            // Note: Could set asset->SetFlag(AssetFlag::Invalid, true) here, but asset is not created yet
            return false;
        }
        asset = materialAsset;
        return true;
    }

    void MaterialAssetSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        RegisterDependenciesFromYAML(GetYAML(metadata), metadata.Handle);
    }

    bool MaterialAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<MaterialAsset> materialAsset = AssetManager::GetAsset<MaterialAsset>(handle);
        if (!materialAsset)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::SerializeToAssetPack - Invalid material asset");
            return false;
        }

        std::string yamlString = SerializeToYAML(materialAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> MaterialAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<MaterialAsset> materialAsset;
        if (bool result = DeserializeFromYAML(yamlString, materialAsset, assetInfo.Handle); !result)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromAssetPack - Failed to deserialize material from YAML");
            return nullptr;
        }

        return materialAsset;
    }

    std::string MaterialAssetSerializer::SerializeToYAML(Ref<MaterialAsset> materialAsset) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap; // Material
        out << YAML::Key << "Material" << YAML::Value;
        {
            out << YAML::BeginMap;

            if (auto material = materialAsset->GetMaterial())
            {
                // Serialize shader name
                out << YAML::Key << "Shader" << YAML::Value << material->GetShader()->GetName();

                // Serialize material textures
                out << YAML::Key << "Textures" << YAML::Value << YAML::BeginMap;

                // Serialize PBR texture maps
                if (material->GetAlbedoMap() && material->GetAlbedoMap()->m_Handle != 0)
                    out << YAML::Key << "AlbedoMap" << YAML::Value << material->GetAlbedoMap()->m_Handle;
                if (material->GetMetallicRoughnessMap() && material->GetMetallicRoughnessMap()->m_Handle != 0)
                    out << YAML::Key << "MetallicRoughnessMap" << YAML::Value << material->GetMetallicRoughnessMap()->m_Handle;
                if (material->GetNormalMap() && material->GetNormalMap()->m_Handle != 0)
                    out << YAML::Key << "NormalMap" << YAML::Value << material->GetNormalMap()->m_Handle;
                if (material->GetAOMap() && material->GetAOMap()->m_Handle != 0)
                    out << YAML::Key << "AOMap" << YAML::Value << material->GetAOMap()->m_Handle;
                if (material->GetEmissiveMap() && material->GetEmissiveMap()->m_Handle != 0)
                    out << YAML::Key << "EmissiveMap" << YAML::Value << material->GetEmissiveMap()->m_Handle;

                // Serialize dynamic texture uniforms
                const auto& texture2DUniforms = material->GetTexture2DUniforms();
                for (const auto& [name, texture] : texture2DUniforms)
                {
                    if (texture && texture->m_Handle != 0)
                    {
                        out << YAML::Key << name << YAML::Value << texture->m_Handle;
                    }
                }
                out << YAML::EndMap;

                // Serialize material properties
                out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;

                // Serialize PBR properties with consistent map structure
                out << YAML::Key << "BaseColor" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "vec4";
                out << YAML::Key << "value" << YAML::Value << YAML::Flow << YAML::BeginSeq
                    << material->GetBaseColorFactor().x << material->GetBaseColorFactor().y
                    << material->GetBaseColorFactor().z << material->GetBaseColorFactor().w << YAML::EndSeq;
                out << YAML::EndMap;

                out << YAML::Key << "Metallic" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "float";
                out << YAML::Key << "value" << YAML::Value << material->GetMetallicFactor();
                out << YAML::EndMap;

                out << YAML::Key << "Roughness" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "float";
                out << YAML::Key << "value" << YAML::Value << material->GetRoughnessFactor();
                out << YAML::EndMap;

                out << YAML::Key << "Emission" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "vec4";
                out << YAML::Key << "value" << YAML::Value << YAML::Flow << YAML::BeginSeq
                    << material->GetEmissiveFactor().x << material->GetEmissiveFactor().y
                    << material->GetEmissiveFactor().z << material->GetEmissiveFactor().w << YAML::EndSeq;
                out << YAML::EndMap;

                // Serialize dynamic float uniforms
                const auto& floatUniforms = material->GetFloatUniforms();
                for (const auto& [name, value] : floatUniforms)
                {
                    out << YAML::Key << name << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "type" << YAML::Value << "float";
                    out << YAML::Key << "value" << YAML::Value << value;
                    out << YAML::EndMap;
                }

                // Serialize dynamic vec3 uniforms
                const auto& vec3Uniforms = material->GetVec3Uniforms();
                for (const auto& [name, value] : vec3Uniforms)
                {
                    out << YAML::Key << name << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "type" << YAML::Value << "vec3";
                    out << YAML::Key << "value" << YAML::Value << YAML::Flow << YAML::BeginSeq
                        << value.x << value.y << value.z << YAML::EndSeq;
                    out << YAML::EndMap;
                }

                out << YAML::EndMap;

                // Serialize material flags
                out << YAML::Key << "MaterialFlags" << YAML::Value << material->GetFlags();
            }

            out << YAML::EndMap;
        }
        out << YAML::EndMap; // Material

        return std::string(out.c_str());
    }

    std::string MaterialAssetSerializer::GetYAML(const AssetMetadata& metadata) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;
        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::GetYAML - Failed to open file: {}", path.string());
            return "";
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        return strStream.str();
    }

    void MaterialAssetSerializer::RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const
    {
        // Deregister existing dependencies first
        AssetManager::DeregisterDependencies(handle);

        YAML::Node root = YAML::Load(yamlString);
        YAML::Node materialNode = root["Material"];
        if (!materialNode)
            return;

        // Register texture dependencies
        if (materialNode["Textures"])
        {
            for (const auto& textureNode : materialNode["Textures"])
            {
                AssetHandle textureHandle = textureNode.second.as<AssetHandle>(0);
                if (textureHandle != 0)
                {
                    AssetManager::RegisterDependency(textureHandle, handle);
                }
            }
        }
    }

    bool MaterialAssetSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<MaterialAsset>& targetMaterialAsset, AssetHandle handle) const
    {
        RegisterDependenciesFromYAML(yamlString, handle);

        YAML::Node root = YAML::Load(yamlString);
        YAML::Node materialNode = root["Material"];
        if (!materialNode)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromYAML - No Material node found");
            return false;
        }

        // Load shader
        std::string shaderName = materialNode["Shader"].as<std::string>("DefaultPBR");
        auto shader = Renderer3D::GetShaderLibrary().Get(shaderName);
        if (!shader)
        {
            // Fallback to loading from file if not in library
            shader = Shader::Create("assets/shaders/" + shaderName + ".glsl");
            if (shader)
            {
                // Add to library for future use
                Renderer3D::GetShaderLibrary().Add(shaderName, shader);
            }
        }
        if (!shader)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromYAML - Shader not found: {}", shaderName);
            return false;
        }

        auto material = Material::Create(shader);
        targetMaterialAsset = Ref<MaterialAsset>(new MaterialAsset(material));
        targetMaterialAsset->m_Handle = handle;

        // Load textures
        if (materialNode["Textures"])
        {
            for (const auto& textureNode : materialNode["Textures"])
            {
                std::string textureName = textureNode.first.as<std::string>();
                AssetHandle textureHandle = textureNode.second.as<AssetHandle>(0);

                if (textureHandle != 0)
                {
                    auto texture = AssetManager::GetAsset<Texture2D>(textureHandle);
                    if (texture)
                    {
                        material->Set(textureName, texture);
                    }
                }
            }
        }

        // Load properties/uniforms
        if (materialNode["Properties"])
        {
            for (const auto& propNode : materialNode["Properties"])
            {
                std::string propName = propNode.first.as<std::string>();
                const auto& valueNode = propNode.second;

                // Determine the type and set the appropriate material property
                if (valueNode["type"])
                {
                    std::string type = valueNode["type"].as<std::string>();

                    if (type == "float")
                    {
                        material->Set(propName, valueNode["value"].as<float>());
                    }
                    else if (type == "int")
                    {
                        material->Set(propName, valueNode["value"].as<int>());
                    }
                    else if (type == "uint")
                    {
                        material->Set(propName, valueNode["value"].as<u32>());
                    }
                    else if (type == "bool")
                    {
                        material->Set(propName, valueNode["value"].as<bool>());
                    }
                    else if (type == "vec2")
                    {
                        material->Set(propName, valueNode["value"].as<glm::vec2>());
                    }
                    else if (type == "vec3")
                    {
                        material->Set(propName, valueNode["value"].as<glm::vec3>());
                    }
                    else if (type == "vec4")
                    {
                        material->Set(propName, valueNode["value"].as<glm::vec4>());
                    }
                    else if (type == "mat3")
                    {
                        material->Set(propName, valueNode["value"].as<glm::mat3>());
                    }
                    else if (type == "mat4")
                    {
                        material->Set(propName, valueNode["value"].as<glm::mat4>());
                    }
                    // Texture properties would be handled separately with asset handles
                    else if (type == "texture2d")
                    {
                        AssetHandle textureHandle = valueNode["value"].as<AssetHandle>(0);
                        if (textureHandle != 0)
                        {
                            auto texture = AssetManager::GetAsset<Texture2D>(textureHandle);
                            if (texture)
                                material->Set(propName, texture);
                        }
                    }
                    else
                    {
                        // No additional handling required.
                    }
                }
            }
        }

        // Load material flags
        if (materialNode["MaterialFlags"])
        {
            u32 flags = materialNode["MaterialFlags"].as<u32>(0);
            material->SetFlags(flags);
        }

        return true;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // EnvironmentSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool EnvironmentSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("EnvironmentSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        // The .hdr / .exr -> EnvMap extension mapping (see AssetExtensions.cpp)
        // means we always receive a single equirectangular file here. Cubemap-
        // folder loading is handled by EnvironmentMapComponent at scene-render
        // time, not by the asset-registry path.
        EnvironmentMapSpecification spec;
        spec.FilePath = path.string();

        auto environment = EnvironmentMap::Create(spec);
        if (!environment)
        {
            OLO_CORE_ERROR("EnvironmentSerializer::TryLoadData - Failed to load environment: {}", path.string());
            return false;
        }

        environment->m_Handle = metadata.Handle;
        asset = environment;

        OLO_CORE_TRACE("EnvironmentSerializer::TryLoadData - Successfully loaded environment: {}", path.string());
        return true;
    }

    bool EnvironmentSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        outInfo.Offset = stream.GetStreamPosition();

        Ref<EnvironmentMap> environment = AssetManager::GetAsset<EnvironmentMap>(handle);
        if (!environment)
        {
            OLO_CORE_ERROR("EnvironmentSerializer::SerializeToAssetPack - Failed to get environment asset");
            return false;
        }

        // Serialize environment specification for recreation
        const auto& spec = environment->GetSpecification();
        stream.WriteString(spec.FilePath);
        stream.WriteRaw(spec.Resolution);
        stream.WriteRaw(static_cast<u32>(std::to_underlying(spec.Format)));
        stream.WriteRaw(spec.GenerateIBL);
        stream.WriteRaw(spec.GenerateMipmaps);

        // Serialize IBL configuration
        const auto& iblConfig = spec.IBLConfig;
        stream.WriteRaw(static_cast<u32>(std::to_underlying(iblConfig.Quality)));
        stream.WriteRaw(iblConfig.UseImportanceSampling);
        stream.WriteRaw(iblConfig.UseSphericalHarmonics);
        stream.WriteRaw(iblConfig.IrradianceResolution);
        stream.WriteRaw(iblConfig.PrefilterResolution);
        stream.WriteRaw(iblConfig.BRDFLutResolution);
        stream.WriteRaw(iblConfig.IrradianceSamples);
        stream.WriteRaw(iblConfig.PrefilterSamples);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        OLO_CORE_TRACE("EnvironmentSerializer::SerializeToAssetPack - Serialized environment: {}", spec.FilePath);
        return true;
    }

    Ref<Asset> EnvironmentSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read environment specification
        EnvironmentMapSpecification spec;
        stream.ReadString(spec.FilePath);
        stream.ReadRaw(spec.Resolution);

        u32 formatValue;
        stream.ReadRaw(formatValue);
        spec.Format = static_cast<ImageFormat>(formatValue);

        stream.ReadRaw(spec.GenerateIBL);
        stream.ReadRaw(spec.GenerateMipmaps);

        // Read IBL configuration
        u32 qualityValue;
        stream.ReadRaw(qualityValue);
        spec.IBLConfig.Quality = static_cast<IBLQuality>(qualityValue);

        stream.ReadRaw(spec.IBLConfig.UseImportanceSampling);
        stream.ReadRaw(spec.IBLConfig.UseSphericalHarmonics);
        stream.ReadRaw(spec.IBLConfig.IrradianceResolution);
        stream.ReadRaw(spec.IBLConfig.PrefilterResolution);
        stream.ReadRaw(spec.IBLConfig.BRDFLutResolution);
        stream.ReadRaw(spec.IBLConfig.IrradianceSamples);
        stream.ReadRaw(spec.IBLConfig.PrefilterSamples);

        // Recreate environment map from specification
        auto environment = EnvironmentMap::Create(spec);
        if (!environment)
        {
            OLO_CORE_ERROR("EnvironmentSerializer::DeserializeFromAssetPack - Failed to create environment from: {}", spec.FilePath);
            return nullptr;
        }

        environment->SetHandle(assetInfo.Handle);
        OLO_CORE_TRACE("EnvironmentSerializer::DeserializeFromAssetPack - Deserialized environment: {}", spec.FilePath);
        return environment;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AudioFileSourceSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void AudioFileSourceSerializer::Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const
    {
        // AudioFile assets don't require explicit serialization to file
        // as they're loaded based on metadata analysis of the source file
    }

    bool AudioFileSourceSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        // Get the file path for analysis
        std::filesystem::path filePath = Project::GetProjectDirectory() / metadata.FilePath;

        // Initialize default values
        double duration = 0.0;
        u32 samplingRate = 44100;
        u16 bitDepth = 16;
        u16 numChannels = 2;
        u64 fileSize = 0;

        // Get file size
        std::error_code ec;
        if (std::filesystem::exists(filePath, ec) && !ec)
        {
            fileSize = std::filesystem::file_size(filePath, ec);
            if (ec)
                fileSize = 0;
        }

        // Basic audio file format detection and analysis
        std::string extension = filePath.extension().string();
        std::ranges::transform(extension, extension.begin(),
                               [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });

        if (extension == ".wav")
        {
            // Basic WAV header analysis
            if (GetWavFileInfo(filePath, duration, samplingRate, bitDepth, numChannels))
            {
                OLO_CORE_TRACE("AudioFileSourceSerializer: Analyzed WAV file - Duration: {:.2f}s, Rate: {}Hz, Depth: {}bit, Channels: {}",
                               duration, samplingRate, bitDepth, numChannels);
            }
        }
        else if (extension == ".mp3" || extension == ".ogg" || extension == ".flac")
        {
            // For other formats, use estimated values based on file size
            // These are rough estimates - in the future, proper audio decoding should be implemented
            if (fileSize > 0)
            {
                // Estimate duration based on average bitrate assumptions
                double estimatedBitrate = 128000.0; // 128 kbps average for compressed audio
                if (extension == ".flac")
                    estimatedBitrate = 1000000.0; // 1 Mbps for FLAC

                duration = (fileSize * 8.0) / estimatedBitrate; // Convert bytes to duration
                samplingRate = 44100;                           // Standard CD quality
                bitDepth = 16;                                  // Standard for compressed formats
                numChannels = 2;                                // Assume stereo
            }

            OLO_CORE_TRACE("AudioFileSourceSerializer: Estimated audio properties for {} - Duration: {:.2f}s (estimated)",
                           extension, duration);
        }
        else
        {
            // No additional handling required.
        }

        // Create AudioFile asset with extracted/estimated metadata
        asset = Ref<AudioFile>::Create(duration, samplingRate, bitDepth, numChannels, fileSize);
        asset->SetHandle(metadata.Handle);

        OLO_CORE_TRACE("AudioFileSourceSerializer: Loaded AudioFile asset {} - {}MB",
                       metadata.Handle, fileSize / (1024 * 1024));
        return true;
    }

    bool AudioFileSourceSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        outInfo.Offset = stream.GetStreamPosition();

        if (Ref<AudioFile> audioFile = AssetManager::GetAsset<AudioFile>(handle); !audioFile)
        {
            OLO_CORE_ERROR("AudioFileSourceSerializer: Failed to get AudioFile asset for handle {0}", handle);
            return false;
        }

        // metadata.FilePath is project-root-relative (see
        // EditorAssetManager::GetRelativePath); resolve via GetProjectDirectory(). Then
        // serialize the *asset-directory-relative* form so the runtime side strips the
        // "Assets/" prefix the same way the rest of the asset-pack pipeline does.
        auto path = Project::GetProjectDirectory() / Project::GetAssetManager()->GetAssetMetadata(handle).FilePath;
        auto relativePath = std::filesystem::relative(path, Project::GetAssetDirectory());

        std::string filePath;
        if (relativePath.empty())
            filePath = path.string();
        else
            filePath = relativePath.string();

        // Serialize the file path so runtime can load the audio file
        stream.WriteString(filePath);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("AudioFileSourceSerializer: Serialized AudioFile to pack - Handle: {0}, Path: {1}, Size: {2}",
                       handle, filePath, outInfo.Size);
        return true;
    }

    Ref<Asset> AudioFileSourceSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string filePath;
        stream.ReadString(filePath);

        // Create AudioFile asset with file path information
        // TODO(olbu): In runtime, analyze the audio file to get proper metadata (#598)
        Ref<AudioFile> audioFile = Ref<AudioFile>::Create();
        audioFile->SetHandle(assetInfo.Handle);

        OLO_CORE_TRACE("AudioFileSourceSerializer: Deserialized AudioFile from pack - Handle: {0}, Path: {1}",
                       assetInfo.Handle, filePath);
        return audioFile;
    }

    bool AudioFileSourceSerializer::GetWavFileInfo(const std::filesystem::path& filePath, double& duration, u32& samplingRate, u16& bitDepth, u16& numChannels) const
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_WARN("AudioFileSourceSerializer: Failed to open WAV file: {}", filePath.string());
            return false;
        }

        // Read RIFF header
        char riffHeader[4];
        file.read(riffHeader, 4);
        if (std::memcmp(riffHeader, "RIFF", 4) != 0)
        {
            OLO_CORE_WARN("AudioFileSourceSerializer: Invalid RIFF header in WAV file: {}", filePath.string());
            return false;
        }

        // Skip chunk size (4 bytes)
        file.seekg(4, std::ios::cur);

        // Read WAVE format
        char waveHeader[4];
        file.read(waveHeader, 4);
        if (std::memcmp(waveHeader, "WAVE", 4) != 0)
        {
            OLO_CORE_WARN("AudioFileSourceSerializer: Invalid WAVE header in WAV file: {}", filePath.string());
            return false;
        }

        // Find fmt chunk
        bool fmtFound = false;
        u32 dataSize = 0;

        while (!file.eof() && (!fmtFound || dataSize == 0))
        {
            char chunkId[4];
            u32 chunkSize;

            file.read(chunkId, 4);
            file.read(reinterpret_cast<char*>(&chunkSize), 4);

            if (std::strncmp(chunkId, "fmt ", 4) == 0)
            {
                // Read format chunk
                u16 audioFormat, channels, blockAlign, bitsPerSample;
                u32 sampleRate, byteRate;

                file.read(reinterpret_cast<char*>(&audioFormat), 2);
                file.read(reinterpret_cast<char*>(&channels), 2);
                file.read(reinterpret_cast<char*>(&sampleRate), 4);
                file.read(reinterpret_cast<char*>(&byteRate), 4);
                file.read(reinterpret_cast<char*>(&blockAlign), 2);
                file.read(reinterpret_cast<char*>(&bitsPerSample), 2);

                // Store values
                numChannels = channels;
                samplingRate = sampleRate;
                bitDepth = bitsPerSample;
                fmtFound = true;

                // Skip any extra fmt data
                if (chunkSize > 16)
                {
                    file.seekg(chunkSize - 16, std::ios::cur);
                }
            }
            else if (std::strncmp(chunkId, "data", 4) == 0)
            {
                dataSize = chunkSize;
                // Skip the data chunk content
                file.seekg(chunkSize, std::ios::cur);
            }
            else
            {
                // Skip unknown chunk
                file.seekg(chunkSize, std::ios::cur);
            }
        }

        if (fmtFound && dataSize > 0)
        {
            // Calculate duration: dataSize / (sampleRate * channels * (bitDepth/8))
            if (u32 bytesPerSample = (bitDepth / 8) * numChannels; bytesPerSample > 0 && samplingRate > 0)
            {
                duration = static_cast<double>(dataSize) / (samplingRate * bytesPerSample);
            }

            OLO_CORE_TRACE("AudioFileSourceSerializer: WAV analysis complete - {}Hz, {}bit, {} channels, {:.2f}s",
                           samplingRate, bitDepth, numChannels, duration);
            return true;
        }

        OLO_CORE_WARN("AudioFileSourceSerializer: Failed to find required chunks in WAV file: {}", filePath.string());
        return false;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // SoundConfigSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void SoundConfigSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<SoundConfigAsset> soundConfig = asset.As<SoundConfigAsset>();
        if (!soundConfig)
        {
            OLO_CORE_ERROR("SoundConfigSerializer::Serialize - Asset is not a SoundConfig");
            return;
        }

        std::string yamlString = SerializeToYAML(soundConfig);

        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;

        // Ensure parent directory exists
        std::filesystem::path parentDir = filepath.parent_path();
        if (!parentDir.empty())
        {
            std::error_code ec;
            if (!std::filesystem::create_directories(parentDir, ec) && ec)
            {
                OLO_CORE_ERROR("SoundConfigSerializer::Serialize - Failed to create parent directories for: {}, error: {}", filepath.string(), ec.message());
                return;
            }
        }

        // Create temporary file for atomic write
        std::filesystem::path tempFilepath = parentDir / (filepath.filename().string() + ".tmp");

        // Write to temporary file
        std::ofstream fout(tempFilepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("SoundConfigSerializer::Serialize - Failed to open temporary file for writing: {}", tempFilepath.string());
            return;
        }

        fout << yamlString;
        fout.flush(); // Ensure data is written to the file

        // Verify the write/flush succeeded before promoting the temp file — a failed
        // stream (disk full, I/O error) must not be renamed over the good file.
        if (!fout)
        {
            OLO_CORE_ERROR("SoundConfigSerializer::Serialize - Failed to write to temporary file: {}", tempFilepath.string());
            fout.close();
            std::error_code rmEc;
            std::filesystem::remove(tempFilepath, rmEc);
            return;
        }
        fout.close();

        // Atomically rename temp file to final file
        std::error_code ec;
        std::filesystem::rename(tempFilepath, filepath, ec);
        if (ec)
        {
            OLO_CORE_ERROR("SoundConfigSerializer::Serialize - Failed to rename temporary file {} to {}, error: {}", tempFilepath.string(), filepath.string(), ec.message());
            // Clean up temporary file on failure
            std::filesystem::remove(tempFilepath, ec);
            return;
        }

        OLO_CORE_TRACE("SoundConfigSerializer::Serialize - Successfully serialized SoundConfig to: {}", filepath.string());
    }

    bool SoundConfigSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("SoundConfigSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("SoundConfigSerializer::TryLoadData - Failed to open file: {}", path.string());
            return false;
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        stream.close();

        auto soundConfig = Ref<SoundConfigAsset>::Create();
        if (bool success = DeserializeFromYAML(strStream.str(), soundConfig); !success)
        {
            OLO_CORE_ERROR("SoundConfigSerializer::TryLoadData - Failed to deserialize from YAML");
            return false;
        }

        soundConfig->SetHandle(metadata.Handle);
        asset = soundConfig;

        OLO_CORE_TRACE("SoundConfigSerializer::TryLoadData - Successfully loaded sound config: {}", path.string());
        return true;
    }

    bool SoundConfigSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<SoundConfigAsset> soundConfig = AssetManager::GetAsset<SoundConfigAsset>(handle);
        if (!soundConfig)
        {
            OLO_CORE_ERROR("SoundConfigSerializer: Failed to get SoundConfig for handle {0}", handle);
            return false;
        }

        std::string yamlString = SerializeToYAML(soundConfig);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("SoundConfigSerializer: Serialized SoundConfig to pack - Handle: {0}, Size: {1}", handle, outInfo.Size);
        return true;
    }

    Ref<Asset> SoundConfigSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string yamlString;
        stream.ReadString(yamlString);

        auto soundConfig = Ref<SoundConfigAsset>::Create();
        if (bool success = DeserializeFromYAML(yamlString, soundConfig); !success)
        {
            OLO_CORE_ERROR("SoundConfigSerializer: Failed to deserialize SoundConfig from YAML - Handle: {0}", assetInfo.Handle);
            return nullptr;
        }

        soundConfig->SetHandle(assetInfo.Handle);
        OLO_CORE_TRACE("SoundConfigSerializer: Deserialized SoundConfig from pack - Handle: {0}", assetInfo.Handle);
        return soundConfig;
    }

    std::string SoundConfigSerializer::SerializeToYAML(const Ref<SoundConfigAsset>& soundConfig) const
    {
        OLO_PROFILE_FUNCTION();

        const AudioSourceConfig& config = soundConfig->m_Config;

        YAML::Emitter out;
        out << YAML::BeginMap; // root

        out << YAML::Key << "SoundConfig" << YAML::Value;
        out << YAML::BeginMap; // SoundConfig data

        out << YAML::Key << "VolumeMultiplier" << YAML::Value << config.VolumeMultiplier;
        out << YAML::Key << "PitchMultiplier" << YAML::Value << config.PitchMultiplier;
        out << YAML::Key << "PlayOnAwake" << YAML::Value << config.PlayOnAwake;
        out << YAML::Key << "Looping" << YAML::Value << config.Looping;

        out << YAML::Key << "Spatialization" << YAML::Value << config.Spatialization;
        out << YAML::Key << "AttenuationModel" << YAML::Value << static_cast<int>(config.AttenuationModel);
        out << YAML::Key << "RollOff" << YAML::Value << config.RollOff;
        out << YAML::Key << "MinGain" << YAML::Value << config.MinGain;
        out << YAML::Key << "MaxGain" << YAML::Value << config.MaxGain;
        out << YAML::Key << "MinDistance" << YAML::Value << config.MinDistance;
        out << YAML::Key << "MaxDistance" << YAML::Value << config.MaxDistance;

        out << YAML::Key << "ConeInnerAngle" << YAML::Value << config.ConeInnerAngle;
        out << YAML::Key << "ConeOuterAngle" << YAML::Value << config.ConeOuterAngle;
        out << YAML::Key << "ConeOuterGain" << YAML::Value << config.ConeOuterGain;

        out << YAML::Key << "DopplerFactor" << YAML::Value << config.DopplerFactor;

        out << YAML::Key << "Spread" << YAML::Value << config.Spread;
        out << YAML::Key << "Focus" << YAML::Value << config.Focus;

        out << YAML::Key << "LowPassCutoff" << YAML::Value << config.LowPassCutoff;
        out << YAML::Key << "HighPassCutoff" << YAML::Value << config.HighPassCutoff;
        out << YAML::Key << "ReverbSend" << YAML::Value << config.ReverbSend;

        out << YAML::EndMap; // SoundConfig data
        out << YAML::EndMap; // root

        return std::string(out.c_str());
    }

    bool SoundConfigSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<SoundConfigAsset>& targetSoundConfig) const
    {
        OLO_PROFILE_FUNCTION();

        try
        {
            YAML::Node data = YAML::Load(yamlString);
            YAML::Node node = data["SoundConfig"];
            if (!node)
            {
                OLO_CORE_ERROR("SoundConfigSerializer: No SoundConfig node found in YAML");
                return false;
            }

            // Start from defaults so any missing key keeps its sensible value, and
            // any non-finite value read from disk falls back to the default below.
            AudioSourceConfig config{};

            // Read a float, validating it is finite (NaN / Inf from a corrupt file
            // are rejected in favour of the supplied default).
            auto readFloat = [&node](const char* key, f32 defaultValue) -> f32
            {
                if (!node[key])
                    return defaultValue;
                f32 value = node[key].as<f32>(defaultValue);
                if (!std::isfinite(value))
                    return defaultValue;
                return value;
            };

            // Multipliers: non-negative (a negative volume/pitch is meaningless).
            config.VolumeMultiplier = std::max(0.0f, readFloat("VolumeMultiplier", config.VolumeMultiplier));
            config.PitchMultiplier = std::max(0.0f, readFloat("PitchMultiplier", config.PitchMultiplier));

            if (node["PlayOnAwake"])
                config.PlayOnAwake = node["PlayOnAwake"].as<bool>(config.PlayOnAwake);
            if (node["Looping"])
                config.Looping = node["Looping"].as<bool>(config.Looping);
            if (node["Spatialization"])
                config.Spatialization = node["Spatialization"].as<bool>(config.Spatialization);

            // Attenuation model: clamp the enum to its valid range, else keep default.
            if (node["AttenuationModel"])
            {
                int model = node["AttenuationModel"].as<int>(static_cast<int>(config.AttenuationModel));
                if (model >= static_cast<int>(AttenuationModelType::None) && model <= static_cast<int>(AttenuationModelType::Exponential))
                    config.AttenuationModel = static_cast<AttenuationModelType>(model);
            }

            config.RollOff = std::max(0.0f, readFloat("RollOff", config.RollOff));

            // Gains are normalized [0,1].
            config.MinGain = std::clamp(readFloat("MinGain", config.MinGain), 0.0f, 1.0f);
            config.MaxGain = std::clamp(readFloat("MaxGain", config.MaxGain), 0.0f, 1.0f);

            // Distances are non-negative.
            config.MinDistance = std::max(0.0f, readFloat("MinDistance", config.MinDistance));
            config.MaxDistance = std::max(0.0f, readFloat("MaxDistance", config.MaxDistance));

            // Cone angles are radians in [0, 2*pi]; outer gain is normalized [0,1].
            constexpr f32 kTwoPi = glm::radians(360.0f);
            config.ConeInnerAngle = std::clamp(readFloat("ConeInnerAngle", config.ConeInnerAngle), 0.0f, kTwoPi);
            config.ConeOuterAngle = std::clamp(readFloat("ConeOuterAngle", config.ConeOuterAngle), 0.0f, kTwoPi);
            config.ConeOuterGain = std::clamp(readFloat("ConeOuterGain", config.ConeOuterGain), 0.0f, 1.0f);

            config.DopplerFactor = std::max(0.0f, readFloat("DopplerFactor", config.DopplerFactor));

            // VBAP spread / focus and DSP sends are all normalized [0,1].
            config.Spread = std::clamp(readFloat("Spread", config.Spread), 0.0f, 1.0f);
            config.Focus = std::clamp(readFloat("Focus", config.Focus), 0.0f, 1.0f);
            config.LowPassCutoff = std::clamp(readFloat("LowPassCutoff", config.LowPassCutoff), 0.0f, 1.0f);
            config.HighPassCutoff = std::clamp(readFloat("HighPassCutoff", config.HighPassCutoff), 0.0f, 1.0f);
            config.ReverbSend = std::clamp(readFloat("ReverbSend", config.ReverbSend), 0.0f, 1.0f);

            targetSoundConfig->m_Config = config;
            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("SoundConfigSerializer: YAML parsing error: {0}", e.what());
            return false;
        }
    }

    //////////////////////////////////////////////////////////////////////////////////
    // PrefabSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void PrefabSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<Prefab> prefab = asset.As<Prefab>();
        if (!prefab)
        {
            OLO_CORE_ERROR("PrefabSerializer::Serialize - Asset is not a Prefab");
            return;
        }

        std::string yamlString = SerializeToYAML(prefab);

        std::ofstream fout(metadata.FilePath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("PrefabSerializer::Serialize - Failed to open file for writing: {}", metadata.FilePath.string());
            return;
        }

        fout << yamlString;
        fout.close();
    }

    bool PrefabSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::ifstream stream(metadata.FilePath);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - Failed to open file: {}", metadata.FilePath.string());
            return false;
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        stream.close();

        Ref<Prefab> prefab = Ref<Prefab>::Create();
        if (bool success = DeserializeFromYAML(strStream.str(), prefab); !success)
        {
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - Failed to deserialize prefab from YAML");
            return false;
        }

        asset = prefab;
        asset->m_Handle = metadata.Handle;
        return true;
    }

    bool PrefabSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(handle);
        if (!prefab)
        {
            OLO_CORE_ERROR("PrefabSerializer::SerializeToAssetPack - Failed to get prefab asset");
            return false;
        }

        std::string yamlString = SerializeToYAML(prefab);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> PrefabSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<Prefab> prefab = Ref<Prefab>::Create();
        if (bool result = DeserializeFromYAML(yamlString, prefab); !result)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromAssetPack - Failed to deserialize prefab from YAML");
            return nullptr;
        }

        prefab->m_Handle = assetInfo.Handle;
        return prefab;
    }

    std::string PrefabSerializer::SerializeToYAML(const Ref<Prefab>& prefab) const
    {
        if (!prefab || !prefab->GetScene())
        {
            OLO_CORE_ERROR("PrefabSerializer::SerializeToYAML - Invalid prefab or scene");
            return "";
        }

        // Use SceneSerializer to serialize the entire scene
        SceneSerializer sceneSerializer(prefab->GetScene());
        std::string sceneYaml = sceneSerializer.SerializeToYAML();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Prefab";
        out << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Handle" << YAML::Value << prefab->GetHandle();
        out << YAML::Key << "Scene" << YAML::Value << sceneYaml;
        out << YAML::EndMap;
        out << YAML::EndMap;

        return std::string(out.c_str());
    }

    bool PrefabSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<Prefab>& prefab) const
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Failed to parse YAML: {}", e.what());
            return false;
        }

        auto prefabNode = data["Prefab"];
        if (!prefabNode)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Missing Prefab node");
            return false;
        }

        // Create a new scene for the prefab
        Ref<Scene> scene = Scene::Create();
        if (!scene)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Failed to create scene");
            return false;
        }

        // Deserialize the scene content
        if (auto sceneNode = prefabNode["Scene"])
        {
            std::string sceneYamlString = sceneNode.as<std::string>();
            SceneSerializer sceneSerializer(scene);
            bool result = sceneSerializer.DeserializeFromYAML(sceneYamlString);
            if (!result)
            {
                OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Failed to deserialize scene from YAML");
                return false;
            }
        }

        // Set up the prefab with the deserialized scene
        prefab->m_Scene = scene;

        // Find the root entity (assuming it's the first entity in the scene)
        if (auto entities = scene->GetAllEntitiesWith<IDComponent>(); !entities.empty())
        {
            auto firstEntity = entities.front();
            prefab->m_Entity = Entity{ firstEntity, scene.get() };
        }

        return true;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // SceneAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void SceneAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<Scene> scene = asset.As<Scene>();
        if (!scene)
        {
            OLO_CORE_ERROR("SceneAssetSerializer::Serialize - Asset is not a Scene");
            return;
        }

        SceneSerializer serializer(scene);
        serializer.Serialize(metadata.FilePath);
    }

    bool SceneAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        Ref<Scene> scene = Ref<Scene>(new Scene());
        if (SceneSerializer serializer(scene); serializer.Deserialize(metadata.FilePath))
        {
            scene->m_Handle = metadata.Handle;
            asset = scene; // Direct assignment - Ref<Scene> should convert to Ref<Asset>
            return true;
        }

        return false;
    }

    bool SceneAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto scene = AssetManager::GetAsset<Scene>(handle);
        if (!scene)
        {
            OLO_CORE_ERROR("SceneAssetSerializer::SerializeToAssetPack - Invalid scene asset");
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();

        // Serialize scene to YAML string directly
        std::string yamlData = SerializeToString(scene);

        // Write YAML data size and content
        u32 dataSize = (u32)yamlData.size();
        stream.WriteRaw(dataSize);
        stream.WriteData(yamlData.c_str(), dataSize);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("SceneAssetSerializer::SerializeToAssetPack - Serialized scene, size: {} bytes", outInfo.Size);
        return true;
    }

    Ref<Asset> SceneAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read YAML data size and content
        u32 dataSize;
        stream.ReadRaw(dataSize);

        std::vector<char> yamlData(dataSize + 1);
        stream.ReadData(yamlData.data(), dataSize);
        yamlData[dataSize] = '\0'; // Null terminate

        // Create scene and deserialize from YAML
        auto scene = Ref<Scene>(new Scene());

        if (!DeserializeFromString(yamlData.data(), scene))
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeFromAssetPack - Failed to deserialize scene from YAML");
            return nullptr;
        }

        scene->m_Handle = assetInfo.Handle;

        OLO_CORE_TRACE("SceneAssetSerializer::DeserializeFromAssetPack - Deserialized scene from pack");
        return scene; // Direct return - Ref<Scene> should convert to Ref<Asset>
    }

    Ref<Scene> SceneAssetSerializer::DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& sceneInfo) const
    {
        // Scene bytes live at the dedicated SceneInfo offset and use the same on-pack
        // layout written by SerializeToAssetPack: [u32 dataSize][char[dataSize] yaml].
        stream.SetStreamPosition(sceneInfo.PackedOffset);

        u32 dataSize = 0;
        stream.ReadRaw(dataSize);
        if (!stream.IsStreamGood() || dataSize == 0)
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeSceneFromAssetPack - Invalid scene data size ({}) for handle {}", dataSize, sceneInfo.Handle);
            return nullptr;
        }

        // dataSize is pack-controlled and untrusted. Reject sizes that would over-allocate
        // before touching memory: the YAML must fit within the scene's packed byte budget
        // (minus the 4-byte length prefix) when PackedSize is set, and within an absolute
        // sanity bound regardless.
        constexpr u32 c_MaxSceneYamlSize = 256u * 1024u * 1024u; // 256 MB
        const u64 maxFromPackedSize = (sceneInfo.PackedSize > sizeof(u32)) ? (sceneInfo.PackedSize - sizeof(u32)) : 0ull;
        if (dataSize > c_MaxSceneYamlSize || (sceneInfo.PackedSize != 0 && dataSize > maxFromPackedSize))
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeSceneFromAssetPack - Scene data size {} exceeds allowed bound for handle {} (packed size {})", dataSize, sceneInfo.Handle, sceneInfo.PackedSize);
            return nullptr;
        }

        std::vector<char> yamlData(static_cast<sizet>(dataSize) + 1);
        stream.ReadData(yamlData.data(), dataSize);
        if (!stream.IsStreamGood())
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeSceneFromAssetPack - Failed to read scene bytes for handle {}", sceneInfo.Handle);
            return nullptr;
        }
        yamlData[dataSize] = '\0'; // Null terminate

        auto scene = Ref<Scene>::Create();
        if (!DeserializeFromString(yamlData.data(), scene))
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeSceneFromAssetPack - Failed to deserialize scene from YAML for handle {}", sceneInfo.Handle);
            return nullptr;
        }

        scene->m_Handle = sceneInfo.Handle;

        OLO_CORE_TRACE("SceneAssetSerializer::DeserializeSceneFromAssetPack - Deserialized scene {} from pack", sceneInfo.Handle);
        return scene;
    }

    std::string SceneAssetSerializer::SerializeToString(const Ref<Scene>& scene) const
    {
        if (!scene)
        {
            OLO_CORE_ERROR("SceneAssetSerializer::SerializeToString - Scene is null");
            return "";
        }

        SceneSerializer serializer(scene);
        return serializer.SerializeToYAML();
    }

    bool SceneAssetSerializer::DeserializeFromString(const std::string& yamlString, Ref<Scene>& scene) const
    {
        if (yamlString.empty())
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeFromString - YAML string is empty");
            return false;
        }

        if (!scene)
        {
            scene = Ref<Scene>::Create();
        }

        SceneSerializer serializer(scene);
        return serializer.DeserializeFromYAML(yamlString);
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MeshColliderSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void MeshColliderSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<MeshColliderAsset> meshCollider = asset.As<MeshColliderAsset>();
        if (!meshCollider)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Invalid mesh collider asset");
            return;
        }

        std::string yamlString = SerializeToYAML(meshCollider);

        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;

        // Ensure parent directory exists
        std::filesystem::path parentDir = filepath.parent_path();
        if (!parentDir.empty())
        {
            std::error_code ec;
            if (!std::filesystem::create_directories(parentDir, ec) && ec)
            {
                OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Failed to create parent directories for: {}, error: {}", filepath.string(), ec.message());
                return;
            }
        }

        // Create temporary file for atomic write
        std::filesystem::path tempFilepath = parentDir / (filepath.filename().string() + ".tmp");

        // Write to temporary file
        std::ofstream fout(tempFilepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Failed to open temporary file for writing: {}", tempFilepath.string());
            return;
        }

        fout << yamlString;
        fout.flush(); // Ensure data is written to the file
        fout.close();

        // Atomically rename temp file to final file
        std::error_code ec;
        std::filesystem::rename(tempFilepath, filepath, ec);
        if (ec)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Failed to rename temporary file {} to {}, error: {}", tempFilepath.string(), filepath.string(), ec.message());
            // Clean up temporary file on failure
            std::filesystem::remove(tempFilepath, ec);
            return;
        }

        OLO_CORE_TRACE("MeshColliderSerializer::Serialize - Successfully serialized MeshCollider to: {}", filepath.string());
    }

    bool MeshColliderSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - Failed to open file: {}", path.string());
            return false;
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        stream.close();

        YAML::Node data;
        try
        {
            data = YAML::Load(strStream.str());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - YAML parsing error: {}", e.what());
            return false;
        }

        if (auto colliderNode = data["MeshCollider"]; !colliderNode)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - No MeshCollider node found");
            return false;
        }

        auto meshCollider = Ref<MeshColliderAsset>::Create();

        // Use the YAML deserializer to load the data
        if (bool success = DeserializeFromYAML(strStream.str(), meshCollider); !success)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - Failed to deserialize from YAML");
            return false;
        }

        meshCollider->SetHandle(metadata.Handle);
        asset = meshCollider;

        OLO_CORE_TRACE("MeshColliderSerializer::TryLoadData - Successfully loaded mesh collider: {}", path.string());
        return true;
    }

    bool MeshColliderSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<MeshColliderAsset> meshCollider = AssetManager::GetAsset<MeshColliderAsset>(handle);
        if (!meshCollider)
        {
            OLO_CORE_ERROR("MeshColliderSerializer: Failed to get MeshColliderAsset for handle {0}", handle);
            return false;
        }

        std::string yamlString = SerializeToYAML(meshCollider);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("MeshColliderSerializer: Serialized MeshCollider to pack - Handle: {0}, Size: {1}",
                       handle, outInfo.Size);
        return true;
    }

    Ref<Asset> MeshColliderSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<MeshColliderAsset> meshCollider = Ref<MeshColliderAsset>::Create();
        if (bool success = DeserializeFromYAML(yamlString, meshCollider); !success)
        {
            OLO_CORE_ERROR("MeshColliderSerializer: Failed to deserialize MeshCollider from YAML - Handle: {0}", assetInfo.Handle);
            return nullptr;
        }

        meshCollider->SetHandle(assetInfo.Handle);
        OLO_CORE_TRACE("MeshColliderSerializer: Deserialized MeshCollider from pack - Handle: {0}", assetInfo.Handle);
        return meshCollider;
    }

    std::string MeshColliderSerializer::SerializeToYAML(Ref<MeshColliderAsset> meshCollider) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap; // MeshCollider

        out << YAML::Key << "MeshCollider" << YAML::Value;
        out << YAML::BeginMap; // Asset Data

        // Serialize ColliderMesh asset reference
        out << YAML::Key << "ColliderMesh" << YAML::Value << meshCollider->m_ColliderMesh;

        // Serialize Material properties
        out << YAML::Key << "Material" << YAML::Value;
        out << YAML::BeginMap; // Material
        out << YAML::Key << "StaticFriction" << YAML::Value << meshCollider->m_Material.GetStaticFriction();
        out << YAML::Key << "DynamicFriction" << YAML::Value << meshCollider->m_Material.GetDynamicFriction();
        out << YAML::Key << "Restitution" << YAML::Value << meshCollider->m_Material.GetRestitution();
        out << YAML::Key << "Density" << YAML::Value << meshCollider->m_Material.GetDensity();
        out << YAML::EndMap; // Material

        // Serialize other properties
        out << YAML::Key << "EnableVertexWelding" << YAML::Value << meshCollider->m_EnableVertexWelding;
        out << YAML::Key << "VertexWeldTolerance" << YAML::Value << meshCollider->m_VertexWeldTolerance;
        out << YAML::Key << "FlipNormals" << YAML::Value << meshCollider->m_FlipNormals;
        out << YAML::Key << "CheckZeroAreaTriangles" << YAML::Value << meshCollider->m_CheckZeroAreaTriangles;
        out << YAML::Key << "AreaTestEpsilon" << YAML::Value << meshCollider->m_AreaTestEpsilon;
        out << YAML::Key << "ShiftVerticesToOrigin" << YAML::Value << meshCollider->m_ShiftVerticesToOrigin;
        out << YAML::Key << "AlwaysShareShape" << YAML::Value << meshCollider->m_AlwaysShareShape;

        // Serialize collision complexity
        out << YAML::Key << "CollisionComplexity" << YAML::Value << (int)meshCollider->m_CollisionComplexity;

        // Serialize scale
        out << YAML::Key << "ColliderScale" << YAML::Value << meshCollider->m_ColliderScale;

        out << YAML::EndMap; // Asset Data
        out << YAML::EndMap; // MeshCollider

        return std::string(out.c_str());
    }

    bool MeshColliderSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<MeshColliderAsset> targetMeshCollider) const
    {
        try
        {
            YAML::Node data = YAML::Load(yamlString);
            if (!data["MeshCollider"])
            {
                OLO_CORE_ERROR("MeshColliderSerializer: No MeshCollider node found in YAML");
                return false;
            }

            YAML::Node meshColliderNode = data["MeshCollider"];

            // Deserialize ColliderMesh asset reference
            if (meshColliderNode["ColliderMesh"])
                targetMeshCollider->m_ColliderMesh = meshColliderNode["ColliderMesh"].as<AssetHandle>();

            // Deserialize Material properties
            if (meshColliderNode["Material"])
            {
                YAML::Node materialNode = meshColliderNode["Material"];
                ColliderMaterial material;
                // Handle both old and new material formats for backward compatibility
                if (materialNode["Friction"])
                {
                    float friction = materialNode["Friction"].as<float>(0.5f);
                    // Clamp friction values to valid range [0.0, 1.0]
                    friction = std::clamp(friction, 0.0f, 1.0f);
                    material.SetStaticFriction(friction);
                    material.SetDynamicFriction(friction);
                }
                else
                {
                    float staticFriction = materialNode["StaticFriction"].as<float>(0.6f);
                    float dynamicFriction = materialNode["DynamicFriction"].as<float>(0.6f);
                    // Clamp friction values to valid range [0.0, 1.0]
                    material.SetStaticFriction(std::clamp(staticFriction, 0.0f, 1.0f));
                    material.SetDynamicFriction(std::clamp(dynamicFriction, 0.0f, 1.0f));
                }

                float restitution = materialNode["Restitution"].as<float>(0.0f);
                float density = materialNode["Density"].as<float>(1000.0f);

                // Clamp restitution to valid range [0.0, 1.0]
                material.SetRestitution(std::clamp(restitution, 0.0f, 1.0f));

                // Clamp density to sensible positive range [MIN_DENSITY, 1e6]
                material.SetDensity(std::clamp(density, ColliderMaterial::MIN_DENSITY, 1e6f));

                targetMeshCollider->m_Material = material;
            }

            // Deserialize other properties
            if (meshColliderNode["EnableVertexWelding"])
                targetMeshCollider->m_EnableVertexWelding = meshColliderNode["EnableVertexWelding"].as<bool>();
            if (meshColliderNode["VertexWeldTolerance"])
                targetMeshCollider->m_VertexWeldTolerance = meshColliderNode["VertexWeldTolerance"].as<float>();
            if (meshColliderNode["FlipNormals"])
                targetMeshCollider->m_FlipNormals = meshColliderNode["FlipNormals"].as<bool>();
            if (meshColliderNode["CheckZeroAreaTriangles"])
                targetMeshCollider->m_CheckZeroAreaTriangles = meshColliderNode["CheckZeroAreaTriangles"].as<bool>();
            if (meshColliderNode["AreaTestEpsilon"])
                targetMeshCollider->m_AreaTestEpsilon = meshColliderNode["AreaTestEpsilon"].as<float>();
            if (meshColliderNode["ShiftVerticesToOrigin"])
                targetMeshCollider->m_ShiftVerticesToOrigin = meshColliderNode["ShiftVerticesToOrigin"].as<bool>();
            if (meshColliderNode["AlwaysShareShape"])
                targetMeshCollider->m_AlwaysShareShape = meshColliderNode["AlwaysShareShape"].as<bool>();

            // Deserialize collision complexity
            if (meshColliderNode["CollisionComplexity"])
                targetMeshCollider->m_CollisionComplexity = (ECollisionComplexity)meshColliderNode["CollisionComplexity"].as<int>();

            // Deserialize scale
            if (meshColliderNode["ColliderScale"])
                targetMeshCollider->m_ColliderScale = meshColliderNode["ColliderScale"].as<glm::vec3>();

            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshColliderSerializer: YAML parsing error: {0}", e.what());
            return false;
        }
    }

    void MeshColliderSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("MeshColliderSerializer::RegisterDependencies - File does not exist: {}", path.string());
            return;
        }

        std::ifstream fin(path);
        if (!fin.good())
        {
            OLO_CORE_WARN("MeshColliderSerializer::RegisterDependencies - Failed to open file: {}", path.string());
            return;
        }

        std::stringstream ss;
        ss << fin.rdbuf();
        fin.close();

        RegisterDependenciesFromYAML(ss.str(), metadata.Handle);
    }

    void MeshColliderSerializer::RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const
    {
        // Deregister existing dependencies first
        AssetManager::DeregisterDependencies(handle);

        try
        {
            YAML::Node root = YAML::Load(yamlString);
            YAML::Node meshColliderNode = root["MeshCollider"];
            if (!meshColliderNode)
                return;

            // Register ColliderMesh dependency
            if (meshColliderNode["ColliderMesh"])
            {
                AssetHandle colliderMeshHandle = meshColliderNode["ColliderMesh"].as<AssetHandle>(0);
                if (colliderMeshHandle != 0)
                {
                    AssetManager::RegisterDependency(colliderMeshHandle, handle);
                    OLO_CORE_TRACE("MeshColliderSerializer: Registered dependency - MeshCollider {0} depends on ColliderMesh {1}", handle, colliderMeshHandle);
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::RegisterDependenciesFromYAML - YAML parsing error: {0}", e.what());
        }
    }

    //////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////
    // ScriptFileSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void ScriptFileSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<ScriptFileAsset> scriptAsset = asset.As<ScriptFileAsset>();
        std::string yamlString = SerializeToYAML(scriptAsset);

        std::ofstream fout(Project::GetProjectDirectory() / metadata.FilePath);
        fout << yamlString;

        OLO_CORE_TRACE("ScriptFileSerializer: Serialized ScriptFile to YAML - Handle: {0}", metadata.Handle);
    }

    bool ScriptFileSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - Failed to open file: {}", path.string());
            return false;
        }

        std::stringstream strStream;
        strStream << file.rdbuf();

        Ref<ScriptFileAsset> scriptAsset = Ref<ScriptFileAsset>::Create();
        if (bool success = DeserializeFromYAML(strStream.str(), scriptAsset); !success)
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - Failed to deserialize from YAML");
            return false;
        }

        scriptAsset->SetHandle(metadata.Handle);
        asset = scriptAsset;

        OLO_CORE_TRACE("ScriptFileSerializer::TryLoadData - Successfully loaded script file: {}", path.string());
        return true;
    }

    bool ScriptFileSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<ScriptFileAsset> scriptAsset = AssetManager::GetAsset<ScriptFileAsset>(handle);
        if (!scriptAsset)
        {
            OLO_CORE_ERROR("ScriptFileSerializer: Failed to get ScriptFileAsset for handle {0}", handle);
            return false;
        }

        std::string yamlString = SerializeToYAML(scriptAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("ScriptFileSerializer: Serialized ScriptFile to pack - Handle: {0}, Size: {1}",
                       handle, outInfo.Size);
        return true;
    }

    Ref<Asset> ScriptFileSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<ScriptFileAsset> scriptAsset = Ref<ScriptFileAsset>::Create();
        if (bool success = DeserializeFromYAML(yamlString, scriptAsset); !success)
        {
            OLO_CORE_ERROR("ScriptFileSerializer: Failed to deserialize ScriptFile from YAML - Handle: {0}", assetInfo.Handle);
            return nullptr;
        }

        scriptAsset->SetHandle(assetInfo.Handle);
        OLO_CORE_TRACE("ScriptFileSerializer: Deserialized ScriptFile from pack - Handle: {0}", assetInfo.Handle);
        return scriptAsset;
    }

    std::string ScriptFileSerializer::SerializeToYAML(Ref<ScriptFileAsset> scriptAsset) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap; // ScriptFile

        out << YAML::Key << "ScriptFile" << YAML::Value;
        out << YAML::BeginMap; // Asset Data

        out << YAML::Key << "ClassNamespace" << YAML::Value << scriptAsset->GetClassNamespace();
        out << YAML::Key << "ClassName" << YAML::Value << scriptAsset->GetClassName();

        out << YAML::EndMap; // Asset Data
        out << YAML::EndMap; // ScriptFile

        return std::string(out.c_str());
    }

    bool ScriptFileSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<ScriptFileAsset> targetScriptAsset) const
    {
        try
        {
            YAML::Node data = YAML::Load(yamlString);
            if (!data["ScriptFile"])
            {
                OLO_CORE_ERROR("ScriptFileSerializer: No ScriptFile node found in YAML");
                return false;
            }

            YAML::Node scriptNode = data["ScriptFile"];

            if (scriptNode["ClassNamespace"])
                targetScriptAsset->SetClassNamespace(scriptNode["ClassNamespace"].as<std::string>());
            if (scriptNode["ClassName"])
                targetScriptAsset->SetClassName(scriptNode["ClassName"].as<std::string>());

            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("ScriptFileSerializer: YAML parsing error: {0}", e.what());
            return false;
        }
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MeshSourceSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool MeshSourceSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("MeshSourceSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        // Try loading from binary mesh cache first
        if (MeshCache::IsMeshCacheValid(path))
        {
            auto meshSource = MeshCache::LoadMeshFromCache(path);
            if (meshSource)
            {
                meshSource->Build();
                meshSource->SetHandle(metadata.Handle);
                asset = meshSource;
                OLO_CORE_TRACE("MeshSourceSerializer::TryLoadData - Loaded from cache: {}", path.string());
                return true;
            }
        }

        // Cache miss — import via Assimp through Model, which writes the cache for next time
        OLO_CORE_INFO("MeshSourceSerializer::TryLoadData - Cache miss, importing via Assimp: {}", path.string());
        Model model(path.string());
        auto meshSource = model.CreateCombinedMeshSource();
        if (!meshSource || meshSource->GetVertices().IsEmpty())
        {
            OLO_CORE_ERROR("MeshSourceSerializer::TryLoadData - Assimp import failed: {}", path.string());
            return false;
        }

        // CreateCombinedMeshSource does not call Build() (it's also used for cache-only
        // serialization). Build GPU buffers here so the asset is renderable.
        meshSource->Build();

        meshSource->SetHandle(metadata.Handle);
        asset = meshSource;
        OLO_CORE_TRACE("MeshSourceSerializer::TryLoadData - Loaded via Assimp: {} ({} verts, {} submeshes)",
                       path.string(), meshSource->GetVertices().Num(), meshSource->GetSubmeshes().Num());
        return true;
    }

    bool MeshSourceSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto meshSource = AssetManager::GetAsset<MeshSource>(handle);
        if (!meshSource)
        {
            OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Invalid MeshSource asset");
            return false;
        }

        const auto& vertices = meshSource->GetVertices();
        const auto& indices = meshSource->GetIndices();
        const auto& submeshes = meshSource->GetSubmeshes();
        // Use the const overload to read materials — the non-const overload is deprecated.
        const auto& materials = std::as_const(*meshSource).GetMaterials();

        auto vertexCount = static_cast<u32>(vertices.Num());
        auto indexCount = static_cast<u32>(indices.Num());
        auto submeshCount = static_cast<u32>(submeshes.Num());
        auto materialCount = static_cast<u32>(materials.Num());
        bool hasBoneInfluences = meshSource->HasBoneInfluences();
        bool hasShadowIndices = meshSource->HasShadowIndices();
        bool hasSkeleton = meshSource->HasSkeleton();
        bool hasBoneInfo = !meshSource->GetBoneInfo().IsEmpty();
        bool hasMorphTargets = meshSource->HasMorphTargets();

        // ── Preflight validation ──
        // Validate all invariants before committing any bytes to stream so that
        // failures never leave a truncated/partial entry.
        // Mirror the same hard caps that DeserializeFromAssetPack enforces so the
        // writer never creates a pack the reader would reject.
        constexpr u32 kMaxVertexCount = 50'000'000;
        constexpr u32 kMaxIndexCount = 150'000'000;
        constexpr u32 kMaxSubmeshCount = 10'000;
        constexpr u32 kMaxMaterialCount = 10'000;
        if (vertexCount > kMaxVertexCount || indexCount > kMaxIndexCount ||
            submeshCount > kMaxSubmeshCount || materialCount > kMaxMaterialCount)
        {
            OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Counts exceed limits "
                           "(verts={}, indices={}, submeshes={}, materials={}), rejecting before write",
                           vertexCount, indexCount, submeshCount, materialCount);
            return false;
        }
        if (hasBoneInfluences)
        {
            if (static_cast<u32>(meshSource->GetBoneInfluences().Num()) != vertexCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - BoneInfluence count ({}) != vertex count ({}), "
                               "rejecting before write",
                               meshSource->GetBoneInfluences().Num(), vertexCount);
                return false;
            }
        }
        if (hasMorphTargets)
        {
            constexpr u32 MAX_NAME_LEN = 1'024;
            constexpr u32 MAX_MORPH_TARGETS = 1'000;
            const auto& morphTargets = meshSource->GetMorphTargets();
            auto vertCount = morphTargets->GetVertexCount();
            if (morphTargets->GetTargetCount() > MAX_MORPH_TARGETS)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Morph target count ({}) exceeds limit ({}), "
                               "rejecting before write",
                               morphTargets->GetTargetCount(), MAX_MORPH_TARGETS);
                return false;
            }
            if (vertCount != vertexCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Morph target vertex count ({}) != mesh vertex count ({}), "
                               "rejecting before write",
                               vertCount, vertexCount);
                return false;
            }
            for (u32 t = 0; t < morphTargets->GetTargetCount(); ++t)
            {
                const auto& target = morphTargets->Targets[t];
                if (target.Name.size() > MAX_NAME_LEN)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Morph target name length ({}) exceeds limit ({}), "
                                   "rejecting before write",
                                   target.Name.size(), MAX_NAME_LEN);
                    return false;
                }
                auto sparseCount = (target.IsSparse && !target.SparseVertices.empty())
                                       ? static_cast<u32>(target.SparseVertices.size())
                                       : 0u;
                if (sparseCount == 0 && static_cast<u32>(target.Vertices.size()) != vertCount)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Dense morph target '{}' vertex count ({}) "
                                   "does not match expected vertCount ({}), rejecting before write",
                                   target.Name, target.Vertices.size(), vertCount);
                    return false;
                }
            }
        }
        if (hasSkeleton)
        {
            constexpr u32 MAX_BONE_NAME_LEN = 1'024;
            constexpr u32 kMaxBoneCount = 10'000;
            const auto* skeleton = meshSource->GetSkeleton();
            auto boneCount = static_cast<u32>(skeleton->m_BoneNames.size());
            if (boneCount > kMaxBoneCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Bone count ({}) exceeds limit ({}), "
                               "rejecting before write",
                               boneCount, kMaxBoneCount);
                return false;
            }
            for (u32 j = 0; j < boneCount; ++j)
            {
                if (skeleton->m_BoneNames[j].size() > MAX_BONE_NAME_LEN)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Bone name length ({}) exceeds limit ({}) at bone {}, "
                                   "rejecting before write",
                                   skeleton->m_BoneNames[j].size(), MAX_BONE_NAME_LEN, j);
                    return false;
                }
            }
            // Validate parent indices are within [0, boneCount) or -1 (root sentinel)
            for (u32 j = 0; j < static_cast<u32>(skeleton->m_ParentIndices.size()); ++j)
            {
                auto const idx = skeleton->m_ParentIndices[j];
                if (idx != -1 && (idx < 0 || idx >= static_cast<i32>(boneCount)))
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - ParentIndex[{}] = {} "
                                   "is out of range [0, {}) or -1, rejecting before write",
                                   j, idx, boneCount);
                    return false;
                }
            }
            // Validate all skeleton transform matrices are finite
            auto const validateFiniteMat4Array = [](const std::vector<glm::mat4>& arr, const char* name) -> bool
            {
                for (sizet i = 0; i < arr.size(); ++i)
                {
                    const auto& m = arr[i];
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r)
                            if (!std::isfinite(m[c][r]))
                            {
                                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Non-finite value in skeleton {} "
                                               "at bone {} element [{},{}], rejecting before write",
                                               name, i, c, r);
                                return false;
                            }
                }
                return true;
            };
            if (!validateFiniteMat4Array(skeleton->m_LocalTransforms, "LocalTransforms") ||
                !validateFiniteMat4Array(skeleton->m_GlobalTransforms, "GlobalTransforms") ||
                !validateFiniteMat4Array(skeleton->m_BindPoseMatrices, "BindPoseMatrices") ||
                !validateFiniteMat4Array(skeleton->m_InverseBindPoses, "InverseBindPoses") ||
                !validateFiniteMat4Array(skeleton->m_BindPoseLocalTransforms, "BindPoseLocalTransforms") ||
                !validateFiniteMat4Array(skeleton->m_BonePreTransforms, "BonePreTransforms"))
            {
                return false;
            }
        }
        if (hasBoneInfo)
        {
            constexpr u32 kMaxBoneCount = 10'000;
            const auto& boneInfo = meshSource->GetBoneInfo();
            auto boneInfoCount = static_cast<u32>(boneInfo.Num());
            if (boneInfoCount > kMaxBoneCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - BoneInfo count ({}) exceeds limit ({}), "
                               "rejecting before write",
                               boneInfoCount, kMaxBoneCount);
                return false;
            }
        }

        outInfo.Offset = stream.GetStreamPosition();

        // Write counts and flags
        stream.WriteRaw<u32>(vertexCount);
        stream.WriteRaw<u32>(indexCount);
        stream.WriteRaw<u32>(submeshCount);
        stream.WriteRaw<u32>(materialCount);
        stream.WriteRaw<bool>(hasBoneInfluences);
        stream.WriteRaw<bool>(hasShadowIndices);
        stream.WriteRaw<bool>(hasSkeleton);
        stream.WriteRaw<bool>(hasBoneInfo);
        stream.WriteRaw<bool>(hasMorphTargets);

        // Encode and write vertex buffer
        constexpr u64 kMaxEncodedSize = 2'000'000'000;
        if (vertexCount > 0)
        {
            auto encodedVB = MeshOptimization::EncodeVertexBuffer(
                vertices.GetData(), vertexCount, sizeof(Vertex));
            auto encodedSize = static_cast<u64>(encodedVB.Data.size());
            if (encodedSize > kMaxEncodedSize)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Encoded vertex buffer size ({}) exceeds limit ({})",
                               encodedSize, kMaxEncodedSize);
                return false;
            }
            stream.WriteRaw<u64>(encodedSize);
            stream.WriteData(reinterpret_cast<const char*>(encodedVB.Data.data()),
                             static_cast<sizet>(encodedSize));
        }

        // Encode and write index buffer
        if (indexCount > 0)
        {
            auto encodedIB = MeshOptimization::EncodeIndexBuffer(
                indices.GetData(), indexCount, vertexCount);
            auto encodedSize = static_cast<u64>(encodedIB.Data.size());
            if (encodedSize > kMaxEncodedSize)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Encoded index buffer size ({}) exceeds limit ({})",
                               encodedSize, kMaxEncodedSize);
                return false;
            }
            stream.WriteRaw<u64>(encodedSize);
            stream.WriteData(reinterpret_cast<const char*>(encodedIB.Data.data()),
                             static_cast<sizet>(encodedSize));
        }

        // Write submeshes
        {
            auto const isFiniteMat4 = [](const glm::mat4& m) -> bool
            {
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        if (!std::isfinite(m[c][r]))
                            return false;
                return true;
            };
            for (i32 i = 0; i < submeshes.Num(); ++i)
            {
                const auto& sub = submeshes[i];
                if (!isFiniteMat4(sub.m_Transform) || !isFiniteMat4(sub.m_LocalTransform))
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Submesh {} has non-finite transform, "
                                   "rejecting before write",
                                   i);
                    return false;
                }
                if (const auto& bb = sub.m_BoundingBox; !std::isfinite(bb.Min.x) || !std::isfinite(bb.Min.y) || !std::isfinite(bb.Min.z) ||
                                                        !std::isfinite(bb.Max.x) || !std::isfinite(bb.Max.y) || !std::isfinite(bb.Max.z) ||
                                                        bb.Min.x > bb.Max.x || bb.Min.y > bb.Max.y || bb.Min.z > bb.Max.z)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Submesh {} has non-finite "
                                   "or inverted bounding box, rejecting before write",
                                   i);
                    return false;
                }
                if (sub.m_BaseVertex > vertexCount || sub.m_VertexCount > vertexCount - sub.m_BaseVertex ||
                    sub.m_BaseIndex > indexCount || sub.m_IndexCount > indexCount - sub.m_BaseIndex)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Submesh {} has out-of-range "
                                   "vertex/index bounds, rejecting before write",
                                   i);
                    return false;
                }
                stream.WriteData(reinterpret_cast<const char*>(&sub.m_Transform), sizeof(glm::mat4));
                stream.WriteData(reinterpret_cast<const char*>(&sub.m_LocalTransform), sizeof(glm::mat4));
                stream.WriteData(reinterpret_cast<const char*>(&sub.m_BoundingBox), sizeof(BoundingBox));
                stream.WriteRaw<u32>(sub.m_BaseVertex);
                stream.WriteRaw<u32>(sub.m_BaseIndex);
                stream.WriteRaw<u32>(sub.m_MaterialIndex);
                stream.WriteRaw<u32>(sub.m_IndexCount);
                stream.WriteRaw<u32>(sub.m_VertexCount);
                {
                    constexpr u32 MAX_SUBMESH_NAME_LEN = 1'024;
                    if (sub.m_NodeName.size() > MAX_SUBMESH_NAME_LEN)
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Submesh {} NodeName length ({}) exceeds limit ({})",
                                       i, sub.m_NodeName.size(), MAX_SUBMESH_NAME_LEN);
                        return false;
                    }
                    if (sub.m_MeshName.size() > MAX_SUBMESH_NAME_LEN)
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Submesh {} MeshName length ({}) exceeds limit ({})",
                                       i, sub.m_MeshName.size(), MAX_SUBMESH_NAME_LEN);
                        return false;
                    }
                }
                stream.WriteString(sub.m_NodeName);
                stream.WriteString(sub.m_MeshName);
                stream.WriteRaw<bool>(sub.m_IsRigged);
            }
        }

        // Write materials
        for (const auto& [index, matHandle] : materials)
        {
            stream.WriteRaw<u32>(index);
            stream.WriteRaw<AssetHandle>(matHandle);
        }

        // Encode and write bone influences
        if (hasBoneInfluences)
        {
            const auto& boneInfluences = meshSource->GetBoneInfluences();
            auto boneCount = static_cast<u32>(boneInfluences.Num());

            if (boneCount != vertexCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::SerializeMeshSource - BoneInfluence count ({}) != vertex count ({})",
                               boneCount, vertexCount);
                return false;
            }

            stream.WriteRaw<u32>(boneCount);

            if (boneCount > 0)
            {
                auto encodedBones = MeshOptimization::EncodeVertexBuffer(
                    boneInfluences.GetData(), boneCount, sizeof(BoneInfluence));
                auto encodedSize = static_cast<u64>(encodedBones.Data.size());
                if (encodedSize > kMaxEncodedSize)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Encoded bone influence size ({}) exceeds limit ({})",
                                   encodedSize, kMaxEncodedSize);
                    return false;
                }
                stream.WriteRaw<u64>(encodedSize);
                stream.WriteData(reinterpret_cast<const char*>(encodedBones.Data.data()),
                                 static_cast<sizet>(encodedSize));
            }
        }

        // Encode and write shadow indices
        if (hasShadowIndices)
        {
            const auto& shadowIndices = meshSource->GetShadowIndices();
            auto shadowCount = static_cast<u32>(shadowIndices.Num());
            stream.WriteRaw<u32>(shadowCount);

            if (shadowCount > 0)
            {
                auto encodedShadow = MeshOptimization::EncodeIndexBuffer(
                    shadowIndices.GetData(), shadowCount, vertexCount);
                auto encodedSize = static_cast<u64>(encodedShadow.Data.size());
                if (encodedSize > kMaxEncodedSize)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Encoded shadow index size ({}) exceeds limit ({})",
                                   encodedSize, kMaxEncodedSize);
                    return false;
                }
                stream.WriteRaw<u64>(encodedSize);
                stream.WriteData(reinterpret_cast<const char*>(encodedShadow.Data.data()),
                                 static_cast<sizet>(encodedSize));
            }
        }

        // Write skeleton
        if (hasSkeleton)
        {
            const auto* skeleton = meshSource->GetSkeleton();
            auto boneCount = static_cast<u32>(skeleton->m_BoneNames.size());

            // Validate skeleton array sizes — reject mismatches instead of inventing data
            auto const validateArraySize = [&boneCount](sizet actual, const char* name) -> bool
            {
                if (static_cast<u32>(actual) != boneCount)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Skeleton {} size ({}) "
                                   "differs from boneCount ({})",
                                   name, actual, boneCount);
                    return false;
                }
                return true;
            };

            if (!validateArraySize(skeleton->m_ParentIndices.size(), "ParentIndices") ||
                !validateArraySize(skeleton->m_LocalTransforms.size(), "LocalTransforms") ||
                !validateArraySize(skeleton->m_GlobalTransforms.size(), "GlobalTransforms") ||
                !validateArraySize(skeleton->m_BindPoseMatrices.size(), "BindPoseMatrices") ||
                !validateArraySize(skeleton->m_InverseBindPoses.size(), "InverseBindPoses") ||
                !validateArraySize(skeleton->m_BindPoseLocalTransforms.size(), "BindPoseLocalTransforms") ||
                !validateArraySize(skeleton->m_BonePreTransforms.size(), "BonePreTransforms"))
            {
                return false;
            }

            stream.WriteRaw<u32>(boneCount);

            // Parent indices
            if (boneCount > 0)
            {
                stream.WriteData(reinterpret_cast<const char*>(skeleton->m_ParentIndices.data()),
                                 boneCount * sizeof(i32));
            }

            // Transform arrays (6 arrays of mat4)
            auto const writeMat4Array = [&stream, &boneCount](const std::vector<glm::mat4>& arr)
            {
                for (u32 j = 0; j < boneCount; ++j)
                {
                    stream.WriteData(reinterpret_cast<const char*>(&arr[j][0][0]), sizeof(glm::mat4));
                }
            };

            writeMat4Array(skeleton->m_LocalTransforms);
            writeMat4Array(skeleton->m_GlobalTransforms);
            writeMat4Array(skeleton->m_BindPoseMatrices);
            writeMat4Array(skeleton->m_InverseBindPoses);
            writeMat4Array(skeleton->m_BindPoseLocalTransforms);
            writeMat4Array(skeleton->m_BonePreTransforms);

            // Bone names
            for (u32 j = 0; j < boneCount; ++j)
            {
                auto nameLen = static_cast<u32>(skeleton->m_BoneNames[j].size());
                stream.WriteRaw<u32>(nameLen);
                if (nameLen > 0)
                {
                    stream.WriteData(skeleton->m_BoneNames[j].data(), nameLen);
                }
            }
        }

        // Write bone info
        if (hasBoneInfo)
        {
            const auto& boneInfo = meshSource->GetBoneInfo();
            auto boneInfoCount = static_cast<u32>(boneInfo.Num());
            stream.WriteRaw<u32>(boneInfoCount);

            // Determine skeleton bone count for index validation
            u32 const skeletonBoneCount = meshSource->HasSkeleton()
                                              ? static_cast<u32>(meshSource->GetSkeleton()->m_BoneNames.size())
                                              : 0;

            for (i32 i = 0; i < boneInfo.Num(); ++i)
            {
                // Validate bone index against skeleton
                if (skeletonBoneCount > 0 && boneInfo[i].m_BoneIndex >= skeletonBoneCount)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - BoneInfo {} has m_BoneIndex {} "
                                   "but skeleton only has {} bones, rejecting before write",
                                   i, boneInfo[i].m_BoneIndex, skeletonBoneCount);
                    return false;
                }
                // Validate inverse bind pose is finite
                bool valid = true;
                for (int c = 0; c < 4 && valid; ++c)
                {
                    for (int r = 0; r < 4 && valid; ++r)
                    {
                        if (!std::isfinite(boneInfo[i].m_InverseBindPose[c][r]))
                        {
                            OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Non-finite value in BoneInfo {} "
                                           "InverseBindPose[{},{}], rejecting before write",
                                           i, c, r);
                            valid = false;
                        }
                    }
                }
                if (!valid)
                {
                    return false;
                }
                stream.WriteData(reinterpret_cast<const char*>(&boneInfo[i].m_InverseBindPose[0][0]), sizeof(glm::mat4));
                stream.WriteRaw<u32>(boneInfo[i].m_BoneIndex);
            }
        }

        // Write morph targets
        if (hasMorphTargets)
        {
            const auto& morphTargets = meshSource->GetMorphTargets();
            auto targetCount = morphTargets->GetTargetCount();
            auto vertCount = morphTargets->GetVertexCount();
            stream.WriteRaw<u32>(targetCount);
            stream.WriteRaw<u32>(vertCount);

            for (u32 t = 0; t < targetCount; ++t)
            {
                const auto& target = morphTargets->Targets[t];
                // If IsSparse but no sparse entries, fall through to dense to avoid reader ambiguity
                auto sparseCount = (target.IsSparse && !target.SparseVertices.empty())
                                       ? static_cast<u32>(target.SparseVertices.size())
                                       : 0u;
                stream.WriteRaw<u32>(sparseCount);

                auto nameLen = static_cast<u32>(target.Name.size());
                stream.WriteRaw<u32>(nameLen);
                if (nameLen > 0)
                {
                    stream.WriteData(target.Name.data(), nameLen);
                }

                if (sparseCount > 0)
                {
                    for (const auto& sparse : target.SparseVertices)
                    {
                        stream.WriteRaw<u32>(sparse.VertexIndex);
                        stream.WriteData(reinterpret_cast<const char*>(&sparse.Delta), sizeof(MorphTargetVertex));
                    }
                }
                else
                {
                    // Dense path: target.Vertices must match vertCount exactly.
                    if (static_cast<u32>(target.Vertices.size()) != vertCount)
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::SerializeToAssetPack - Dense morph target '{}' vertex count ({}) "
                                       "does not match expected vertCount ({})",
                                       target.Name, target.Vertices.size(), vertCount);
                        return false;
                    }
                    if (vertCount > 0)
                    {
                        stream.WriteData(reinterpret_cast<const char*>(target.Vertices.data()),
                                         vertCount * sizeof(MorphTargetVertex));
                    }
                }
            }
        }

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        OLO_CORE_TRACE("MeshSourceSerializer::SerializeToAssetPack: {} verts, {} indices, {} bytes (encoded)",
                       vertexCount, indexCount, outInfo.Size);
        return true;
    }

    // ── Asset-pack deserialization helpers ──────────────────────────────────
    // Each helper encapsulates reads, validations and logging for one logical
    // block of the packed MeshSource format.  Returns true on success.

    namespace
    {
        constexpr u64 kMaxEncodedSize = 2'000'000'000; // 2 GB
        constexpr u32 kMaxVertexCount = 50'000'000;
        constexpr u32 kMaxIndexCount = 150'000'000;
        constexpr u32 kMaxSubmeshCount = 10'000;
        constexpr u32 kMaxMaterialCount = 10'000;
        constexpr u32 kMaxBoneCount = 10'000;

        bool DecodeVertexBufferFromPack(FileStreamReader& stream, u32 vertexCount, TArray<Vertex>& outVertices)
        {
            if (vertexCount == 0)
            {
                return true;
            }

            u64 encodedSize = 0;
            stream.ReadRaw<u64>(encodedSize);

            if (encodedSize > kMaxEncodedSize)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Vertex encodedSize ({}) exceeds limit",
                               encodedSize);
                return false;
            }

            EncodedMeshBuffer encoded;
            encoded.Data.resize(static_cast<sizet>(encodedSize));
            encoded.OriginalSize = vertexCount * sizeof(Vertex);
            stream.ReadData(reinterpret_cast<char*>(encoded.Data.data()),
                            static_cast<sizet>(encodedSize));

            outVertices.SetNum(static_cast<i32>(vertexCount));
            if (!MeshOptimization::DecodeVertexBuffer(outVertices.GetData(), vertexCount, sizeof(Vertex), encoded))
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Failed to decode vertex buffer");
                return false;
            }
            return true;
        }

        bool DecodeIndexBufferFromPack(FileStreamReader& stream, u32 indexCount, TArray<u32>& outIndices, u32 vertexCount)
        {
            if (indexCount == 0)
            {
                return true;
            }

            u64 encodedSize = 0;
            stream.ReadRaw<u64>(encodedSize);

            if (encodedSize > kMaxEncodedSize)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Index encodedSize ({}) exceeds limit",
                               encodedSize);
                return false;
            }

            EncodedMeshBuffer encoded;
            encoded.Data.resize(static_cast<sizet>(encodedSize));
            encoded.OriginalSize = indexCount * sizeof(u32);
            stream.ReadData(reinterpret_cast<char*>(encoded.Data.data()),
                            static_cast<sizet>(encodedSize));

            outIndices.SetNum(static_cast<i32>(indexCount));
            if (!MeshOptimization::DecodeIndexBuffer(outIndices.GetData(), indexCount, encoded))
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Failed to decode index buffer");
                return false;
            }

            // Validate decoded indices against vertex count
            for (u32 i = 0; i < indexCount; ++i)
            {
                if (outIndices[static_cast<i32>(i)] >= vertexCount)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Index {} out of range "
                                   "(value={}, vertexCount={})",
                                   i, outIndices[static_cast<i32>(i)], vertexCount);
                    return false;
                }
            }
            return true;
        }

        bool ReadSubmeshesFromPack(FileStreamReader& stream, u32 submeshCount, Ref<MeshSource>& meshSource,
                                   u32 vertexCount, u32 indexCount)
        {
            for (u32 i = 0; i < submeshCount; ++i)
            {
                Submesh sub;
                stream.ReadData(reinterpret_cast<char*>(&sub.m_Transform), sizeof(glm::mat4));
                stream.ReadData(reinterpret_cast<char*>(&sub.m_LocalTransform), sizeof(glm::mat4));
                stream.ReadData(reinterpret_cast<char*>(&sub.m_BoundingBox), sizeof(BoundingBox));
                stream.ReadRaw<u32>(sub.m_BaseVertex);
                stream.ReadRaw<u32>(sub.m_BaseIndex);
                stream.ReadRaw<u32>(sub.m_MaterialIndex);
                stream.ReadRaw<u32>(sub.m_IndexCount);
                stream.ReadRaw<u32>(sub.m_VertexCount);
                stream.ReadString(sub.m_NodeName);
                stream.ReadString(sub.m_MeshName);
                {
                    constexpr u32 MAX_SUBMESH_NAME_LEN = 1'024;
                    if (sub.m_NodeName.size() > MAX_SUBMESH_NAME_LEN)
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Submesh {} NodeName length ({}) exceeds limit ({})",
                                       i, sub.m_NodeName.size(), MAX_SUBMESH_NAME_LEN);
                        return false;
                    }
                    if (sub.m_MeshName.size() > MAX_SUBMESH_NAME_LEN)
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Submesh {} MeshName length ({}) exceeds limit ({})",
                                       i, sub.m_MeshName.size(), MAX_SUBMESH_NAME_LEN);
                        return false;
                    }
                }
                stream.ReadRaw<bool>(sub.m_IsRigged);

                // Validate submesh transform and bounding box floats for NaN/Inf
                {
                    auto const isFiniteMat4 = [](const glm::mat4& m) -> bool
                    {
                        for (int c = 0; c < 4; ++c)
                            for (int r = 0; r < 4; ++r)
                                if (!std::isfinite(m[c][r]))
                                    return false;
                        return true;
                    };
                    if (!isFiniteMat4(sub.m_Transform) || !isFiniteMat4(sub.m_LocalTransform))
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Submesh {} has non-finite transform", i);
                        return false;
                    }
                    const auto& bb = sub.m_BoundingBox;
                    if (!std::isfinite(bb.Min.x) || !std::isfinite(bb.Min.y) || !std::isfinite(bb.Min.z) ||
                        !std::isfinite(bb.Max.x) || !std::isfinite(bb.Max.y) || !std::isfinite(bb.Max.z) ||
                        bb.Min.x > bb.Max.x || bb.Min.y > bb.Max.y || bb.Min.z > bb.Max.z)
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Submesh {} has non-finite "
                                       "or inverted bounding box",
                                       i);
                        return false;
                    }
                }

                // Validate submesh ranges against total counts to catch corrupt data (overflow-safe)
                if (sub.m_BaseVertex > vertexCount || sub.m_VertexCount > vertexCount - sub.m_BaseVertex ||
                    sub.m_BaseIndex > indexCount || sub.m_IndexCount > indexCount - sub.m_BaseIndex)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Submesh {} has out-of-range "
                                   "vertex/index bounds (BaseVertex={}, VertexCount={}, BaseIndex={}, IndexCount={})",
                                   i, sub.m_BaseVertex, sub.m_VertexCount, sub.m_BaseIndex, sub.m_IndexCount);
                    return false;
                }

                meshSource->AddSubmesh(sub);
            }
            return true;
        }

        bool ReadBoneInfluencesFromPack(FileStreamReader& stream, Ref<MeshSource>& meshSource, u32 vertexCount)
        {
            u32 boneCount = 0;
            stream.ReadRaw<u32>(boneCount);

            // Bone influences should be 1:1 with vertices
            if (boneCount != vertexCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - BoneInfluence count ({}) != vertex count ({})",
                               boneCount, vertexCount);
                return false;
            }

            if (boneCount > 0)
            {
                u64 encodedSize = 0;
                stream.ReadRaw<u64>(encodedSize);

                if (encodedSize > kMaxEncodedSize)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - BoneInfluence encodedSize ({}) exceeds limit",
                                   encodedSize);
                    return false;
                }

                EncodedMeshBuffer encoded;
                encoded.Data.resize(static_cast<sizet>(encodedSize));
                encoded.OriginalSize = boneCount * sizeof(BoneInfluence);
                stream.ReadData(reinterpret_cast<char*>(encoded.Data.data()),
                                static_cast<sizet>(encodedSize));

                auto& boneInfluences = meshSource->GetBoneInfluences();
                boneInfluences.SetNum(static_cast<i32>(boneCount));
                if (!MeshOptimization::DecodeVertexBuffer(boneInfluences.GetData(), boneCount, sizeof(BoneInfluence), encoded))
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Failed to decode bone influences");
                    return false;
                }
            }
            return true;
        }

        bool ReadShadowIndicesFromPack(FileStreamReader& stream, Ref<MeshSource>& meshSource, u32 vertexCount)
        {
            u32 shadowCount = 0;
            stream.ReadRaw<u32>(shadowCount);

            if (shadowCount > kMaxIndexCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Shadow index count ({}) exceeds limit",
                               shadowCount);
                return false;
            }

            if (shadowCount > 0)
            {
                u64 encodedSize = 0;
                stream.ReadRaw<u64>(encodedSize);

                if (encodedSize > kMaxEncodedSize)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Shadow encodedSize ({}) exceeds limit",
                                   encodedSize);
                    return false;
                }

                EncodedMeshBuffer encoded;
                encoded.Data.resize(static_cast<sizet>(encodedSize));
                encoded.OriginalSize = shadowCount * sizeof(u32);
                stream.ReadData(reinterpret_cast<char*>(encoded.Data.data()),
                                static_cast<sizet>(encodedSize));

                auto& shadowIndices = meshSource->GetShadowIndices();
                shadowIndices.SetNum(static_cast<i32>(shadowCount));
                if (!MeshOptimization::DecodeIndexBuffer(shadowIndices.GetData(), shadowCount, encoded))
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Failed to decode shadow indices");
                    return false;
                }

                // Validate shadow index range against vertex count
                for (u32 si = 0; si < shadowCount; ++si)
                {
                    if (shadowIndices[static_cast<i32>(si)] >= vertexCount)
                    {
                        OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Shadow index {} out of range "
                                       "(value={}, vertexCount={})",
                                       si, shadowIndices[static_cast<i32>(si)], vertexCount);
                        return false;
                    }
                }
            }
            return true;
        }

        bool ReadSkeletonFromPack(FileStreamReader& stream, Ref<MeshSource>& meshSource)
        {
            u32 boneCount = 0;
            stream.ReadRaw<u32>(boneCount);

            if (boneCount > kMaxBoneCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Bone count {} exceeds limit {}",
                               boneCount, kMaxBoneCount);
                return false;
            }

            auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(boneCount));

            // Parent indices — read into place, then validate bounds
            stream.ReadData(reinterpret_cast<char*>(skeleton->m_ParentIndices.data()),
                            boneCount * sizeof(i32));

            for (u32 j = 0; j < boneCount; ++j)
            {
                auto const idx = skeleton->m_ParentIndices[j];
                if (idx != -1 && (idx < 0 || idx >= static_cast<i32>(boneCount)))
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - ParentIndex[{}] = {} "
                                   "is out of range [0, {}) or -1",
                                   j, idx, boneCount);
                    return false;
                }
            }

            // Transform arrays
            auto const readMat4Array = [&stream, &boneCount](std::vector<glm::mat4>& arr)
            {
                arr.resize(boneCount);
                stream.ReadData(reinterpret_cast<char*>(arr.data()), boneCount * sizeof(glm::mat4));
            };

            readMat4Array(skeleton->m_LocalTransforms);
            readMat4Array(skeleton->m_GlobalTransforms);
            readMat4Array(skeleton->m_BindPoseMatrices);
            readMat4Array(skeleton->m_InverseBindPoses);
            readMat4Array(skeleton->m_BindPoseLocalTransforms);
            readMat4Array(skeleton->m_BonePreTransforms);

            // Validate all deserialized matrices for non-finite values
            auto const validateMat4Array = [&](const std::vector<glm::mat4>& arr, const char* name) -> bool
            {
                for (sizet i = 0; i < arr.size(); ++i)
                {
                    const auto& m = arr[i];
                    for (int c = 0; c < 4; ++c)
                    {
                        for (int r = 0; r < 4; ++r)
                        {
                            if (!std::isfinite(m[c][r]))
                            {
                                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Non-finite value in skeleton {} "
                                               "at bone {} element [{},{}]",
                                               name, i, c, r);
                                return false;
                            }
                        }
                    }
                }
                return true;
            };

            if (!validateMat4Array(skeleton->m_LocalTransforms, "LocalTransforms") ||
                !validateMat4Array(skeleton->m_GlobalTransforms, "GlobalTransforms") ||
                !validateMat4Array(skeleton->m_BindPoseMatrices, "BindPoseMatrices") ||
                !validateMat4Array(skeleton->m_InverseBindPoses, "InverseBindPoses") ||
                !validateMat4Array(skeleton->m_BindPoseLocalTransforms, "BindPoseLocalTransforms") ||
                !validateMat4Array(skeleton->m_BonePreTransforms, "BonePreTransforms"))
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Corrupt skeleton data, rejecting");
                return false;
            }

            // Bone names
            constexpr u32 MAX_BONE_NAME_LEN = 1'024;
            skeleton->m_BoneNames.resize(boneCount);
            for (u32 j = 0; j < boneCount; ++j)
            {
                u32 nameLen = 0;
                stream.ReadRaw<u32>(nameLen);
                if (nameLen > MAX_BONE_NAME_LEN)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Bone name length ({}) "
                                   "exceeds limit at bone {}",
                                   nameLen, j);
                    return false;
                }
                if (nameLen > 0)
                {
                    skeleton->m_BoneNames[j].resize(nameLen);
                    stream.ReadData(skeleton->m_BoneNames[j].data(), nameLen);
                }
            }

            // Compute bind-pose FinalBoneMatrices
            skeleton->m_FinalBoneMatrices.resize(boneCount);
            for (u32 j = 0; j < boneCount; ++j)
            {
                skeleton->m_FinalBoneMatrices[j] = skeleton->m_GlobalTransforms[j] * skeleton->m_InverseBindPoses[j];
            }

            meshSource->SetSkeleton(skeleton);
            return true;
        }

        bool ReadBoneInfoFromPack(FileStreamReader& stream, Ref<MeshSource>& meshSource)
        {
            u32 boneInfoCount = 0;
            stream.ReadRaw<u32>(boneInfoCount);

            if (boneInfoCount > kMaxBoneCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - boneInfoCount {} exceeds limit {}",
                               boneInfoCount, kMaxBoneCount);
                return false;
            }

            // Determine skeleton bone count for index validation
            u32 const skeletonBoneCount = meshSource->HasSkeleton()
                                              ? static_cast<u32>(meshSource->GetSkeleton()->m_BoneNames.size())
                                              : 0;

            auto& boneInfo = meshSource->GetBoneInfo();
            for (u32 i = 0; i < boneInfoCount; ++i)
            {
                BoneInfo info;
                stream.ReadData(reinterpret_cast<char*>(&info.m_InverseBindPose[0][0]), sizeof(glm::mat4));
                stream.ReadRaw<u32>(info.m_BoneIndex);

                // Validate bone index against skeleton
                if (skeletonBoneCount > 0 && info.m_BoneIndex >= skeletonBoneCount)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - BoneInfo {} has m_BoneIndex {} "
                                   "but skeleton only has {} bones",
                                   i, info.m_BoneIndex, skeletonBoneCount);
                    return false;
                }

                // Validate inverse bind pose matrix for non-finite values
                bool valid = true;
                for (int c = 0; c < 4 && valid; ++c)
                {
                    for (int r = 0; r < 4 && valid; ++r)
                    {
                        if (!std::isfinite(info.m_InverseBindPose[c][r]))
                        {
                            OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Non-finite value in BoneInfo {} "
                                           "InverseBindPose[{},{}]",
                                           i, c, r);
                            valid = false;
                        }
                    }
                }
                if (!valid)
                {
                    return false;
                }

                boneInfo.Add(info);
            }
            return true;
        }

        bool ReadMorphTargetsFromPack(FileStreamReader& stream, Ref<MeshSource>& meshSource, u32 vertexCount)
        {
            u32 targetCount = 0;
            u32 vertCount = 0;
            stream.ReadRaw<u32>(targetCount);
            stream.ReadRaw<u32>(vertCount);

            constexpr u32 MAX_MORPH_TARGETS = 1'000;
            constexpr u32 MAX_NAME_LEN = 1'024;

            if (targetCount > MAX_MORPH_TARGETS)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Morph targetCount ({}) exceeds limit",
                               targetCount);
                return false;
            }
            if (vertCount != vertexCount)
            {
                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Morph vertCount ({}) does not match vertexCount ({})",
                               vertCount, vertexCount);
                return false;
            }

            auto morphTargetSet = Ref<MorphTargetSet>::Create();

            auto const validateMorphVertex = [](const MorphTargetVertex& v) -> bool
            {
                return std::isfinite(v.DeltaPosition.x) && std::isfinite(v.DeltaPosition.y) && std::isfinite(v.DeltaPosition.z) &&
                       std::isfinite(v.DeltaNormal.x) && std::isfinite(v.DeltaNormal.y) && std::isfinite(v.DeltaNormal.z) &&
                       std::isfinite(v.DeltaTangent.x) && std::isfinite(v.DeltaTangent.y) && std::isfinite(v.DeltaTangent.z);
            };

            for (u32 t = 0; t < targetCount; ++t)
            {
                u32 sparseCount = 0;
                stream.ReadRaw<u32>(sparseCount);

                if (sparseCount > vertexCount)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Morph sparseCount ({}) exceeds vertexCount ({})",
                                   sparseCount, vertexCount);
                    return false;
                }

                MorphTarget target;
                u32 nameLen = 0;
                stream.ReadRaw<u32>(nameLen);

                if (nameLen > MAX_NAME_LEN)
                {
                    OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Morph target name length ({}) exceeds limit",
                                   nameLen);
                    return false;
                }
                if (nameLen > 0)
                {
                    target.Name.resize(nameLen);
                    stream.ReadData(target.Name.data(), nameLen);
                }

                if (sparseCount > 0)
                {
                    target.IsSparse = true;
                    target.SparseVertices.resize(sparseCount);
                    for (u32 s = 0; s < sparseCount; ++s)
                    {
                        stream.ReadRaw<u32>(target.SparseVertices[s].VertexIndex);
                        stream.ReadData(reinterpret_cast<char*>(&target.SparseVertices[s].Delta), sizeof(MorphTargetVertex));

                        if (target.SparseVertices[s].VertexIndex >= vertexCount)
                        {
                            OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Sparse morph vertex index {} "
                                           "out of range (vertexCount={})",
                                           target.SparseVertices[s].VertexIndex, vertexCount);
                            return false;
                        }
                        if (!validateMorphVertex(target.SparseVertices[s].Delta))
                        {
                            OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Non-finite morph delta at "
                                           "sparse vertex index {}",
                                           target.SparseVertices[s].VertexIndex);
                            return false;
                        }
                    }
                }
                else
                {
                    target.IsSparse = false;
                    target.Vertices.resize(vertCount);
                    if (vertCount > 0)
                    {
                        stream.ReadData(reinterpret_cast<char*>(target.Vertices.data()),
                                        vertCount * sizeof(MorphTargetVertex));

                        for (u32 v = 0; v < vertCount; ++v)
                        {
                            if (!validateMorphVertex(target.Vertices[v]))
                            {
                                OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Non-finite morph delta at "
                                               "dense vertex index {}",
                                               v);
                                return false;
                            }
                        }
                    }
                }

                morphTargetSet->AddTarget(std::move(target));
            }

            meshSource->SetMorphTargets(morphTargetSet);
            return true;
        }
    } // anonymous namespace

    Ref<Asset> MeshSourceSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read counts and flags
        u32 vertexCount = 0;
        u32 indexCount = 0;
        u32 submeshCount = 0;
        u32 materialCount = 0;
        bool hasBoneInfluences = false;
        bool hasShadowIndices = false;
        bool hasSkeleton = false;
        bool hasBoneInfo = false;
        bool hasMorphTargets = false;

        stream.ReadRaw<u32>(vertexCount);
        stream.ReadRaw<u32>(indexCount);
        stream.ReadRaw<u32>(submeshCount);
        stream.ReadRaw<u32>(materialCount);
        stream.ReadRaw<bool>(hasBoneInfluences);
        stream.ReadRaw<bool>(hasShadowIndices);
        stream.ReadRaw<bool>(hasSkeleton);
        stream.ReadRaw<bool>(hasBoneInfo);
        stream.ReadRaw<bool>(hasMorphTargets);

        // Bounds-check deserialized counts to detect corrupt data early
        if (vertexCount > kMaxVertexCount || indexCount > kMaxIndexCount ||
            submeshCount > kMaxSubmeshCount || materialCount > kMaxMaterialCount)
        {
            OLO_CORE_ERROR("MeshSourceSerializer::DeserializeFromAssetPack - Suspicious counts "
                           "(verts={}, indices={}, submeshes={}, materials={})",
                           vertexCount, indexCount, submeshCount, materialCount);
            return nullptr;
        }

        // Decode vertex and index buffers
        TArray<Vertex> vertices;
        if (!DecodeVertexBufferFromPack(stream, vertexCount, vertices))
        {
            return nullptr;
        }

        TArray<u32> indices;
        if (!DecodeIndexBufferFromPack(stream, indexCount, indices, vertexCount))
        {
            return nullptr;
        }

        auto meshSource = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));

        // Read submeshes
        if (!ReadSubmeshesFromPack(stream, submeshCount, meshSource, vertexCount, indexCount))
        {
            return nullptr;
        }

        // Read materials
        for (u32 i = 0; i < materialCount; ++i)
        {
            u32 matIndex = 0;
            AssetHandle matHandle{};
            stream.ReadRaw<u32>(matIndex);
            stream.ReadRaw<AssetHandle>(matHandle);
            meshSource->SetMaterial(matIndex, matHandle);
        }

        // Decode bone influences
        if (hasBoneInfluences && !ReadBoneInfluencesFromPack(stream, meshSource, vertexCount))
        {
            return nullptr;
        }

        // Decode shadow indices
        if (hasShadowIndices && !ReadShadowIndicesFromPack(stream, meshSource, vertexCount))
        {
            return nullptr;
        }

        // Read skeleton
        if (hasSkeleton && !ReadSkeletonFromPack(stream, meshSource))
        {
            return nullptr;
        }

        // Read bone info
        if (hasBoneInfo && !ReadBoneInfoFromPack(stream, meshSource))
        {
            return nullptr;
        }

        // Read morph targets
        if (hasMorphTargets && !ReadMorphTargetsFromPack(stream, meshSource, vertexCount))
        {
            return nullptr;
        }

        meshSource->SetHandle(assetInfo.Handle);

        // Asset pack data is already optimized — skip re-optimization during Build()
        meshSource->SetPreOptimized(true);
        meshSource->Build();

        OLO_CORE_TRACE("MeshSourceSerializer::DeserializeFromAssetPack: {} verts, {} indices",
                       vertexCount, indexCount);
        return meshSource;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MeshSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void MeshSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<Mesh> mesh = asset.As<Mesh>();
        if (!mesh)
        {
            OLO_CORE_ERROR("MeshSerializer::Serialize - Invalid mesh asset");
            return;
        }

        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;

        // Mesh is a lightweight reference: a MeshSource handle + a single submesh index.
        // The MeshSource owns the geometry/skeleton/bones; submesh/material containers live on StaticMesh.
        AssetHandle meshSourceHandle = mesh->GetMeshSource() ? mesh->GetMeshSource()->GetHandle() : AssetHandle(0);

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Mesh" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "MeshSource" << YAML::Value << meshSourceHandle;
        out << YAML::Key << "SubmeshIndex" << YAML::Value << mesh->GetSubmeshIndex();
        out << YAML::EndMap;
        out << YAML::EndMap;

        // Ensure parent dir exists. When EditorAssetManager registers a brand-new
        // .olomesh under Assets/<some>/<sub>/foo.olomesh and the subfolder hasn't been
        // created yet (e.g. via duplicate from another folder), the ofstream silently
        // fails. Surface the failure instead of dropping the save.
        std::error_code ec;
        if (auto parent = filepath.parent_path(); !parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                OLO_CORE_ERROR("MeshSerializer::Serialize - Failed to create parent directory '{}': {}", parent.string(), ec.message());
                return;
            }
        }

        std::ofstream fout(filepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("MeshSerializer::Serialize - Failed to open file for writing: {}", filepath.string());
            return;
        }
        fout << out.c_str();
        fout.close();
        if (fout.fail())
        {
            OLO_CORE_ERROR("MeshSerializer::Serialize - Write failed for: {}", filepath.string());
            return;
        }

        OLO_CORE_TRACE("MeshSerializer::Serialize - Serialized mesh to {} (MeshSource: {}, SubmeshIndex: {})",
                       filepath.string(), meshSourceHandle, mesh->GetSubmeshIndex());
    }

    bool MeshSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(path.string());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - YAML parsing error in {}: {}", path.string(), e.what());
            return false;
        }

        YAML::Node meshNode = data["Mesh"];
        if (!meshNode)
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - No Mesh node found in {}", path.string());
            return false;
        }

        AssetHandle meshSourceHandle = meshNode["MeshSource"].as<u64>(0);
        if (meshSourceHandle == 0)
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - Invalid or missing MeshSource handle in {}", path.string());
            return false;
        }

        u32 submeshIndex = meshNode["SubmeshIndex"].as<u32>(0);

        Ref<MeshSource> meshSource = AssetManager::GetAsset<MeshSource>(meshSourceHandle);
        if (!meshSource)
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - Failed to load MeshSource {} referenced by {}", meshSourceHandle, path.string());
            return false;
        }

        // Mesh ctor asserts the submesh index is in range; clamp + warn instead so a stale asset
        // file doesn't take down the editor when a MeshSource is re-imported with fewer submeshes.
        const i32 submeshCount = meshSource->GetSubmeshes().Num();
        if (submeshCount <= 0)
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - MeshSource {} has no submeshes ({})", meshSourceHandle, path.string());
            return false;
        }
        if (submeshIndex >= static_cast<u32>(submeshCount))
        {
            OLO_CORE_WARN("MeshSerializer::TryLoadData - SubmeshIndex {} out of range (MeshSource has {} submeshes), clamping to 0",
                          submeshIndex, submeshCount);
            submeshIndex = 0;
        }

        Ref<Mesh> mesh = Ref<Mesh>(new Mesh(meshSource, submeshIndex));
        mesh->SetHandle(metadata.Handle);
        asset = mesh;

        OLO_CORE_TRACE("MeshSerializer::TryLoadData - Loaded Mesh from {} (MeshSource: {}, SubmeshIndex: {})",
                       path.string(), meshSourceHandle, submeshIndex);
        return true;
    }

    void MeshSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        // MeshSerializer is registered only for AssetType::Mesh; StaticMesh has its own serializer.
        if (metadata.Type != AssetType::Mesh)
        {
            OLO_CORE_WARN("MeshSerializer::RegisterDependencies - Unexpected asset type: {}", (int)metadata.Type);
            return;
        }

        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("MeshSerializer::RegisterDependencies - File does not exist: {}", path.string());
            return;
        }

        try
        {
            AssetManager::DeregisterDependencies(metadata.Handle);

            YAML::Node yamlData = YAML::LoadFile(path.string());
            YAML::Node meshNode = yamlData["Mesh"];
            if (!meshNode)
            {
                OLO_CORE_WARN("MeshSerializer::RegisterDependencies - Missing Mesh node in {}", path.string());
                return;
            }

            AssetHandle meshSourceHandle = meshNode["MeshSource"].as<u64>(0);
            if (meshSourceHandle != 0)
            {
                AssetManager::RegisterDependency(meshSourceHandle, metadata.Handle);
                OLO_CORE_TRACE("MeshSerializer: Registered MeshSource dependency - Mesh {0} depends on MeshSource {1}", metadata.Handle, meshSourceHandle);
            }
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshSerializer::RegisterDependencies - YAML parsing error in {}: {}", path.string(), e.what());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("MeshSerializer::RegisterDependencies - Error in {}: {}", path.string(), e.what());
        }
    }

    bool MeshSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        outInfo.Offset = stream.GetStreamPosition();

        Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(handle);
        if (!mesh)
        {
            OLO_CORE_ERROR("MeshSerializer: Failed to get Mesh asset for handle {0}", handle);
            return false;
        }

        // Serialize mesh properties
        // For basic Mesh, we store the MeshSource handle and submesh index
        AssetHandle meshSourceHandle = 0;
        if (mesh->GetMeshSource())
        {
            meshSourceHandle = mesh->GetMeshSource()->GetHandle();
        }

        stream.WriteRaw<AssetHandle>(meshSourceHandle);
        stream.WriteRaw<u32>(mesh->GetSubmeshIndex());

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("MeshSerializer: Serialized Mesh to pack - Handle: {0}, MeshSource: {1}, SubmeshIndex: {2}, Size: {3}",
                       handle, meshSourceHandle, mesh->GetSubmeshIndex(), outInfo.Size);
        return true;
    }

    Ref<Asset> MeshSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read mesh properties
        AssetHandle meshSourceHandle;
        stream.ReadRaw<AssetHandle>(meshSourceHandle);

        u32 submeshIndex;
        stream.ReadRaw<u32>(submeshIndex);

        // Create Mesh asset
        Ref<MeshSource> meshSource = nullptr;
        if (meshSourceHandle != 0)
        {
            meshSource = AssetManager::GetAsset<MeshSource>(meshSourceHandle);
        }

        Ref<Mesh> mesh = Ref<Mesh>(new Mesh(meshSource, submeshIndex));
        mesh->SetHandle(assetInfo.Handle);

        OLO_CORE_TRACE("MeshSerializer: Deserialized Mesh from pack - Handle: {0}, MeshSource: {1}, SubmeshIndex: {2}",
                       assetInfo.Handle, meshSourceHandle, submeshIndex);
        return mesh;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // StaticMeshSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void StaticMeshSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<StaticMesh> staticMesh = asset.As<StaticMesh>();
        if (!staticMesh)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::Serialize - Asset is not a valid StaticMesh");
            return;
        }

        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        try
        {
            YAML::Emitter out;
            out << YAML::BeginMap;
            out << YAML::Key << "StaticMesh" << YAML::Value;
            out << YAML::BeginMap;

            // Serialize mesh source handle
            out << YAML::Key << "MeshSource" << YAML::Value << staticMesh->GetMeshSource();

            // Serialize collider generation flag
            out << YAML::Key << "GenerateColliders" << YAML::Value << staticMesh->ShouldGenerateColliders();

            // Serialize submesh indices if not using all submeshes
            if (const auto& submeshIndices = staticMesh->GetSubmeshes(); !submeshIndices.IsEmpty())
            {
                out << YAML::Key << "Submeshes" << YAML::Value;
                out << YAML::BeginSeq;
                for (u32 index : submeshIndices)
                {
                    out << index;
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap;
            out << YAML::EndMap;

            std::ofstream fout(path);
            fout << out.c_str();
            fout.close();

            OLO_CORE_TRACE("StaticMeshSerializer::Serialize - Successfully serialized static mesh to: {}", path.string());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::Serialize - Error serializing static mesh {}: {}", path.string(), e.what());
        }
    }

    bool StaticMeshSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        try
        {
            // Load YAML file with static mesh configuration
            YAML::Node yamlData = YAML::LoadFile(path.string());

            if (!yamlData["StaticMesh"])
            {
                OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Invalid static mesh file (missing StaticMesh node): {}", path.string());
                return false;
            }

            YAML::Node meshNode = yamlData["StaticMesh"];

            // Get the mesh source handle
            AssetHandle meshSourceHandle = meshNode["MeshSource"].as<u64>(0);
            if (meshSourceHandle == 0)
            {
                OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Invalid mesh source handle in: {}", path.string());
                return false;
            }

            // Get optional settings
            bool generateColliders = meshNode["GenerateColliders"].as<bool>(false);

            // Get submesh indices (optional)
            TArray<u32> submeshIndices;
            if (meshNode["Submeshes"])
            {
                for (const auto& submeshNode : meshNode["Submeshes"])
                {
                    submeshIndices.Add(submeshNode.as<u32>());
                }
            }

            // Create the static mesh
            Ref<StaticMesh> staticMesh;
            if (submeshIndices.IsEmpty())
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, generateColliders);
            }
            else
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, submeshIndices, generateColliders);
            }

            staticMesh->SetHandle(metadata.Handle);
            asset = staticMesh;

            OLO_CORE_TRACE("StaticMeshSerializer::TryLoadData - Successfully loaded static mesh: {}", path.string());
            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - YAML parsing error in {}: {}", path.string(), e.what());
            return false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Error loading static mesh {}: {}", path.string(), e.what());
            return false;
        }
    }

    void StaticMeshSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        std::filesystem::path path = Project::GetProjectDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("StaticMeshSerializer::RegisterDependencies - File does not exist: {}", path.string());
            return;
        }

        try
        {
            // Deregister existing dependencies first
            AssetManager::DeregisterDependencies(metadata.Handle);

            YAML::Node yamlData = YAML::LoadFile(path.string());

            if (!yamlData["StaticMesh"])
            {
                OLO_CORE_WARN("StaticMeshSerializer::RegisterDependencies - Invalid static mesh file: {}", path.string());
                return;
            }

            YAML::Node meshNode = yamlData["StaticMesh"];

            // Register mesh source dependency
            if (AssetHandle meshSourceHandle = meshNode["MeshSource"].as<u64>(0); meshSourceHandle != 0)
            {
                AssetManager::RegisterDependency(meshSourceHandle, metadata.Handle);
                OLO_CORE_TRACE("StaticMeshSerializer: Registered MeshSource dependency - StaticMesh {0} depends on MeshSource {1}", metadata.Handle, meshSourceHandle);
            }

            // Register material dependencies from MaterialTable
            if (meshNode["MaterialTable"])
            {
                YAML::Node materialTable = meshNode["MaterialTable"];

                if (materialTable["Materials"] && materialTable["Materials"].IsMap())
                {
                    for (const auto& materialEntry : materialTable["Materials"])
                    {
                        u32 materialIndex = materialEntry.first.as<u32>();
                        AssetHandle materialHandle = materialEntry.second.as<AssetHandle>(0);

                        if (materialHandle != 0)
                        {
                            AssetManager::RegisterDependency(materialHandle, metadata.Handle);
                            OLO_CORE_TRACE("StaticMeshSerializer: Registered material dependency - StaticMesh {0} depends on Material {1} at index {2}", metadata.Handle, materialHandle, materialIndex);
                        }
                    }
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::RegisterDependencies - YAML parsing error in {}: {}", path.string(), e.what());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::RegisterDependencies - Error in {}: {}", path.string(), e.what());
        }
    }

    bool StaticMeshSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<StaticMesh> staticMesh = AssetManager::GetAsset<StaticMesh>(handle);
        if (!staticMesh)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::SerializeToAssetPack - Failed to load static mesh asset {}", handle);
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();

        try
        {
            // Write mesh source handle
            stream.WriteRaw<AssetHandle>(staticMesh->GetMeshSource());

            // Write collider generation flag
            stream.WriteRaw<bool>(staticMesh->ShouldGenerateColliders());

            // Write submesh indices
            const auto& submeshIndices = staticMesh->GetSubmeshes();
            stream.WriteRaw<u32>(static_cast<u32>(submeshIndices.Num()));
            for (u32 index : submeshIndices)
            {
                stream.WriteRaw<u32>(index);
            }

            outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::SerializeToAssetPack - Error serializing static mesh {}: {}", handle, e.what());
            return false;
        }
    }

    Ref<Asset> StaticMeshSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        try
        {
            // Read mesh source handle
            AssetHandle meshSourceHandle;
            stream.ReadRaw(meshSourceHandle);

            // Read collider generation flag
            bool generateColliders;
            stream.ReadRaw(generateColliders);

            // Read submesh indices
            u32 submeshCount;
            stream.ReadRaw(submeshCount);
            TArray<u32> submeshIndices;
            submeshIndices.Reserve(static_cast<i32>(submeshCount));

            for (u32 i = 0; i < submeshCount; ++i)
            {
                u32 submeshIndex;
                stream.ReadRaw(submeshIndex);
                submeshIndices.Add(submeshIndex);
            }

            // Create static mesh
            Ref<StaticMesh> staticMesh;
            if (submeshIndices.IsEmpty())
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, generateColliders);
            }
            else
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, submeshIndices, generateColliders);
            }

            staticMesh->SetHandle(assetInfo.Handle);
            return staticMesh;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::DeserializeFromAssetPack - Error deserializing static mesh: {}", e.what());
            return nullptr;
        }
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AnimationAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    std::string AnimationAssetSerializer::SerializeToYAML(Ref<AnimationAsset> animationAsset) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        {
            out << YAML::Key << "Animation" << YAML::Value;
            out << YAML::BeginMap;
            {
                out << YAML::Key << "AnimationSource" << YAML::Value << animationAsset->GetAnimationSource();
                out << YAML::Key << "Mesh" << YAML::Value << animationAsset->GetMeshHandle();
                out << YAML::Key << "AnimationName" << YAML::Value << animationAsset->GetAnimationName();
                out << YAML::Key << "ExtractRootMotion" << YAML::Value << animationAsset->IsExtractRootMotion();
                out << YAML::Key << "RootBoneIndex" << YAML::Value << animationAsset->GetRootBoneIndex();
                out << YAML::Key << "RootTranslationMask" << YAML::Value << animationAsset->GetRootTranslationMask();
                out << YAML::Key << "RootRotationMask" << YAML::Value << animationAsset->GetRootRotationMask();
                out << YAML::Key << "DiscardRootMotion" << YAML::Value << animationAsset->IsDiscardRootMotion();
            }
            out << YAML::EndMap;
        }
        out << YAML::EndMap;

        return std::string(out.c_str());
    }

    bool AnimationAssetSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<AnimationAsset>& animationAsset) const
    {
        YAML::Node data = YAML::Load(yamlString);
        return DeserializeFromYAML(data, animationAsset);
    }

    bool AnimationAssetSerializer::DeserializeFromYAML(const YAML::Node& data, Ref<AnimationAsset>& animationAsset) const
    {
        auto animationNode = data["Animation"];
        if (!animationNode)
            return false;

        AssetHandle animationSource = animationNode["AnimationSource"].as<AssetHandle>();
        AssetHandle mesh = animationNode["Mesh"].as<AssetHandle>();
        std::string animationName = animationNode["AnimationName"].as<std::string>();
        bool extractRootMotion = animationNode["ExtractRootMotion"] ? animationNode["ExtractRootMotion"].as<bool>() : false;
        u32 rootBoneIndex = animationNode["RootBoneIndex"] ? animationNode["RootBoneIndex"].as<u32>() : 0;

        glm::vec3 rootTranslationMask = glm::vec3(1.0f);
        if (animationNode["RootTranslationMask"])
        {
            auto maskNode = animationNode["RootTranslationMask"];
            if (maskNode.IsSequence() && maskNode.size() == 3)
            {
                rootTranslationMask.x = maskNode[0].as<float>();
                rootTranslationMask.y = maskNode[1].as<float>();
                rootTranslationMask.z = maskNode[2].as<float>();
            }
        }

        glm::vec3 rootRotationMask = glm::vec3(1.0f);
        if (animationNode["RootRotationMask"])
        {
            auto maskNode = animationNode["RootRotationMask"];
            if (maskNode.IsSequence() && maskNode.size() == 3)
            {
                rootRotationMask.x = maskNode[0].as<float>();
                rootRotationMask.y = maskNode[1].as<float>();
                rootRotationMask.z = maskNode[2].as<float>();
            }
        }

        bool discardRootMotion = animationNode["DiscardRootMotion"] ? animationNode["DiscardRootMotion"].as<bool>() : false;

        AnimationRootMotionSettings rootMotion;
        rootMotion.ExtractRootMotion = extractRootMotion;
        rootMotion.RootBoneIndex = rootBoneIndex;
        rootMotion.RootTranslationMask = rootTranslationMask;
        rootMotion.RootRotationMask = rootRotationMask;
        rootMotion.DiscardRootMotion = discardRootMotion;
        animationAsset = Ref<AnimationAsset>(new AnimationAsset(animationSource, mesh, animationName, rootMotion));
        return true;
    }

    void AnimationAssetSerializer::RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const
    {
        YAML::Node data = YAML::Load(yamlString);
        auto animationNode = data["Animation"];
        if (!animationNode)
            return;

        // Register dependencies on animation source and mesh
        if (animationNode["AnimationSource"])
        {
            AssetHandle animationSource = animationNode["AnimationSource"].as<AssetHandle>();
            if (animationSource != 0)
                AssetManager::RegisterDependency(animationSource, handle);
        }

        if (animationNode["Mesh"])
        {
            AssetHandle mesh = animationNode["Mesh"].as<AssetHandle>();
            if (mesh != 0)
                AssetManager::RegisterDependency(mesh, handle);
        }
    }

    void AnimationAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<AnimationAsset> animationAsset = asset.As<AnimationAsset>();
        if (!animationAsset)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::Serialize - Asset is not an AnimationAsset");
            return;
        }

        std::ofstream fout(Project::GetProjectDirectory() / metadata.FilePath);
        if (!fout.good())
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::Serialize - Failed to open file for writing: {}", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(animationAsset);
        fout << yamlString;
    }

    bool AnimationAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::ifstream fin(Project::GetProjectDirectory() / metadata.FilePath);
        if (!fin.good())
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::TryLoadData - Failed to open file: {}", metadata.FilePath.string());
            return false;
        }

        std::stringstream buffer;
        buffer << fin.rdbuf();
        std::string yamlString = buffer.str();

        Ref<AnimationAsset> animationAsset;
        if (bool result = DeserializeFromYAML(yamlString, animationAsset); !result)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::TryLoadData - Failed to deserialize animation asset");
            return false;
        }

        asset = animationAsset;
        asset->m_Handle = metadata.Handle;
        RegisterDependenciesFromYAML(yamlString, asset->m_Handle);
        return true;
    }

    void AnimationAssetSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        std::ifstream fin(Project::GetProjectDirectory() / metadata.FilePath);
        if (!fin.good())
        {
            OLO_CORE_WARN("AnimationAssetSerializer::RegisterDependencies - Failed to open file: {}", metadata.FilePath.string());
            return;
        }

        std::stringstream buffer;
        buffer << fin.rdbuf();
        std::string yamlString = buffer.str();

        RegisterDependenciesFromYAML(yamlString, metadata.Handle);
    }

    bool AnimationAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<AnimationAsset> animationAsset = AssetManager::GetAsset<AnimationAsset>(handle);
        if (!animationAsset)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::SerializeToAssetPack - Failed to get animation asset");
            return false;
        }

        std::string yamlString = SerializeToYAML(animationAsset);

        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> AnimationAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<AnimationAsset> animationAsset;
        if (bool result = DeserializeFromYAML(yamlString, animationAsset); !result)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::DeserializeFromAssetPack - Failed to deserialize animation asset");
            return nullptr;
        }

        return animationAsset;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AnimationGraphAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void AnimationGraphAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto graphAsset = asset.As<AnimationGraphAsset>();
        if (!graphAsset || !graphAsset->GetGraph())
        {
            OLO_CORE_ERROR("AnimationGraphAssetSerializer::Serialize - Invalid animation graph asset");
            return;
        }

        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;
        if (!AnimationGraphSerializer::Serialize(graphAsset->GetGraph(), filepath.string()))
        {
            OLO_CORE_ERROR("AnimationGraphAssetSerializer::Serialize - Failed to write: {}", filepath.string());
        }
    }

    bool AnimationGraphAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;
        auto graphAsset = AnimationGraphSerializer::DeserializeAsset(filepath.string());
        if (!graphAsset)
        {
            OLO_CORE_ERROR("AnimationGraphAssetSerializer::TryLoadData - Failed to load: {}", metadata.FilePath.string());
            return false;
        }
        graphAsset->m_Handle = metadata.Handle;
        asset = graphAsset;
        return true;
    }

    void AnimationGraphAssetSerializer::RegisterDependencies([[maybe_unused]] const AssetMetadata& metadata) const
    {
    }

    bool AnimationGraphAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto graphAsset = AssetManager::GetAsset<AnimationGraphAsset>(handle);
        if (!graphAsset || !graphAsset->GetGraph())
        {
            OLO_CORE_ERROR("AnimationGraphAssetSerializer::SerializeToAssetPack - Invalid animation graph asset");
            return false;
        }

        std::string yamlString = AnimationGraphSerializer::SerializeToString(graphAsset->GetGraph());
        if (yamlString.empty())
        {
            OLO_CORE_ERROR("AnimationGraphAssetSerializer::SerializeToAssetPack - Failed to serialize graph to string");
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> AnimationGraphAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto graph = AnimationGraphSerializer::DeserializeFromString(yamlString);
        if (!graph)
        {
            OLO_CORE_ERROR("AnimationGraphAssetSerializer::DeserializeFromAssetPack - Failed to deserialize graph from string");
            return nullptr;
        }

        auto graphAsset = Ref<AnimationGraphAsset>::Create();
        graphAsset->SetGraph(graph);
        graphAsset->m_Handle = assetInfo.Handle;
        return graphAsset;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // CinematicSequenceAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void CinematicSequenceAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto sequence = asset.As<CinematicSequence>();
        if (!sequence)
        {
            OLO_CORE_ERROR("CinematicSequenceAssetSerializer::Serialize - asset is not a CinematicSequence");
            return;
        }

        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;
        if (!CinematicSequenceSerializer::Serialize(sequence, filepath.string()))
        {
            OLO_CORE_ERROR("CinematicSequenceAssetSerializer::Serialize - Failed to write: {}", filepath.string());
        }
    }

    bool CinematicSequenceAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        std::filesystem::path filepath = Project::GetProjectDirectory() / metadata.FilePath;
        auto sequence = CinematicSequenceSerializer::DeserializeAsset(filepath.string());
        if (!sequence)
        {
            OLO_CORE_ERROR("CinematicSequenceAssetSerializer::TryLoadData - Failed to load: {}", metadata.FilePath.string());
            return false;
        }
        sequence->m_Handle = metadata.Handle;
        asset = sequence;
        return true;
    }

    void CinematicSequenceAssetSerializer::RegisterDependencies([[maybe_unused]] const AssetMetadata& metadata) const
    {
    }

    bool CinematicSequenceAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto sequence = AssetManager::GetAsset<CinematicSequence>(handle);
        if (!sequence)
        {
            OLO_CORE_ERROR("CinematicSequenceAssetSerializer::SerializeToAssetPack - Invalid cinematic sequence asset");
            return false;
        }

        std::string yamlString = CinematicSequenceSerializer::SerializeToString(sequence);
        if (yamlString.empty())
        {
            OLO_CORE_ERROR("CinematicSequenceAssetSerializer::SerializeToAssetPack - Failed to serialize sequence to string");
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> CinematicSequenceAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto sequence = CinematicSequenceSerializer::DeserializeFromString(yamlString);
        if (!sequence)
        {
            OLO_CORE_ERROR("CinematicSequenceAssetSerializer::DeserializeFromAssetPack - Failed to deserialize sequence from string");
            return nullptr;
        }

        sequence->m_Handle = assetInfo.Handle;
        return sequence;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // SoundGraphSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void SoundGraphSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<SoundGraphAsset> soundGraphAsset = asset.As<SoundGraphAsset>();
        if (!soundGraphAsset)
        {
            OLO_CORE_ERROR("SoundGraphSerializer::Serialize - asset is not a SoundGraphAsset");
            return;
        }

        // Resolve absolute path by anchoring to project asset directory
        std::filesystem::path absolutePath = Project::GetProjectDirectory() / metadata.FilePath;

        if (std::filesystem::path parentDir = absolutePath.parent_path(); !parentDir.empty())
        {
            std::error_code ec;
            if (!std::filesystem::create_directories(parentDir, ec) && ec)
            {
                OLO_CORE_ERROR("SoundGraphSerializer::Serialize - Failed to create parent directories for: {}, error: {}", absolutePath.string(), ec.message());
                return;
            }
        }

        if (!Audio::SoundGraph::SoundGraphSerializer::Serialize(*soundGraphAsset, absolutePath))
        {
            OLO_CORE_ERROR("SoundGraphSerializer::Serialize - Failed to serialize sound graph to file: {}", absolutePath.string());
            return;
        }
    }

    bool SoundGraphSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<SoundGraphAsset> soundGraphAsset = Ref<SoundGraphAsset>::Create();

        // Resolve absolute path by anchoring to project asset directory
        if (std::filesystem::path absolutePath = Project::GetProjectDirectory() / metadata.FilePath; !Audio::SoundGraph::SoundGraphSerializer::Deserialize(*soundGraphAsset, absolutePath))
        {
            OLO_CORE_ERROR("SoundGraphSerializer::TryLoadData - Failed to deserialize SoundGraph from '{}'", absolutePath.string());
            return false;
        }

        // Set the asset handle from metadata
        soundGraphAsset->SetHandle(metadata.Handle);

        asset = soundGraphAsset;
        return true;
    }

    bool SoundGraphSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        outInfo.Offset = stream.GetStreamPosition();

        // Get the SoundGraphAsset
        Ref<SoundGraphAsset> soundGraphAsset = AssetManager::GetAsset<SoundGraphAsset>(handle);
        if (!soundGraphAsset)
        {
            OLO_CORE_ERROR("SoundGraphSerializer::SerializeToAssetPack - Failed to get SoundGraphAsset for handle {}", handle);
            return false;
        }

        // Serialize the SoundGraphAsset to YAML string
        std::string yamlData = Audio::SoundGraph::SoundGraphSerializer::SerializeToString(*soundGraphAsset);

        // Write the YAML data as a string to the pack
        stream.WriteString(yamlData);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("SoundGraphSerializer::SerializeToAssetPack - Serialized SoundGraph '{}' to pack, Size: {} bytes",
                       soundGraphAsset->GetName(), outInfo.Size);
        return true;
    }

    Ref<Asset> SoundGraphSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        // Set stream position to the asset's offset in the pack
        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read the YAML data as a string
        std::string yamlData;
        stream.ReadString(yamlData);

        // Deserialize from YAML string
        Ref<SoundGraphAsset> soundGraphAsset = Ref<SoundGraphAsset>::Create();
        if (!Audio::SoundGraph::SoundGraphSerializer::DeserializeFromString(*soundGraphAsset, yamlData))
        {
            OLO_CORE_ERROR("SoundGraphSerializer::DeserializeFromAssetPack - Failed to deserialize SoundGraph from pack");
            return nullptr;
        }

        // Set the handle
        soundGraphAsset->SetHandle(assetInfo.Handle);

        OLO_CORE_TRACE("SoundGraphSerializer::DeserializeFromAssetPack - Deserialized SoundGraph '{}' from pack, Handle: {}",
                       soundGraphAsset->GetName(), assetInfo.Handle);
        return soundGraphAsset;
    }

    // ============================================================
    // ParticleSystemAssetSerializer
    // ============================================================

    namespace
    {
        template<typename T>
        void TrySetPS(T& target, const YAML::Node& node)
        {
            if (node)
            {
                target = node.as<T>();
            }
        }

    } // namespace

    void ParticleSystemAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();
        auto particleAsset = asset.As<ParticleSystemAsset>();
        if (!particleAsset)
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer::Serialize - Failed to cast asset to ParticleSystemAsset ({})", metadata.FilePath.string());
            return;
        }
        std::string yamlString = SerializeToYAML(particleAsset);
        auto fullPath = Project::GetProjectDirectory() / metadata.FilePath;
        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer::Serialize - Failed to create directories for ({}): {}", fullPath.string(), ec.message());
            return;
        }
        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer::Serialize - Failed to open file for writing ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool ParticleSystemAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();
        auto path = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("ParticleSystemAssetSerializer::TryLoadData - File does not exist ({})", path.string());
            return false;
        }
        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer::TryLoadData - Failed to open file ({})", path.string());
            return false;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        auto particleAsset = Ref<ParticleSystemAsset>::Create();
        if (!DeserializeFromYAML(ss.str(), particleAsset))
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer::TryLoadData - Failed to deserialize ({})", path.string());
            return false;
        }
        particleAsset->SetHandle(metadata.Handle);
        asset = particleAsset;
        return true;
    }

    bool ParticleSystemAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();
        auto particleAsset = AssetManager::GetAsset<ParticleSystemAsset>(handle);
        if (!particleAsset)
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer::SerializeToAssetPack - Failed to get asset ({})", static_cast<u64>(handle));
            return false;
        }
        std::string yamlString = SerializeToYAML(particleAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> ParticleSystemAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();
        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);
        auto particleAsset = Ref<ParticleSystemAsset>::Create();
        if (!DeserializeFromYAML(yamlString, particleAsset))
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer::DeserializeFromAssetPack - Failed to deserialize YAML (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }
        particleAsset->SetHandle(assetInfo.Handle);
        return particleAsset;
    }

    std::string ParticleSystemAssetSerializer::SerializeToYAML(const Ref<ParticleSystemAsset>& particleAsset) const
    {
        OLO_PROFILE_FUNCTION();

        const auto& sys = particleAsset->System;
        const auto& emitter = sys.Emitter;

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "ParticleSystem" << YAML::Value;
        out << YAML::BeginMap;

        out << YAML::Key << "MaxParticles" << YAML::Value << sys.GetMaxParticles();
        out << YAML::Key << "Playing" << YAML::Value << sys.Playing;
        out << YAML::Key << "Looping" << YAML::Value << sys.Looping;
        out << YAML::Key << "Duration" << YAML::Value << sys.Duration;
        out << YAML::Key << "PlaybackSpeed" << YAML::Value << sys.PlaybackSpeed;
        out << YAML::Key << "WarmUpTime" << YAML::Value << sys.WarmUpTime;
        out << YAML::Key << "SimulationSpace" << YAML::Value << static_cast<int>(std::to_underlying(sys.SimulationSpace));

        // Emitter
        out << YAML::Key << "RateOverTime" << YAML::Value << emitter.RateOverTime;
        out << YAML::Key << "InitialSpeed" << YAML::Value << emitter.InitialSpeed;
        out << YAML::Key << "SpeedVariance" << YAML::Value << emitter.SpeedVariance;
        out << YAML::Key << "LifetimeMin" << YAML::Value << emitter.LifetimeMin;
        out << YAML::Key << "LifetimeMax" << YAML::Value << emitter.LifetimeMax;
        out << YAML::Key << "InitialSize" << YAML::Value << emitter.InitialSize;
        out << YAML::Key << "SizeVariance" << YAML::Value << emitter.SizeVariance;
        out << YAML::Key << "InitialRotation" << YAML::Value << emitter.InitialRotation;
        out << YAML::Key << "RotationVariance" << YAML::Value << emitter.RotationVariance;
        out << YAML::Key << "InitialColor" << YAML::Value << emitter.InitialColor;

        // Bursts
        out << YAML::Key << "Bursts" << YAML::Value << YAML::BeginSeq;
        for (const auto& burst : emitter.Bursts)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Time" << YAML::Value << burst.Time;
            out << YAML::Key << "Count" << YAML::Value << burst.Count;
            out << YAML::Key << "Probability" << YAML::Value << burst.Probability;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "EmissionShapeType" << YAML::Value << std::to_underlying(GetEmissionShapeType(emitter.Shape));

        if (auto* sphere = std::get_if<EmitSphere>(&emitter.Shape))
        {
            out << YAML::Key << "EmissionSphereRadius" << YAML::Value << sphere->Radius;
        }
        else if (auto* box = std::get_if<EmitBox>(&emitter.Shape))
        {
            out << YAML::Key << "EmissionBoxHalfExtents" << YAML::Value << box->HalfExtents;
        }
        else if (auto* cone = std::get_if<EmitCone>(&emitter.Shape))
        {
            out << YAML::Key << "EmissionConeAngle" << YAML::Value << cone->Angle;
            out << YAML::Key << "EmissionConeRadius" << YAML::Value << cone->Radius;
        }
        else if (auto* ring = std::get_if<EmitRing>(&emitter.Shape))
        {
            out << YAML::Key << "EmissionRingInnerRadius" << YAML::Value << ring->InnerRadius;
            out << YAML::Key << "EmissionRingOuterRadius" << YAML::Value << ring->OuterRadius;
        }
        else if (auto* edge = std::get_if<EmitEdge>(&emitter.Shape))
        {
            out << YAML::Key << "EmissionEdgeLength" << YAML::Value << edge->Length;
        }
        else if (auto* mesh = std::get_if<EmitMesh>(&emitter.Shape))
        {
            out << YAML::Key << "EmissionMeshPrimitiveType" << YAML::Value << mesh->PrimitiveType;
        }
        else
        {
            // No additional handling required.
        }

        // Modules
        out << YAML::Key << "GravityEnabled" << YAML::Value << sys.GravityModule.Enabled;
        out << YAML::Key << "Gravity" << YAML::Value << sys.GravityModule.Gravity;
        out << YAML::Key << "DragEnabled" << YAML::Value << sys.DragModule.Enabled;
        out << YAML::Key << "DragCoefficient" << YAML::Value << sys.DragModule.DragCoefficient;
        out << YAML::Key << "ColorOverLifetimeEnabled" << YAML::Value << sys.ColorModule.Enabled;
        ParticleCurveSerializer::Serialize4(out, "ColorCurve", sys.ColorModule.ColorCurve);
        out << YAML::Key << "SizeOverLifetimeEnabled" << YAML::Value << sys.SizeModule.Enabled;
        ParticleCurveSerializer::Serialize(out, "SizeCurve", sys.SizeModule.SizeCurve);
        out << YAML::Key << "VelocityOverLifetimeEnabled" << YAML::Value << sys.VelocityModule.Enabled;
        out << YAML::Key << "LinearAcceleration" << YAML::Value << sys.VelocityModule.LinearAcceleration;
        out << YAML::Key << "SpeedMultiplier" << YAML::Value << sys.VelocityModule.SpeedMultiplier;
        ParticleCurveSerializer::Serialize(out, "SpeedCurve", sys.VelocityModule.SpeedCurve);
        out << YAML::Key << "RotationOverLifetimeEnabled" << YAML::Value << sys.RotationModule.Enabled;
        out << YAML::Key << "AngularVelocity" << YAML::Value << sys.RotationModule.AngularVelocity;
        out << YAML::Key << "NoiseEnabled" << YAML::Value << sys.NoiseModule.Enabled;
        out << YAML::Key << "NoiseStrength" << YAML::Value << sys.NoiseModule.Strength;
        out << YAML::Key << "NoiseFrequency" << YAML::Value << sys.NoiseModule.Frequency;

        // Phase 2 modules
        out << YAML::Key << "CollisionEnabled" << YAML::Value << sys.CollisionModule.Enabled;
        out << YAML::Key << "CollisionMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.CollisionModule.Mode));
        out << YAML::Key << "CollisionPlaneNormal" << YAML::Value << sys.CollisionModule.PlaneNormal;
        out << YAML::Key << "CollisionPlaneOffset" << YAML::Value << sys.CollisionModule.PlaneOffset;
        out << YAML::Key << "CollisionBounce" << YAML::Value << sys.CollisionModule.Bounce;
        out << YAML::Key << "CollisionLifetimeLoss" << YAML::Value << sys.CollisionModule.LifetimeLoss;
        out << YAML::Key << "CollisionKillOnCollide" << YAML::Value << sys.CollisionModule.KillOnCollide;
        out << YAML::Key << "ForceFields" << YAML::Value << YAML::BeginSeq;
        for (const auto& ff : sys.ForceFields)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Enabled" << YAML::Value << ff.Enabled;
            out << YAML::Key << "Type" << YAML::Value << static_cast<int>(std::to_underlying(ff.Type));
            out << YAML::Key << "Position" << YAML::Value << ff.Position;
            out << YAML::Key << "Strength" << YAML::Value << ff.Strength;
            out << YAML::Key << "Radius" << YAML::Value << ff.Radius;
            out << YAML::Key << "Axis" << YAML::Value << ff.Axis;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::Key << "TrailEnabled" << YAML::Value << sys.TrailModule.Enabled;
        out << YAML::Key << "TrailMaxPoints" << YAML::Value << sys.TrailModule.MaxTrailPoints;
        out << YAML::Key << "TrailLifetime" << YAML::Value << sys.TrailModule.TrailLifetime;
        out << YAML::Key << "TrailMinVertexDistance" << YAML::Value << sys.TrailModule.MinVertexDistance;
        out << YAML::Key << "TrailWidthStart" << YAML::Value << sys.TrailModule.WidthStart;
        out << YAML::Key << "TrailWidthEnd" << YAML::Value << sys.TrailModule.WidthEnd;
        out << YAML::Key << "TrailColorStart" << YAML::Value << sys.TrailModule.ColorStart;
        out << YAML::Key << "TrailColorEnd" << YAML::Value << sys.TrailModule.ColorEnd;
        out << YAML::Key << "SubEmitterEnabled" << YAML::Value << sys.SubEmitterModule.Enabled;
        out << YAML::Key << "SubEmitterEntries" << YAML::Value << YAML::BeginSeq;
        for (const auto& entry : sys.SubEmitterModule.Entries)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Trigger" << YAML::Value << static_cast<int>(std::to_underlying(entry.Trigger));
            out << YAML::Key << "EmitCount" << YAML::Value << entry.EmitCount;
            out << YAML::Key << "InheritVelocity" << YAML::Value << entry.InheritVelocity;
            out << YAML::Key << "InheritVelocityScale" << YAML::Value << entry.InheritVelocityScale;
            out << YAML::Key << "ChildSystemIndex" << YAML::Value << entry.ChildSystemIndex;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::Key << "LODDistance1" << YAML::Value << sys.LODDistance1;
        out << YAML::Key << "LODMaxDistance" << YAML::Value << sys.LODMaxDistance;

        // Rendering settings
        out << YAML::Key << "BlendMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.BlendMode));
        out << YAML::Key << "RenderMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.RenderMode));
        out << YAML::Key << "DepthSortEnabled" << YAML::Value << sys.DepthSortEnabled;
        out << YAML::Key << "UseGPU" << YAML::Value << sys.UseGPU;
        out << YAML::Key << "SoftParticlesEnabled" << YAML::Value << sys.SoftParticlesEnabled;
        out << YAML::Key << "SoftParticleDistance" << YAML::Value << sys.SoftParticleDistance;
        out << YAML::Key << "VelocityInheritance" << YAML::Value << sys.VelocityInheritance;

        // Texture sheet animation
        out << YAML::Key << "TextureSheetEnabled" << YAML::Value << sys.TextureSheetModule.Enabled;
        out << YAML::Key << "TextureSheetGridX" << YAML::Value << sys.TextureSheetModule.GridX;
        out << YAML::Key << "TextureSheetGridY" << YAML::Value << sys.TextureSheetModule.GridY;
        out << YAML::Key << "TextureSheetTotalFrames" << YAML::Value << sys.TextureSheetModule.TotalFrames;
        out << YAML::Key << "TextureSheetMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.TextureSheetModule.Mode));
        out << YAML::Key << "TextureSheetSpeedRange" << YAML::Value << sys.TextureSheetModule.SpeedRange;

        out << YAML::EndMap;
        out << YAML::EndMap;
        return std::string(out.c_str());
    }

    bool ParticleSystemAssetSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<ParticleSystemAsset>& particleAsset) const
    {
        OLO_PROFILE_FUNCTION();

        try
        {
            YAML::Node data = YAML::Load(yamlString);
            if (!data["ParticleSystem"])
                return false;

            auto ps = data["ParticleSystem"];
            auto& sys = particleAsset->System;
            auto& emitter = sys.Emitter;

            if (ps["MaxParticles"])
                sys.SetMaxParticles(ps["MaxParticles"].as<u32>());
            TrySetPS(sys.Playing, ps["Playing"]);
            TrySetPS(sys.Looping, ps["Looping"]);
            TrySetPS(sys.Duration, ps["Duration"]);
            TrySetPS(sys.PlaybackSpeed, ps["PlaybackSpeed"]);
            TrySetPS(sys.WarmUpTime, ps["WarmUpTime"]);
            if (ps["SimulationSpace"])
                sys.SimulationSpace = static_cast<ParticleSpace>(ps["SimulationSpace"].as<int>());

            TrySetPS(emitter.RateOverTime, ps["RateOverTime"]);
            TrySetPS(emitter.InitialSpeed, ps["InitialSpeed"]);
            TrySetPS(emitter.SpeedVariance, ps["SpeedVariance"]);
            TrySetPS(emitter.LifetimeMin, ps["LifetimeMin"]);
            TrySetPS(emitter.LifetimeMax, ps["LifetimeMax"]);
            TrySetPS(emitter.InitialSize, ps["InitialSize"]);
            TrySetPS(emitter.SizeVariance, ps["SizeVariance"]);
            TrySetPS(emitter.InitialRotation, ps["InitialRotation"]);
            TrySetPS(emitter.RotationVariance, ps["RotationVariance"]);
            TrySetPS(emitter.InitialColor, ps["InitialColor"]);

            // Bursts
            emitter.Bursts.clear();
            if (auto burstsNode = ps["Bursts"]; burstsNode && burstsNode.IsSequence())
            {
                for (const auto& burstNode : burstsNode)
                {
                    BurstEntry burst{};
                    TrySetPS(burst.Time, burstNode["Time"]);
                    TrySetPS(burst.Count, burstNode["Count"]);
                    TrySetPS(burst.Probability, burstNode["Probability"]);
                    emitter.Bursts.push_back(burst);
                }
            }

            if (auto shapeType = ps["EmissionShapeType"]; shapeType)
            {
                switch (static_cast<EmissionShapeType>(shapeType.as<int>()))
                {
                    case EmissionShapeType::Point:
                        emitter.Shape = EmitPoint{};
                        break;
                    case EmissionShapeType::Sphere:
                    {
                        EmitSphere s{};
                        TrySetPS(s.Radius, ps["EmissionSphereRadius"]);
                        emitter.Shape = s;
                        break;
                    }
                    case EmissionShapeType::Box:
                    {
                        EmitBox b{};
                        TrySetPS(b.HalfExtents, ps["EmissionBoxHalfExtents"]);
                        emitter.Shape = b;
                        break;
                    }
                    case EmissionShapeType::Cone:
                    {
                        EmitCone c{};
                        TrySetPS(c.Angle, ps["EmissionConeAngle"]);
                        TrySetPS(c.Radius, ps["EmissionConeRadius"]);
                        emitter.Shape = c;
                        break;
                    }
                    case EmissionShapeType::Ring:
                    {
                        EmitRing r{};
                        TrySetPS(r.InnerRadius, ps["EmissionRingInnerRadius"]);
                        TrySetPS(r.OuterRadius, ps["EmissionRingOuterRadius"]);
                        emitter.Shape = r;
                        break;
                    }
                    case EmissionShapeType::Edge:
                    {
                        EmitEdge e{};
                        TrySetPS(e.Length, ps["EmissionEdgeLength"]);
                        emitter.Shape = e;
                        break;
                    }
                    case EmissionShapeType::Mesh:
                    {
                        EmitMesh m{};
                        if (auto val = ps["EmissionMeshPrimitiveType"]; val)
                        {
                            m.PrimitiveType = val.as<i32>();
                        }
                        BuildEmitMeshFromPrimitive(m, m.PrimitiveType);
                        emitter.Shape = std::move(m);
                        break;
                    }
                    default:
                        OLO_CORE_WARN("ParticleSystemAssetSerializer: Unknown EmissionShapeType ({})", shapeType.as<int>());
                        break;
                }
            }

            TrySetPS(sys.GravityModule.Enabled, ps["GravityEnabled"]);
            TrySetPS(sys.GravityModule.Gravity, ps["Gravity"]);
            TrySetPS(sys.DragModule.Enabled, ps["DragEnabled"]);
            TrySetPS(sys.DragModule.DragCoefficient, ps["DragCoefficient"]);
            TrySetPS(sys.ColorModule.Enabled, ps["ColorOverLifetimeEnabled"]);
            ParticleCurveSerializer::Deserialize4(ps["ColorCurve"], sys.ColorModule.ColorCurve);
            TrySetPS(sys.SizeModule.Enabled, ps["SizeOverLifetimeEnabled"]);
            ParticleCurveSerializer::Deserialize(ps["SizeCurve"], sys.SizeModule.SizeCurve);
            TrySetPS(sys.VelocityModule.Enabled, ps["VelocityOverLifetimeEnabled"]);
            TrySetPS(sys.VelocityModule.LinearAcceleration, ps["LinearAcceleration"]);
            // Backward compatibility with older assets
            if (!ps["LinearAcceleration"])
                TrySetPS(sys.VelocityModule.LinearAcceleration, ps["LinearVelocity"]);
            TrySetPS(sys.VelocityModule.SpeedMultiplier, ps["SpeedMultiplier"]);
            ParticleCurveSerializer::Deserialize(ps["SpeedCurve"], sys.VelocityModule.SpeedCurve);
            TrySetPS(sys.RotationModule.Enabled, ps["RotationOverLifetimeEnabled"]);
            TrySetPS(sys.RotationModule.AngularVelocity, ps["AngularVelocity"]);
            TrySetPS(sys.NoiseModule.Enabled, ps["NoiseEnabled"]);
            TrySetPS(sys.NoiseModule.Strength, ps["NoiseStrength"]);
            TrySetPS(sys.NoiseModule.Frequency, ps["NoiseFrequency"]);

            TrySetPS(sys.CollisionModule.Enabled, ps["CollisionEnabled"]);
            if (auto val = ps["CollisionMode"]; val)
                sys.CollisionModule.Mode = static_cast<CollisionMode>(val.as<int>());
            TrySetPS(sys.CollisionModule.PlaneNormal, ps["CollisionPlaneNormal"]);
            TrySetPS(sys.CollisionModule.PlaneOffset, ps["CollisionPlaneOffset"]);
            TrySetPS(sys.CollisionModule.Bounce, ps["CollisionBounce"]);
            TrySetPS(sys.CollisionModule.LifetimeLoss, ps["CollisionLifetimeLoss"]);
            TrySetPS(sys.CollisionModule.KillOnCollide, ps["CollisionKillOnCollide"]);

            if (auto forceFieldsNode = ps["ForceFields"]; forceFieldsNode && forceFieldsNode.IsSequence())
            {
                sys.ForceFields.clear();
                for (auto ffNode : forceFieldsNode)
                {
                    ModuleForceField ff{};
                    TrySetPS(ff.Enabled, ffNode["Enabled"]);
                    if (auto val = ffNode["Type"]; val)
                        ff.Type = static_cast<ForceFieldType>(val.as<int>());
                    TrySetPS(ff.Position, ffNode["Position"]);
                    TrySetPS(ff.Strength, ffNode["Strength"]);
                    TrySetPS(ff.Radius, ffNode["Radius"]);
                    TrySetPS(ff.Axis, ffNode["Axis"]);
                    sys.ForceFields.push_back(ff);
                }
            }
            else if (auto oldEnabled = ps["ForceFieldEnabled"]; oldEnabled)
            {
                ModuleForceField ff{};
                TrySetPS(ff.Enabled, oldEnabled);
                if (auto val = ps["ForceFieldType"]; val)
                    ff.Type = static_cast<ForceFieldType>(val.as<int>());
                TrySetPS(ff.Position, ps["ForceFieldPosition"]);
                TrySetPS(ff.Strength, ps["ForceFieldStrength"]);
                TrySetPS(ff.Radius, ps["ForceFieldRadius"]);
                TrySetPS(ff.Axis, ps["ForceFieldAxis"]);
                sys.ForceFields.push_back(ff);
            }
            else
            {
                // No additional handling required.
            }

            TrySetPS(sys.TrailModule.Enabled, ps["TrailEnabled"]);
            if (auto val = ps["TrailMaxPoints"]; val)
                sys.TrailModule.MaxTrailPoints = val.as<u32>();
            TrySetPS(sys.TrailModule.TrailLifetime, ps["TrailLifetime"]);
            TrySetPS(sys.TrailModule.MinVertexDistance, ps["TrailMinVertexDistance"]);
            TrySetPS(sys.TrailModule.WidthStart, ps["TrailWidthStart"]);
            TrySetPS(sys.TrailModule.WidthEnd, ps["TrailWidthEnd"]);
            TrySetPS(sys.TrailModule.ColorStart, ps["TrailColorStart"]);
            TrySetPS(sys.TrailModule.ColorEnd, ps["TrailColorEnd"]);

            TrySetPS(sys.SubEmitterModule.Enabled, ps["SubEmitterEnabled"]);
            if (auto entriesNode = ps["SubEmitterEntries"]; entriesNode && entriesNode.IsSequence())
            {
                sys.SubEmitterModule.Entries.clear();
                for (auto entryNode : entriesNode)
                {
                    SubEmitterEntry entry{};
                    if (auto val = entryNode["Trigger"]; val)
                        entry.Trigger = static_cast<SubEmitterEvent>(val.as<int>());
                    if (auto val = entryNode["EmitCount"]; val)
                        entry.EmitCount = val.as<u32>();
                    TrySetPS(entry.InheritVelocity, entryNode["InheritVelocity"]);
                    TrySetPS(entry.InheritVelocityScale, entryNode["InheritVelocityScale"]);
                    if (auto val = entryNode["ChildSystemIndex"]; val)
                        entry.ChildSystemIndex = val.as<i32>();
                    sys.SubEmitterModule.Entries.push_back(entry);
                }
            }
            TrySetPS(sys.LODDistance1, ps["LODDistance1"]);
            TrySetPS(sys.LODMaxDistance, ps["LODMaxDistance"]);

            // Rendering settings
            if (auto val = ps["BlendMode"]; val)
                sys.BlendMode = static_cast<ParticleBlendMode>(val.as<int>());
            if (auto val = ps["RenderMode"]; val)
                sys.RenderMode = static_cast<ParticleRenderMode>(val.as<int>());
            TrySetPS(sys.DepthSortEnabled, ps["DepthSortEnabled"]);
            TrySetPS(sys.UseGPU, ps["UseGPU"]);
            TrySetPS(sys.SoftParticlesEnabled, ps["SoftParticlesEnabled"]);
            TrySetPS(sys.SoftParticleDistance, ps["SoftParticleDistance"]);
            TrySetPS(sys.VelocityInheritance, ps["VelocityInheritance"]);

            // Texture sheet animation
            TrySetPS(sys.TextureSheetModule.Enabled, ps["TextureSheetEnabled"]);
            TrySetPS(sys.TextureSheetModule.GridX, ps["TextureSheetGridX"]);
            TrySetPS(sys.TextureSheetModule.GridY, ps["TextureSheetGridY"]);
            TrySetPS(sys.TextureSheetModule.TotalFrames, ps["TextureSheetTotalFrames"]);
            if (auto val = ps["TextureSheetMode"]; val)
                sys.TextureSheetModule.Mode = static_cast<TextureSheetAnimMode>(val.as<int>());
            TrySetPS(sys.TextureSheetModule.SpeedRange, ps["TextureSheetSpeedRange"]);

            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("ParticleSystemAssetSerializer: Failed to deserialize - {}", e.what());
            return false;
        }
    }

} // namespace OloEngine
