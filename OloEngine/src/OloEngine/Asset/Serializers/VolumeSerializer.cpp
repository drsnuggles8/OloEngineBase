#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/VolumeAsset.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "OloEngine/Serialization/VolumeBinaryFormat.h"
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
        void AppendBytes(std::vector<u8>& out, const void* data, sizet size)
        {
            const auto* bytes = static_cast<const u8*>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        // Bounds-checked sequential reader over the decompressed payload —
        // identical shape to LightmapSerializer's BufferReader.
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
    } // anonymous namespace

    // ========================================================================
    // Byte-stream encode / decode — the single source of the .olovol layout.
    // The standalone file and the asset-pack record carry identical bytes.
    // ========================================================================

    bool VolumeSerializer::EncodeToBytes(const glm::uvec3& dimensions, const glm::vec3& voxelSize,
                                         const glm::mat4& gridTransform, f32 backgroundValue,
                                         const std::vector<f32>& density, std::vector<u8>& outBytes,
                                         std::string_view sourceName)
    {
        if (dimensions.x == 0 || dimensions.y == 0 || dimensions.z == 0 ||
            dimensions.x > OLoVolFormat::MaxAxisDimension || dimensions.y > OLoVolFormat::MaxAxisDimension ||
            dimensions.z > OLoVolFormat::MaxAxisDimension)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' dimensions {}x{}x{} are zero or exceed the format cap {}",
                           sourceName, dimensions.x, dimensions.y, dimensions.z, OLoVolFormat::MaxAxisDimension);
            return false;
        }
        const u64 expectedTexelCount = static_cast<u64>(dimensions.x) * dimensions.y * dimensions.z;
        if (density.size() != expectedTexelCount)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' density buffer has {} texels, expected {} ({}x{}x{})",
                           sourceName, density.size(), expectedTexelCount, dimensions.x, dimensions.y, dimensions.z);
            return false;
        }
        for (f32 const texel : density)
        {
            if (!std::isfinite(texel))
            {
                OLO_CORE_ERROR("VolumeSerializer: refusing to serialize '{}' — contains a non-finite density texel", sourceName);
                return false;
            }
        }
        if (!std::isfinite(backgroundValue))
        {
            OLO_CORE_ERROR("VolumeSerializer: refusing to serialize '{}' — non-finite background value", sourceName);
            return false;
        }

        u64 const densityBytes = expectedTexelCount * sizeof(f32);
        u64 const payloadSize = 2u * sizeof(OLoVolFormat::SectionFrame) +
                                sizeof(OLoVolFormat::InfoSection) +
                                densityBytes;
        if (payloadSize > OLoVolFormat::MaxUncompressedPayloadSize)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' payload size {} exceeds the format cap {}",
                           sourceName, payloadSize, OLoVolFormat::MaxUncompressedPayloadSize);
            return false;
        }

        // ── Build the uncompressed payload ──
        std::vector<u8> payload;
        payload.reserve(static_cast<sizet>(payloadSize));

        // Section 0: Info
        {
            OLoVolFormat::SectionFrame frame;
            frame.SectionId = std::to_underlying(OLoVolFormat::SectionType::Info);
            frame.ByteCount = sizeof(OLoVolFormat::InfoSection);
            AppendBytes(payload, &frame, sizeof(frame));

            OLoVolFormat::InfoSection info;
            info.Width = dimensions.x;
            info.Height = dimensions.y;
            info.Depth = dimensions.z;
            info.VoxelSize[0] = voxelSize.x;
            info.VoxelSize[1] = voxelSize.y;
            info.VoxelSize[2] = voxelSize.z;
            info.BackgroundValue = backgroundValue;
            // glm::mat4 is column-major in memory; store row-major explicitly
            // so the on-disk layout doesn't silently depend on glm's storage
            // convention (documented in VolumeBinaryFormat.h as row-major).
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    info.GridTransform[(row * 4) + col] = gridTransform[col][row];
                }
            }
            AppendBytes(payload, &info, sizeof(info));
        }

        // Section 1: Density
        {
            OLoVolFormat::SectionFrame frame;
            frame.SectionId = std::to_underlying(OLoVolFormat::SectionType::Density);
            frame.ByteCount = densityBytes;
            AppendBytes(payload, &frame, sizeof(frame));
            AppendBytes(payload, density.data(), static_cast<sizet>(densityBytes));
        }

        // ── Compress + header ──
        auto compressed = ZlibSection::Compress(payload.data(), payload.size(), "VolumeSerializer");
        if (compressed.empty())
        {
            OLO_CORE_ERROR("VolumeSerializer: failed to compress payload for '{}'", sourceName);
            return false;
        }

        OLoVolFormat::FileHeader header;
        header.Flags = OLoVolFormat::FlagCompressed;
        header.Checksum = Hash::CRC32(compressed.data(), compressed.size());
        header.UncompressedPayloadSize = payload.size();

        outBytes.clear();
        outBytes.reserve(sizeof(header) + compressed.size());
        AppendBytes(outBytes, &header, sizeof(header));
        AppendBytes(outBytes, compressed.data(), compressed.size());
        return true;
    }

    bool VolumeSerializer::DecodeFromBytes(const u8* data, sizet size, RawVolumeData& outVolume, std::string_view sourceName)
    {
        // All-or-nothing: outVolume is assigned only after every check has
        // passed; any inconsistency rejects the whole file.

        OLoVolFormat::FileHeader header;
        if (!data || size < sizeof(header))
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' is too small to hold a .olovol header", sourceName);
            return false;
        }
        std::memcpy(&header, data, sizeof(header));

        if (header.Magic != OLoVolFormat::MagicNumber)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' has invalid magic 0x{:08X}", sourceName, header.Magic);
            return false;
        }
        if (header.Version > OLoVolFormat::CurrentVersion)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' is version {} but this build reads at most {} — "
                           "produced by a newer build; re-import it here",
                           sourceName, header.Version, OLoVolFormat::CurrentVersion);
            return false;
        }
        if (header.Version < OLoVolFormat::MinSupportedVersion)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' is version {} below the minimum supported {} — "
                           "a .olovol is a derived artifact; re-import instead of migrating",
                           sourceName, header.Version, OLoVolFormat::MinSupportedVersion);
            return false;
        }

        sizet const storedPayloadSize = size - sizeof(header);
        const u8* storedPayload = data + sizeof(header);
        if (storedPayloadSize == 0 || storedPayloadSize > OLoVolFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' stored payload size {} is empty or exceeds the cap {}",
                           sourceName, storedPayloadSize, OLoVolFormat::MaxCompressedPayloadSize);
            return false;
        }

        if (auto const computed = Hash::CRC32(storedPayload, storedPayloadSize); computed != header.Checksum)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' checksum mismatch (stored 0x{:08X}, computed 0x{:08X}) — file is corrupt",
                           sourceName, header.Checksum, computed);
            return false;
        }

        if (header.UncompressedPayloadSize == 0 || header.UncompressedPayloadSize > OLoVolFormat::MaxUncompressedPayloadSize)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' claims uncompressed payload size {} (cap {})",
                           sourceName, header.UncompressedPayloadSize, OLoVolFormat::MaxUncompressedPayloadSize);
            return false;
        }

        std::vector<u8> decompressed;
        BufferReader reader;
        if ((header.Flags & OLoVolFormat::FlagCompressed) != 0)
        {
            decompressed = ZlibSection::Decompress(storedPayload, storedPayloadSize,
                                                   header.UncompressedPayloadSize,
                                                   OLoVolFormat::MaxUncompressedPayloadSize,
                                                   "VolumeSerializer");
            if (decompressed.empty())
            {
                OLO_CORE_ERROR("VolumeSerializer: failed to decompress payload of '{}'", sourceName);
                return false;
            }
            reader = { decompressed.data(), decompressed.size(), 0 };
        }
        else
        {
            if (storedPayloadSize != header.UncompressedPayloadSize)
            {
                OLO_CORE_ERROR("VolumeSerializer: '{}' uncompressed payload size {} does not match header claim {}",
                               sourceName, storedPayloadSize, header.UncompressedPayloadSize);
                return false;
            }
            reader = { storedPayload, storedPayloadSize, 0 };
        }

        // ── Section 0: Info ──
        OLoVolFormat::SectionFrame frame;
        OLoVolFormat::InfoSection info;
        if (!reader.Read(&frame, sizeof(frame)) ||
            frame.SectionId != std::to_underlying(OLoVolFormat::SectionType::Info) ||
            frame.ByteCount != sizeof(info) ||
            !reader.Read(&info, sizeof(info)))
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' Info section is missing or malformed", sourceName);
            return false;
        }
        if (info.Width == 0 || info.Width > OLoVolFormat::MaxAxisDimension ||
            info.Height == 0 || info.Height > OLoVolFormat::MaxAxisDimension ||
            info.Depth == 0 || info.Depth > OLoVolFormat::MaxAxisDimension)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' has out-of-range dimensions ({}x{}x{}; cap {})",
                           sourceName, info.Width, info.Height, info.Depth, OLoVolFormat::MaxAxisDimension);
            return false;
        }
        if (!std::isfinite(info.VoxelSize[0]) || !std::isfinite(info.VoxelSize[1]) || !std::isfinite(info.VoxelSize[2]) ||
            !std::isfinite(info.BackgroundValue))
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' has a non-finite voxel size or background value — rejecting the file", sourceName);
            return false;
        }
        for (f32 const cell : info.GridTransform)
        {
            if (!std::isfinite(cell))
            {
                OLO_CORE_ERROR("VolumeSerializer: '{}' grid transform contains a non-finite element — rejecting the file", sourceName);
                return false;
            }
        }

        // ── Section 1: Density ──
        u64 const expectedDensityBytes = static_cast<u64>(info.Width) * info.Height * info.Depth * sizeof(f32);
        if (!reader.Read(&frame, sizeof(frame)) ||
            frame.SectionId != std::to_underlying(OLoVolFormat::SectionType::Density) ||
            frame.ByteCount != expectedDensityBytes)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' Density section is missing or its size does not match Width*Height*Depth*4",
                           sourceName);
            return false;
        }
        // Necessary but not sufficient: bound the allocation by the bytes
        // actually present before sizing a vector from a header-derived
        // value (same rule as LightmapSerializer's Texels section).
        if (expectedDensityBytes > reader.Remaining())
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' Density section claims {} bytes but only {} payload "
                           "bytes remain — corrupt or hostile header, refusing to allocate",
                           sourceName, expectedDensityBytes, reader.Remaining());
            return false;
        }
        std::vector<f32> density(static_cast<sizet>(expectedDensityBytes / sizeof(f32)));
        if (!reader.Read(density.data(), static_cast<sizet>(expectedDensityBytes)))
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' is truncated inside the Density section", sourceName);
            return false;
        }
        for (f32 const texel : density)
        {
            if (!std::isfinite(texel))
            {
                OLO_CORE_ERROR("VolumeSerializer: '{}' contains a non-finite density texel — rejecting the file", sourceName);
                return false;
            }
        }

        if (reader.Remaining() != 0)
        {
            OLO_CORE_ERROR("VolumeSerializer: '{}' has {} unexpected trailing payload bytes", sourceName, reader.Remaining());
            return false;
        }

        // Every check passed — populate outVolume in one shot.
        outVolume.Density = std::move(density);
        outVolume.Dimensions = { info.Width, info.Height, info.Depth };
        outVolume.VoxelSize = { info.VoxelSize[0], info.VoxelSize[1], info.VoxelSize[2] };
        outVolume.BackgroundValue = info.BackgroundValue;
        glm::mat4 transform(1.0f);
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                transform[col][row] = info.GridTransform[(row * 4) + col];
            }
        }
        outVolume.GridTransform = transform;
        return true;
    }

    // ========================================================================
    // File-level helpers
    // ========================================================================

    bool VolumeSerializer::SerializeToFile(const std::filesystem::path& path, const glm::uvec3& dimensions,
                                           const glm::vec3& voxelSize, const glm::mat4& gridTransform,
                                           f32 backgroundValue, const std::vector<f32>& density)
    {
        std::vector<u8> bytes;
        if (!EncodeToBytes(dimensions, voxelSize, gridTransform, backgroundValue, density, bytes, path.string()))
        {
            return false;
        }

        if (auto const parentDir = path.parent_path(); !parentDir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parentDir, ec);
            if (ec)
            {
                OLO_CORE_ERROR("VolumeSerializer::SerializeToFile - failed to create directory '{}': {}",
                               parentDir.string(), ec.message());
                return false;
            }
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("VolumeSerializer::SerializeToFile - failed to open '{}' for writing", path.string());
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (out.fail())
        {
            OLO_CORE_ERROR("VolumeSerializer::SerializeToFile - failed while writing '{}'", path.string());
            return false;
        }

        OLO_CORE_TRACE("VolumeSerializer: wrote '{}' ({} bytes, {}x{}x{})",
                       path.filename().string(), bytes.size(), dimensions.x, dimensions.y, dimensions.z);
        return true;
    }

    bool VolumeSerializer::DeserializeFromFile(const std::filesystem::path& path, RawVolumeData& outVolume)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open())
        {
            OLO_CORE_ERROR("VolumeSerializer::DeserializeFromFile - failed to open '{}'", path.string());
            return false;
        }

        auto const fileSize = static_cast<u64>(in.tellg());
        if (fileSize < sizeof(OLoVolFormat::FileHeader) ||
            fileSize > sizeof(OLoVolFormat::FileHeader) + OLoVolFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("VolumeSerializer::DeserializeFromFile - '{}' has implausible size {}", path.string(), fileSize);
            return false;
        }
        in.seekg(0, std::ios::beg);

        std::vector<u8> bytes(static_cast<sizet>(fileSize));
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (in.gcount() != static_cast<std::streamsize>(bytes.size()))
        {
            OLO_CORE_ERROR("VolumeSerializer::DeserializeFromFile - short read from '{}'", path.string());
            return false;
        }

        return DecodeFromBytes(bytes.data(), bytes.size(), outVolume, path.string());
    }

    // ========================================================================
    // GPU upload — the only part of this file that touches the RHI.
    // ========================================================================

    bool VolumeSerializer::BuildVolumeAsset(const RawVolumeData& rawVolume, Ref<Asset>& outAsset)
    {
        if (!rawVolume.IsValid())
        {
            OLO_CORE_ERROR("VolumeSerializer::BuildVolumeAsset - invalid raw volume data");
            return false;
        }

        Texture3DSpecification spec;
        spec.Width = rawVolume.Dimensions.x;
        spec.Height = rawVolume.Dimensions.y;
        spec.Depth = rawVolume.Dimensions.z;
        spec.Format = Texture3DFormat::R32F; // single-channel scalar density — matches the volumetric shadow map's precedent (#723)
        spec.Repeat = false;                 // samples outside the grid should read as empty, not tile

        Ref<Texture3D> texture = Texture3D::Create(spec);
        if (!texture)
        {
            OLO_CORE_ERROR("VolumeSerializer::BuildVolumeAsset - failed to create Texture3D ({}x{}x{})",
                           spec.Width, spec.Height, spec.Depth);
            return false;
        }
        texture->SetData(rawVolume.Density.data(), static_cast<u32>(rawVolume.Density.size() * sizeof(f32)));

        auto volume = Ref<VolumeAsset>::Create();
        volume->m_Texture = texture;
        volume->m_Dimensions = rawVolume.Dimensions;
        volume->m_VoxelSize = rawVolume.VoxelSize;
        volume->m_GridTransform = rawVolume.GridTransform;
        volume->m_BackgroundValue = rawVolume.BackgroundValue;
        volume->SetHandle(rawVolume.Handle);

        outAsset = volume;
        return true;
    }

    // ========================================================================
    // AssetSerializer interface
    // ========================================================================

    bool VolumeSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // metadata.FilePath is project-root-relative — see the matching
        // comment in TextureSerializer::TryLoadData.
        auto filepath = Project::GetProjectDirectory() / metadata.FilePath;

        RawVolumeData rawVolume;
        if (!DeserializeFromFile(filepath, rawVolume))
        {
            return false;
        }
        rawVolume.Handle = metadata.Handle;

        return BuildVolumeAsset(rawVolume, asset);
    }

    bool VolumeSerializer::TryLoadRawData(const AssetMetadata& metadata, RawAssetData& outRawData) const
    {
        // Safe to call from any thread — pure CPU decode, no GL/RHI calls.
        auto filepath = Project::GetProjectDirectory() / metadata.FilePath;

        RawVolumeData rawVolume;
        if (!DeserializeFromFile(filepath, rawVolume))
        {
            return false;
        }
        rawVolume.Handle = metadata.Handle;

        outRawData = std::move(rawVolume);
        return true;
    }

    bool VolumeSerializer::FinalizeFromRawData(const RawAssetData& rawData, Ref<Asset>& asset) const
    {
        // MUST be called from the main thread — creates the GPU Texture3D.
        if (!std::holds_alternative<RawVolumeData>(rawData))
        {
            OLO_CORE_ERROR("VolumeSerializer::FinalizeFromRawData - invalid raw data type");
            return false;
        }
        return BuildVolumeAsset(std::get<RawVolumeData>(rawData), asset);
    }

    bool VolumeSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto volume = AssetManager::GetAsset<VolumeAsset>(handle);
        if (!volume)
        {
            return false;
        }

        // Re-encode from the metadata carried on the live asset — the pack
        // record is the exact .olovol byte stream a standalone file would
        // hold, matching LightmapSerializer's approach. This needs the
        // density buffer back from GPU, which VolumeAsset does not keep
        // resident (see class comment) — so pack builds re-read the source
        // .olovol file rather than reading back the GPU texture.
        auto filepath = Project::GetProjectDirectory() / AssetManager::GetAssetMetadata(handle).FilePath;
        RawVolumeData rawVolume;
        if (!DeserializeFromFile(filepath, rawVolume))
        {
            OLO_CORE_ERROR("VolumeSerializer::SerializeToAssetPack - failed to re-read source '{}' for asset {}",
                           filepath.string(), static_cast<u64>(handle));
            return false;
        }

        std::vector<u8> bytes;
        if (!EncodeToBytes(rawVolume.Dimensions, rawVolume.VoxelSize, rawVolume.GridTransform, rawVolume.BackgroundValue,
                           rawVolume.Density, bytes, std::to_string(static_cast<u64>(handle))))
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

    Ref<Asset> VolumeSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        if (assetInfo.PackedSize < sizeof(OLoVolFormat::FileHeader) ||
            assetInfo.PackedSize > sizeof(OLoVolFormat::FileHeader) + OLoVolFormat::MaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("VolumeSerializer::DeserializeFromAssetPack - implausible packed size {} for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        std::vector<u8> bytes(static_cast<sizet>(assetInfo.PackedSize));
        if (!stream.ReadData(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        {
            OLO_CORE_ERROR("VolumeSerializer::DeserializeFromAssetPack - failed to read {} bytes for asset {}",
                           assetInfo.PackedSize, static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }

        RawVolumeData rawVolume;
        if (!DecodeFromBytes(bytes.data(), bytes.size(), rawVolume, std::to_string(static_cast<u64>(assetInfo.Handle))))
        {
            return nullptr;
        }
        rawVolume.Handle = assetInfo.Handle;

        Ref<Asset> asset;
        if (!BuildVolumeAsset(rawVolume, asset))
        {
            return nullptr;
        }
        return asset;
    }

} // namespace OloEngine
