#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Serialization/Archive.h"

#include <string>

namespace OloEngine
{
    // ========================================================================
    // Save Slot Type
    // ========================================================================

    enum class SaveSlotType : u8
    {
        Manual = 0,
        QuickSave,
        AutoSave
    };

    // ========================================================================
    // Save Game Metadata (variable-length, human-readable info)
    // ========================================================================

    struct SaveGameMetadata
    {
        std::string DisplayName;
        std::string SceneName;
        i64 TimestampUTC = 0; // Unix timestamp (seconds since epoch)
        f32 PlaytimeSeconds = 0.0f;
        SaveSlotType SlotType = SaveSlotType::Manual;
        u32 EntityCount = 0;
        bool ThumbnailAvailable = false;

        friend FArchive& operator<<(FArchive& ar, SaveGameMetadata& meta)
        {
            ar << meta.DisplayName;
            ar << meta.SceneName;
            ar << meta.TimestampUTC;
            ar << meta.PlaytimeSeconds;
            ar << meta.SlotType;
            ar << meta.EntityCount;
            ar << meta.ThumbnailAvailable;
            return ar;
        }
    };

    // ========================================================================
    // Save Game Header (fixed 128 bytes at file start)
    // ========================================================================

    static constexpr u32 kSaveGameMagic = 0x4F4C4F53; // "OLOS" in little-endian
    static constexpr u32 kSaveGameFormatVersion = 1;
    static constexpr u32 kSaveGameHeaderSize = 128;

    // Compression flags (stored in Header.Flags)
    enum class SaveGameCompression : u32
    {
        None = 0,
        Zlib = 1
    };

#pragma pack(push, 1)
    struct SaveGameHeader
    {
        // u32 block (24 bytes)
        u32 Magic = kSaveGameMagic;
        u32 FormatVersion = kSaveGameFormatVersion;
        u32 EngineVersion = 1; // Bump with breaking engine changes
        u32 Flags = 0;         // SaveGameCompression bits
        u32 ChecksumCRC32 = 0; // CRC32 over everything after the header
        u32 EntityCount = 0;

        // u64 block (56 bytes)
        u64 MetadataOffset = 0;
        u64 MetadataSize = 0;
        u64 ThumbnailOffset = 0;
        u64 ThumbnailSize = 0;
        u64 PayloadOffset = 0;
        u64 PayloadSize = 0;             // Compressed size on disk
        u64 PayloadUncompressedSize = 0; // Original size before compression

        u8 Reserved[128 - 80] = {}; // Padding to exactly 128 bytes

        [[nodiscard]] bool IsValid() const
        {
            return Magic == kSaveGameMagic && FormatVersion == kSaveGameFormatVersion;
        }

        // Check whether the save was produced by a compatible engine version
        [[nodiscard]] bool IsCompatible() const
        {
            return IsValid() && EngineVersion == 1;
        }

        [[nodiscard]] SaveGameCompression GetCompression() const
        {
            return static_cast<SaveGameCompression>(Flags & 0xFF);
        }

        void SetCompression(SaveGameCompression compression)
        {
            Flags = (Flags & ~0xFFu) | static_cast<u32>(compression);
        }
    };
#pragma pack(pop)

    static_assert(sizeof(SaveGameHeader) == kSaveGameHeaderSize, "SaveGameHeader must be exactly 128 bytes");

} // namespace OloEngine
