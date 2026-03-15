#include "OloEnginePCH.h"
#include "SaveGameFile.h"

#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <limits>

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
            SaveGameMetadata metaCopy = metadata;
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
            auto crc32Chunked = [](uLong crc, const u8* data, sizet size) -> uLong
            {
                while (size > 0)
                {
                    uInt chunk = static_cast<uInt>(std::min(size, static_cast<sizet>(std::numeric_limits<uInt>::max())));
                    crc = ::crc32(crc, data, chunk);
                    data += chunk;
                    size -= chunk;
                }
                return crc;
            };

            uLong crc = ::crc32(0L, Z_NULL, 0);
            if (!metaBlob.empty())
            {
                crc = crc32Chunked(crc, metaBlob.data(), metaBlob.size());
            }
            if (!thumbnailPNG.empty())
            {
                crc = crc32Chunked(crc, thumbnailPNG.data(), thumbnailPNG.size());
            }
            if (!payload.empty())
            {
                crc = crc32Chunked(crc, payload.data(), payload.size());
            }
            header.ChecksumCRC32 = static_cast<u32>(crc);
        }

        // Write to a temporary file, then atomically rename
        auto tempPath = path;
        tempPath += ".tmp";

        {
            std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                OLO_CORE_ERROR("[SaveGameFile] Failed to open '{}' for writing", tempPath.string());
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
            if (!file.good())
            {
                OLO_CORE_ERROR("[SaveGameFile] Write to '{}' failed", tempPath.string());
                file.close();
                std::error_code removeEc;
                std::filesystem::remove(tempPath, removeEc);
                return false;
            }
        }

        // Atomic rename (overwrites existing file)
        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec)
        {
            OLO_CORE_ERROR("[SaveGameFile] Rename '{}' -> '{}' failed: {}", tempPath.string(), path.string(), ec.message());
            std::filesystem::remove(tempPath, ec);
            return false;
        }

        return true;
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

        // Reject suspiciously large metadata (1 MB cap)
        static constexpr u64 kMaxMetadataSize = 1ULL << 20;
        if (outHeader.MetadataSize > kMaxMetadataSize)
        {
            OLO_CORE_ERROR("[SaveGameFile] MetadataSize {} exceeds cap", outHeader.MetadataSize);
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        // Bounds check: metadata must fit within the file (overflow-safe)
        file.seekg(0, std::ios::end);
        auto actualFileSize = static_cast<u64>(file.tellg());
        if (outHeader.MetadataSize > actualFileSize || outHeader.MetadataOffset > actualFileSize - outHeader.MetadataSize)
        {
            OLO_CORE_ERROR("[SaveGameFile] Metadata region exceeds file size");
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

        // Reject suspiciously large thumbnails (64 MB cap)
        static constexpr u64 kMaxThumbnailSize = 64ULL << 20;
        if (header.ThumbnailSize > kMaxThumbnailSize)
        {
            OLO_CORE_ERROR("[SaveGameFile] ThumbnailSize {} exceeds cap", header.ThumbnailSize);
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        // Bounds check: thumbnail must fit within the file (overflow-safe)
        file.seekg(0, std::ios::end);
        auto actualFileSize = static_cast<u64>(file.tellg());
        if (header.ThumbnailSize > actualFileSize || header.ThumbnailOffset > actualFileSize - header.ThumbnailSize)
        {
            OLO_CORE_ERROR("[SaveGameFile] Thumbnail region exceeds file size");
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

        // Reject suspiciously large compressed payloads (1 GB cap)
        static constexpr u64 kMaxCompressedPayloadSize = 1ULL << 30;
        if (header.PayloadSize > kMaxCompressedPayloadSize)
        {
            OLO_CORE_ERROR("[SaveGameFile] PayloadSize {} exceeds cap", header.PayloadSize);
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        // Bounds check: payload must fit within the file (overflow-safe)
        file.seekg(0, std::ios::end);
        auto actualFileSize = static_cast<u64>(file.tellg());
        if (header.PayloadSize > actualFileSize || header.PayloadOffset > actualFileSize - header.PayloadSize)
        {
            OLO_CORE_ERROR("[SaveGameFile] Payload region exceeds file size");
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
            // Cap uncompressed size to prevent excessive allocation (1 GB)
            static constexpr u64 kMaxUncompressedSize = 1ULL << 30;
            if (header.PayloadUncompressedSize > kMaxUncompressedSize)
            {
                OLO_CORE_ERROR("[SaveGameFile] PayloadUncompressedSize {} exceeds cap", header.PayloadUncompressedSize);
                return false;
            }
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

        // Read data after header in fixed-size chunks and compute CRC incrementally
        u64 dataSize = fileSize - kSaveGameHeaderSize;
        file.seekg(kSaveGameHeaderSize);
        u64 remaining = dataSize;
        uLong crc = ::crc32(0L, Z_NULL, 0);
        constexpr sizet kChunkSize = 64 * 1024; // 64 KB
        u8 chunkBuf[kChunkSize];

        while (remaining > 0)
        {
            auto toRead = static_cast<std::streamsize>(std::min(remaining, static_cast<u64>(kChunkSize)));
            file.read(reinterpret_cast<char*>(chunkBuf), toRead);
            if (!file.good() && !file.eof())
            {
                return false;
            }
            auto bytesRead = static_cast<sizet>(file.gcount());
            if (bytesRead == 0)
            {
                return false;
            }

            // Feed chunk to CRC (handle >4GB via sub-chunks for zlib's uInt limit)
            const u8* ptr = chunkBuf;
            sizet left = bytesRead;
            while (left > 0)
            {
                uInt chunk = static_cast<uInt>(std::min(left, static_cast<sizet>(std::numeric_limits<uInt>::max())));
                crc = ::crc32(crc, ptr, chunk);
                ptr += chunk;
                left -= chunk;
            }
            remaining -= bytesRead;
        }

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

        if (input.size() > static_cast<sizet>(std::numeric_limits<uLong>::max()))
        {
            OLO_CORE_ERROR("[SaveGameFile] Input too large for zlib compress ({} bytes)", input.size());
            output.clear();
            return false;
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

        if (destLen != static_cast<uLongf>(uncompressedSize))
        {
            OLO_CORE_ERROR("[SaveGameFile] Decompressed size mismatch: expected {} but got {}", uncompressedSize, static_cast<u64>(destLen));
            output.clear();
            return false;
        }

        output.resize(destLen);
        return true;
    }

} // namespace OloEngine
