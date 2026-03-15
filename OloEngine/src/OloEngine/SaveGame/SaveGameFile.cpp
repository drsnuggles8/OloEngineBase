#include "OloEnginePCH.h"
#include "SaveGameFile.h"

#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <zlib.h>

#include <cstring>
#include <fstream>

namespace OloEngine
{
    // ========================================================================
    // Write
    // ========================================================================

    bool SaveGameFile::Write(const std::filesystem::path& path,
                             const SaveGameHeader& headerIn,
                             const SaveGameMetadata& metadata,
                             const std::vector<u8>& thumbnailPNG,
                             const std::vector<u8>& payload)
    {
        OLO_PROFILE_FUNCTION();

        // Serialize metadata to binary
        std::vector<u8> metaBlob;
        {
            FMemoryWriter writer(metaBlob);
            writer.ArIsSaveGame = true;
            auto metaCopy = const_cast<SaveGameMetadata&>(metadata);
            writer << metaCopy;
        }

        // Build header with section offsets
        SaveGameHeader header = headerIn;
        header.MetadataOffset = kSaveGameHeaderSize;
        header.MetadataSize = metaBlob.size();
        header.ThumbnailOffset = header.MetadataOffset + header.MetadataSize;
        header.ThumbnailSize = thumbnailPNG.size();
        header.PayloadOffset = header.ThumbnailOffset + header.ThumbnailSize;
        header.PayloadSize = payload.size();

        // Compute CRC32 over metadata + thumbnail + payload (using zlib crc32 for binary safety)
        {
            uLong crc = ::crc32(0L, Z_NULL, 0);
            if (!metaBlob.empty())
            {
                crc = ::crc32(crc, metaBlob.data(), static_cast<uInt>(metaBlob.size()));
            }
            if (!thumbnailPNG.empty())
            {
                crc = ::crc32(crc, thumbnailPNG.data(), static_cast<uInt>(thumbnailPNG.size()));
            }
            if (!payload.empty())
            {
                crc = ::crc32(crc, payload.data(), static_cast<uInt>(payload.size()));
            }
            header.ChecksumCRC32 = static_cast<u32>(crc);
        }

        // Write to disk
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("[SaveGameFile] Failed to open '{}' for writing", path.string());
            return false;
        }

        file.write(reinterpret_cast<const char*>(&header), sizeof(SaveGameHeader));
        if (!metaBlob.empty())
        {
            file.write(reinterpret_cast<const char*>(metaBlob.data()),
                       static_cast<std::streamsize>(metaBlob.size()));
        }
        if (!thumbnailPNG.empty())
        {
            file.write(reinterpret_cast<const char*>(thumbnailPNG.data()),
                       static_cast<std::streamsize>(thumbnailPNG.size()));
        }
        if (!payload.empty())
        {
            file.write(reinterpret_cast<const char*>(payload.data()),
                       static_cast<std::streamsize>(payload.size()));
        }

