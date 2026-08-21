#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Serialization/LightmapBinaryFormat.h"
#include "OloEngine/Serialization/ZlibSection.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine
{
    // ========================================================================
    // Internal helpers
    // ========================================================================

    namespace
    {
        // zlib deflate/inflate lives in the shared, allocation-hardened
        // Serialization/ZlibSection helper (also used by the .omesh/.oanim
        // serializers). SaveGameFile keeps its own copy by decision.

        void AppendBytes(std::vector<u8>& out, const void* data, sizet size)
        {
            const auto* bytes = static_cast<const u8*>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        // Bounds-checked sequential reader over the decompressed payload.
        struct BufferReader
        {
            const u8* Data = nullptr;
            sizet Size = 0;
            sizet Pos = 0;

            [[nodiscard]] bool Read(void* dst, sizet count)
            {
                if (count > Size - Pos)
                {
                    return false;
                }
                std::memcpy(dst, Data + Pos, count);
                Pos += count;
                return true;
            }

            [[nodiscard]] sizet Remaining() const
            {
                return Size - Pos;
            }
        };

        // Format caps the WRITER enforces on top of LightmapAsset::Validate(),
        // so it never produces a file the reader would reject.
        bool CheckFormatCaps(const LightmapAsset& lightmap, std::string_view sourceName)
        {
            if (lightmap.GetWidth() > OLmapFormat::MaxDimension || lightmap.GetHeight() > OLmapFormat::MaxDimension)
            {
                OLO_CORE_ERROR("LightmapSerializer: '{}' atlas dimensions {}x{} exceed the format cap {}",
                               sourceName, lightmap.GetWidth(), lightmap.GetHeight(), OLmapFormat::MaxDimension);
                return false;
            }
            if (lightmap.GetPageCount() > OLmapFormat::MaxPageCount)
            {
                OLO_CORE_ERROR("LightmapSerializer: '{}' page count {} exceeds the format cap {}",
                               sourceName, lightmap.GetPageCount(), OLmapFormat::MaxPageCount);
                return false;
            }
            if (lightmap.GetEntries().size() > OLmapFormat::MaxEntryCount)
            {
                OLO_CORE_ERROR("LightmapSerializer: '{}' entity entry count {} exceeds the format cap {}",
                               sourceName, lightmap.GetEntries().size(), OLmapFormat::MaxEntryCount);
                return false;
            }
            return true;
        }
    } // anonymous namespace

    // ========================================================================
    // Byte-stream encode / decode — the single source of the .olmap layout.
    // The standalone file and the asset-pack record carry identical bytes.
    // ========================================================================

    bool LightmapSerializer::EncodeToBytes(const LightmapAsset& lightmap, std::vector<u8>& outBytes, std::string_view sourceName)
    {
        // Refuse to persist an inconsistent or non-finite asset — a NaN that
        // reaches disk would poison every later load, so the write is the
        // place to stop it.
        if (!lightmap.Validate())
        {
            OLO_CORE_ERROR("LightmapSerializer: refusing to serialize '{}' — asset failed validation "
                           "(inconsistent texel buffer, non-finite texel, or bad entity entry)",
                           sourceName);
            return false;
        }
        if (!CheckFormatCaps(lightmap, sourceName))
        {
            return false;
        }

        u64 const texelBytes = lightmap.GetExpectedTexelCount() * sizeof(f32);
        u64 const entryBytes = static_cast<u64>(lightmap.GetEntries().size()) * sizeof(LightmapEntityEntry);
        u64 const payloadSize = 3u * sizeof(OLmapFormat::SectionFrame) +
                                sizeof(OLmapFormat::InfoSection) +
                                texelBytes +
                                sizeof(OLmapFormat::EntityTableHeader) + entryBytes;
        if (payloadSize > OLmapFormat::MaxUncompressedPayloadSize)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' payload size {} exceeds the format cap {}",
                           sourceName, payloadSize, OLmapFormat::MaxUncompressedPayloadSize);
            return false;
        }

        // ── Build the uncompressed payload ──
        std::vector<u8> payload;
        payload.reserve(static_cast<sizet>(payloadSize));

        // Section 0: Info
        {
            OLmapFormat::SectionFrame frame;
            frame.SectionId = std::to_underlying(OLmapFormat::SectionType::Info);
            frame.ByteCount = sizeof(OLmapFormat::InfoSection);
            AppendBytes(payload, &frame, sizeof(frame));

            OLmapFormat::InfoSection info;
            info.Width = lightmap.GetWidth();
            info.Height = lightmap.GetHeight();
            info.PageCount = lightmap.GetPageCount();
            info.BakeKey = lightmap.GetBakeKey();
            AppendBytes(payload, &info, sizeof(info));
        }

        // Section 1: Texels
        {
            OLmapFormat::SectionFrame frame;
            frame.SectionId = std::to_underlying(OLmapFormat::SectionType::Texels);
            frame.ByteCount = texelBytes;
            AppendBytes(payload, &frame, sizeof(frame));
            AppendBytes(payload, lightmap.GetTexelData().data(), static_cast<sizet>(texelBytes));
        }

        // Section 2: EntityTable
        {
            OLmapFormat::SectionFrame frame;
            frame.SectionId = std::to_underlying(OLmapFormat::SectionType::EntityTable);
            frame.ByteCount = sizeof(OLmapFormat::EntityTableHeader) + entryBytes;
            AppendBytes(payload, &frame, sizeof(frame));

            OLmapFormat::EntityTableHeader tableHeader;
            tableHeader.EntryCount = static_cast<u32>(lightmap.GetEntries().size());
            AppendBytes(payload, &tableHeader, sizeof(tableHeader));
            if (entryBytes > 0)
            {
                AppendBytes(payload, lightmap.GetEntries().data(), static_cast<sizet>(entryBytes));
            }
        }

        // ── Compress + header ──
        auto compressed = ZlibSection::Compress(payload.data(), payload.size(), "LightmapSerializer");
        if (compressed.empty())
        {
            OLO_CORE_ERROR("LightmapSerializer: failed to compress payload for '{}'", sourceName);
            return false;
        }

        OLmapFormat::FileHeader header;
        header.Flags = OLmapFormat::FlagCompressed;
        header.Checksum = Hash::CRC32(compressed.data(), compressed.size());
        header.UncompressedPayloadSize = payload.size();

        outBytes.clear();
        outBytes.reserve(sizeof(header) + compressed.size());
        AppendBytes(outBytes, &header, sizeof(header));
        AppendBytes(outBytes, compressed.data(), compressed.size());
        return true;
    }

    bool LightmapSerializer::DecodeFromBytes(const u8* data, sizet size, Ref<LightmapAsset>& outLightmap, std::string_view sourceName)
    {
        // All-or-nothing: outLightmap is assigned only after every check has
        // passed; any inconsistency rejects the whole file.

        // ── Header ──
        OLmapFormat::FileHeader header;
        if (!data || size < sizeof(header))
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' is too small to hold a .olmap header", sourceName);
            return false;
        }
        std::memcpy(&header, data, sizeof(header));

        if (header.Magic != OLmapFormat::MagicNumber)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' has invalid magic 0x{:08X}", sourceName, header.Magic);
            return false;
        }
        if (header.Version > OLmapFormat::CurrentVersion)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' is version {} but this build reads at most {} — "
                           "produced by a newer build; re-bake to load it here",
                           sourceName, header.Version, OLmapFormat::CurrentVersion);
            return false;
        }
        if (header.Version < OLmapFormat::MinSupportedVersion)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' is version {} below the minimum supported {} — "
                           "a .olmap is a derived artifact; re-bake instead of migrating",
                           sourceName, header.Version, OLmapFormat::MinSupportedVersion);
            return false;
        }

        // ── Stored payload: CRC, then optional decompress ──
        sizet const storedPayloadSize = size - sizeof(header);
        const u8* storedPayload = data + sizeof(header);
        if (storedPayloadSize == 0 || storedPayloadSize > OLmapFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' stored payload size {} is empty or exceeds the cap {}",
                           sourceName, storedPayloadSize, OLmapFormat::MaxCompressedPayloadSize);
            return false;
        }

        if (auto const computed = Hash::CRC32(storedPayload, storedPayloadSize); computed != header.Checksum)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' checksum mismatch (stored 0x{:08X}, computed 0x{:08X}) — file is corrupt",
                           sourceName, header.Checksum, computed);
            return false;
        }

        if (header.UncompressedPayloadSize == 0 || header.UncompressedPayloadSize > OLmapFormat::MaxUncompressedPayloadSize)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' claims uncompressed payload size {} (cap {})",
                           sourceName, header.UncompressedPayloadSize, OLmapFormat::MaxUncompressedPayloadSize);
            return false;
        }

        std::vector<u8> decompressed;
        BufferReader reader;
        if ((header.Flags & OLmapFormat::FlagCompressed) != 0)
        {
            // The helper re-validates the claimed size against the format cap
            // AND against the deflate ratio bound before sizing the
            // destination buffer, so a tiny hostile file claiming a huge
            // uncompressed payload allocates nothing.
            decompressed = ZlibSection::Decompress(storedPayload, storedPayloadSize,
                                                   header.UncompressedPayloadSize,
                                                   OLmapFormat::MaxUncompressedPayloadSize,
                                                   "LightmapSerializer");
            if (decompressed.empty())
            {
                OLO_CORE_ERROR("LightmapSerializer: failed to decompress payload of '{}'", sourceName);
                return false;
            }
            reader = { decompressed.data(), decompressed.size(), 0 };
        }
        else
        {
            if (storedPayloadSize != header.UncompressedPayloadSize)
            {
                OLO_CORE_ERROR("LightmapSerializer: '{}' uncompressed payload size {} does not match header claim {}",
                               sourceName, storedPayloadSize, header.UncompressedPayloadSize);
                return false;
            }
            reader = { storedPayload, storedPayloadSize, 0 };
        }

        // ── Section 0: Info ──
        OLmapFormat::SectionFrame frame;
        OLmapFormat::InfoSection info;
        if (!reader.Read(&frame, sizeof(frame)) ||
            frame.SectionId != std::to_underlying(OLmapFormat::SectionType::Info) ||
            frame.ByteCount != sizeof(info) ||
            !reader.Read(&info, sizeof(info)))
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' Info section is missing or malformed", sourceName);
            return false;
        }
        if (info.Width == 0 || info.Width > OLmapFormat::MaxDimension ||
            info.Height == 0 || info.Height > OLmapFormat::MaxDimension ||
            info.PageCount == 0 || info.PageCount > OLmapFormat::MaxPageCount)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' has out-of-range atlas parameters ({}x{}, {} pages; caps {} / {})",
                           sourceName, info.Width, info.Height, info.PageCount,
                           OLmapFormat::MaxDimension, OLmapFormat::MaxPageCount);
            return false;
        }

        // ── Section 1: Texels ──
        u64 const expectedTexelBytes = static_cast<u64>(info.PageCount) * info.Width * info.Height * 4u * sizeof(f32);
        if (!reader.Read(&frame, sizeof(frame)) ||
            frame.SectionId != std::to_underlying(OLmapFormat::SectionType::Texels) ||
            frame.ByteCount != expectedTexelBytes)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' Texels section is missing or its size does not match "
                           "PageCount*Width*Height*16",
                           sourceName);
            return false;
        }
        // frame.ByteCount == expectedTexelBytes is necessary but NOT
        // sufficient to size an allocation: both values derive from the
        // untrusted payload. Atlas parameters at the caps imply a 32 GiB
        // texel buffer (MaxDimension^2 * MaxPageCount * 16), while the
        // payload itself is capped at MaxUncompressedPayloadSize — so bound
        // the allocation by the bytes actually present BEFORE any vector is
        // sized from header-derived values.
        if (expectedTexelBytes > reader.Remaining())
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' Texels section claims {} bytes but only {} payload "
                           "bytes remain — corrupt or hostile header, refusing to allocate",
                           sourceName, expectedTexelBytes, reader.Remaining());
            return false;
        }
        std::vector<f32> texels(static_cast<sizet>(expectedTexelBytes / sizeof(f32)));
        if (!reader.Read(texels.data(), static_cast<sizet>(expectedTexelBytes)))
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' is truncated inside the Texels section", sourceName);
            return false;
        }
        for (f32 const texel : texels)
        {
            if (!std::isfinite(texel))
            {
                // Reject the file, not the texel: a partially-sanitised atlas
                // would shade convincingly and wrong.
                OLO_CORE_ERROR("LightmapSerializer: '{}' contains a non-finite texel — rejecting the file", sourceName);
                return false;
            }
        }

        // ── Section 2: EntityTable ──
        OLmapFormat::EntityTableHeader tableHeader;
        if (!reader.Read(&frame, sizeof(frame)) ||
            frame.SectionId != std::to_underlying(OLmapFormat::SectionType::EntityTable) ||
            frame.ByteCount < sizeof(tableHeader) ||
            !reader.Read(&tableHeader, sizeof(tableHeader)))
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' EntityTable section is missing or malformed", sourceName);
            return false;
        }
        if (tableHeader.EntryCount > OLmapFormat::MaxEntryCount)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' entity entry count {} exceeds the cap {}",
                           sourceName, tableHeader.EntryCount, OLmapFormat::MaxEntryCount);
            return false;
        }
        u64 const entryBytes = static_cast<u64>(tableHeader.EntryCount) * sizeof(LightmapEntityEntry);
        if (frame.ByteCount != sizeof(tableHeader) + entryBytes)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' EntityTable size does not match its entry count", sourceName);
            return false;
        }
        // Same allocation-bound rule as the Texels section: the declared
        // entry count must fit in the bytes actually present before the
        // entries vector is sized from it.
        if (entryBytes > reader.Remaining())
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' EntityTable claims {} entry bytes but only {} payload "
                           "bytes remain — corrupt or hostile header, refusing to allocate",
                           sourceName, entryBytes, reader.Remaining());
            return false;
        }
        std::vector<LightmapEntityEntry> entries(tableHeader.EntryCount);
        if (entryBytes > 0 && !reader.Read(entries.data(), static_cast<sizet>(entryBytes)))
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' is truncated inside the EntityTable section", sourceName);
            return false;
        }
        for (const auto& entry : entries)
        {
            bool finite = true;
            for (glm::length_t c = 0; c < 4; ++c)
            {
                finite = finite && std::isfinite(entry.ScaleOffset[c]);
            }
            if (!finite || entry.Page >= info.PageCount)
            {
                OLO_CORE_ERROR("LightmapSerializer: '{}' has an entity entry with a non-finite scale/offset "
                               "or an out-of-range page — rejecting the file",
                               sourceName);
                return false;
            }
        }

        // ── Trailing bytes are corruption under strict v1 framing ──
        if (reader.Remaining() != 0)
        {
            OLO_CORE_ERROR("LightmapSerializer: '{}' has {} unexpected trailing payload bytes", sourceName, reader.Remaining());
            return false;
        }

        // Every check passed — populate the asset in one shot.
        auto lightmap = Ref<LightmapAsset>::Create();
        lightmap->SetDimensions(info.Width, info.Height, info.PageCount);
        lightmap->SetBakeKey(info.BakeKey);
        lightmap->SetTexelData(std::move(texels));
        lightmap->SetEntries(std::move(entries));
        outLightmap = lightmap;
        return true;
    }

    // ========================================================================
    // File-level helpers
    // ========================================================================

    bool LightmapSerializer::SerializeToFile(const std::filesystem::path& path, const Ref<LightmapAsset>& lightmap)
    {
        if (!lightmap)
        {
            OLO_CORE_ERROR("LightmapSerializer::SerializeToFile - null asset for '{}'", path.string());
            return false;
        }

        std::vector<u8> bytes;
        if (!EncodeToBytes(*lightmap, bytes, path.string()))
        {
            return false;
        }

        if (auto const parentDir = path.parent_path(); !parentDir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parentDir, ec);
            if (ec)
            {
                OLO_CORE_ERROR("LightmapSerializer::SerializeToFile - failed to create directory '{}': {}",
                               parentDir.string(), ec.message());
                return false;
            }
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("LightmapSerializer::SerializeToFile - failed to open '{}' for writing", path.string());
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (out.fail())
        {
            OLO_CORE_ERROR("LightmapSerializer::SerializeToFile - failed while writing '{}'", path.string());
            return false;
        }

        OLO_CORE_TRACE("LightmapSerializer: wrote '{}' ({} bytes, {}x{} x{} pages, {} entries)",
                       path.filename().string(), bytes.size(), lightmap->GetWidth(), lightmap->GetHeight(),
                       lightmap->GetPageCount(), lightmap->GetEntries().size());
        return true;
    }

    bool LightmapSerializer::DeserializeFromFile(const std::filesystem::path& path, Ref<LightmapAsset>& outLightmap)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open())
        {
            OLO_CORE_ERROR("LightmapSerializer::DeserializeFromFile - failed to open '{}'", path.string());
            return false;
        }

        auto const fileSize = static_cast<u64>(in.tellg());
        if (fileSize < sizeof(OLmapFormat::FileHeader) ||
            fileSize > sizeof(OLmapFormat::FileHeader) + OLmapFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("LightmapSerializer::DeserializeFromFile - '{}' has implausible size {}", path.string(), fileSize);
            return false;
        }
        in.seekg(0, std::ios::beg);

        std::vector<u8> bytes(static_cast<sizet>(fileSize));
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (in.gcount() != static_cast<std::streamsize>(bytes.size()))
        {
            OLO_CORE_ERROR("LightmapSerializer::DeserializeFromFile - short read from '{}'", path.string());
            return false;
        }

        return DecodeFromBytes(bytes.data(), bytes.size(), outLightmap, path.string());
    }

    // ========================================================================
    // AssetSerializer interface
    // ========================================================================

    void LightmapSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        auto lightmap = asset.As<LightmapAsset>();
        if (!lightmap)
        {
            OLO_CORE_ERROR("LightmapSerializer::Serialize - Invalid asset");
            return;
        }

        // metadata.FilePath is project-root-relative (see
        // EditorAssetManager::GetRelativePath) — it already starts with "Assets/", so we
        // resolve against GetProjectDirectory(). Joining onto GetAssetDirectory() would
        // double the Assets segment and the file open would fail.
        auto filepath = Project::GetProjectDirectory() / metadata.FilePath;
        (void)SerializeToFile(filepath, lightmap);
    }

    bool LightmapSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // See Serialize() above for why this resolves against GetProjectDirectory() —
        // metadata.FilePath is project-root-relative.
        auto filepath = Project::GetProjectDirectory() / metadata.FilePath;

        Ref<LightmapAsset> lightmap;
        if (!DeserializeFromFile(filepath, lightmap))
        {
            return false;
        }

        lightmap->SetHandle(metadata.Handle);
        asset = lightmap;
        return true;
    }

    bool LightmapSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto lightmap = AssetManager::GetAsset<LightmapAsset>(handle);
        if (!lightmap)
        {
            return false;
        }

        // The pack record is the exact .olmap byte stream a standalone file
        // would hold, so pack and loose-file loads share one decode path.
        std::vector<u8> bytes;
        if (!EncodeToBytes(*lightmap, bytes, std::to_string(static_cast<u64>(handle))))
        {
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();
        if (!stream.WriteData(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        {
            return false;
        }
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> LightmapSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        if (assetInfo.PackedSize < sizeof(OLmapFormat::FileHeader) ||
            assetInfo.PackedSize > sizeof(OLmapFormat::FileHeader) + OLmapFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("LightmapSerializer::DeserializeFromAssetPack - implausible packed size {} for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        std::vector<u8> bytes(static_cast<sizet>(assetInfo.PackedSize));
        if (!stream.ReadData(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        {
            OLO_CORE_ERROR("LightmapSerializer::DeserializeFromAssetPack - failed to read {} bytes for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        Ref<LightmapAsset> lightmap;
        if (!DecodeFromBytes(bytes.data(), bytes.size(), lightmap, std::to_string(static_cast<u64>(assetInfo.Handle))))
        {
            return nullptr;
        }

        lightmap->SetHandle(assetInfo.Handle);
        return lightmap;
    }

} // namespace OloEngine
