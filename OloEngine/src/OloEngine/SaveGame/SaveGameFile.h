#pragma once

#include "OloEngine/SaveGame/SaveGameTypes.h"

#include <filesystem>
#include <vector>

namespace OloEngine
{
    // Binary .olosave file reader/writer.
    // File layout: Header (128B) | Metadata blob | Thumbnail PNG | Compressed payload
    class SaveGameFile
    {
      public:
        // Write a complete .olosave file
        static bool Write(const std::filesystem::path& path,
                          const SaveGameHeader& header,
                          const SaveGameMetadata& metadata,
                          const std::vector<u8>& thumbnailPNG,
                          const std::vector<u8>& payload);

        // Read only the fixed header (128 bytes)
        static bool ReadHeader(const std::filesystem::path& path, SaveGameHeader& outHeader);

        // Read header + metadata (fast — no decompression)
        static bool ReadMetadata(const std::filesystem::path& path,
                                 SaveGameHeader& outHeader,
                                 SaveGameMetadata& outMetadata);

        // Read thumbnail PNG bytes
        static bool ReadThumbnail(const std::filesystem::path& path, std::vector<u8>& outPNG);

        // Read and decompress the payload
        static bool ReadPayload(const std::filesystem::path& path, std::vector<u8>& outPayload);

        // Validate CRC32 checksum of data after header
        static bool ValidateChecksum(const std::filesystem::path& path);

        // Compress raw data using zlib
        static bool Compress(const std::vector<u8>& input, std::vector<u8>& output);

        // Decompress zlib-compressed data
        static bool Decompress(const std::vector<u8>& input, u64 uncompressedSize, std::vector<u8>& output);
    };

} // namespace OloEngine