        file.flush();
        return file.good();
    }

    // ========================================================================
    // ReadHeader
    // ========================================================================

    bool SaveGameFile::ReadHeader(const std::filesystem::path& path, SaveGameHeader& outHeader)
    {
        OLO_PROFILE_FUNCTION();

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file.read(reinterpret_cast<char*>(&outHeader), sizeof(SaveGameHeader));
        if (!file.good() || !outHeader.IsValid())
        {
            return false;
        }

        return true;
    }

    // ========================================================================
    // ReadMetadata
    // ========================================================================

    bool SaveGameFile::ReadMetadata(const std::filesystem::path& path,
                                    SaveGameHeader& outHeader,
                                    SaveGameMetadata& outMetadata)
    {
        OLO_PROFILE_FUNCTION();

        if (!ReadHeader(path, outHeader))
        {
            return false;
        }

        if (outHeader.MetadataSize == 0)
        {
            return true;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file.seekg(static_cast<std::streamoff>(outHeader.MetadataOffset));
        std::vector<u8> metaBlob(outHeader.MetadataSize);
        file.read(reinterpret_cast<char*>(metaBlob.data()),
                  static_cast<std::streamsize>(outHeader.MetadataSize));
        if (!file.good())
        {
            return false;
        }

        FMemoryReader reader(metaBlob);
        reader.ArIsSaveGame = true;
        reader << outMetadata;
        return !reader.IsError();
    }

    // ========================================================================
    // ReadThumbnail
    // ========================================================================

    bool SaveGameFile::ReadThumbnail(const std::filesystem::path& path, std::vector<u8>& outPNG)
    {
        OLO_PROFILE_FUNCTION();

        SaveGameHeader header;
        if (!ReadHeader(path, header))
        {
            return false;
        }

        if (header.ThumbnailSize == 0)
        {
            outPNG.clear();
            return true;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file.seekg(static_cast<std::streamoff>(header.ThumbnailOffset));
        outPNG.resize(header.ThumbnailSize);
        file.read(reinterpret_cast<char*>(outPNG.data()),
                  static_cast<std::streamsize>(header.ThumbnailSize));
        return file.good();
    }

    // ========================================================================
    // ReadPayload
    // ========================================================================

    bool SaveGameFile::ReadPayload(const std::filesystem::path& path, std::vector<u8>& outPayload)
    {
        OLO_PROFILE_FUNCTION();

        SaveGameHeader header;
        if (!ReadHeader(path, header))
        {
            return false;
        }

        if (header.PayloadSize == 0)
        {
            outPayload.clear();
            return true;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file.seekg(static_cast<std::streamoff>(header.PayloadOffset));
        std::vector<u8> compressedData(header.PayloadSize);
        file.read(reinterpret_cast<char*>(compressedData.data()),
                  static_cast<std::streamsize>(header.PayloadSize));
        if (!file.good())
        {
            return false;
        }

        // Decompress if needed
        if (header.GetCompression() == SaveGameCompression::Zlib)
        {
            return Decompress(compressedData, header.PayloadUncompressedSize, outPayload);
        }

        outPayload = std::move(compressedData);
        return true;
    }

    // ========================================================================
    // ValidateChecksum
    // ========================================================================

    bool SaveGameFile::ValidateChecksum(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        SaveGameHeader header;
        if (!ReadHeader(path, header))
        {
            return false;
        }

        // Read all data after header
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file.seekg(0, std::ios::end);
        auto fileSize = static_cast<u64>(file.tellg());
        if (fileSize <= kSaveGameHeaderSize)
        {
            return header.ChecksumCRC32 == 0;
        }

        u64 dataSize = fileSize - kSaveGameHeaderSize;
        file.seekg(kSaveGameHeaderSize);
        std::vector<u8> data(dataSize);
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(dataSize));
        if (!file.good())
        {
            return false;
        }

        uLong crc = ::crc32(0L, Z_NULL, 0);
        crc = ::crc32(crc, data.data(), static_cast<uInt>(data.size()));
        return static_cast<u32>(crc) == header.ChecksumCRC32;
    }

    // ========================================================================
    // Compress / Decompress (zlib)
    // ========================================================================

    bool SaveGameFile::Compress(const std::vector<u8>& input, std::vector<u8>& output)
    {
        OLO_PROFILE_FUNCTION();

        if (input.empty())
        {
            output.clear();
            return true;
        }

        uLongf destLen = compressBound(static_cast<uLong>(input.size()));
        output.resize(destLen);

        int result = compress2(output.data(), &destLen,
                               input.data(), static_cast<uLong>(input.size()),
                               Z_DEFAULT_COMPRESSION);
        if (result != Z_OK)
        {
            OLO_CORE_ERROR("[SaveGameFile] zlib compress2 failed with code {}", result);
            output.clear();
            return false;
        }

        output.resize(destLen);
        return true;
    }

    bool SaveGameFile::Decompress(const std::vector<u8>& input, u64 uncompressedSize, std::vector<u8>& output)
    {
        OLO_PROFILE_FUNCTION();

        if (input.empty() || uncompressedSize == 0)
        {
            output.clear();
            return true;
        }

        output.resize(uncompressedSize);
        uLongf destLen = static_cast<uLongf>(uncompressedSize);

        int result = uncompress(output.data(), &destLen,
                                input.data(), static_cast<uLong>(input.size()));
        if (result != Z_OK)
        {
            OLO_CORE_ERROR("[SaveGameFile] zlib uncompress failed with code {}", result);
            output.clear();
            return false;
        }

        output.resize(destLen);
        return true;
    }

} // namespace OloEngine
