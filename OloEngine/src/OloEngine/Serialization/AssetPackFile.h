#pragma once

#include <map>
#include <utility>
#include <vector>
#include <filesystem>
#include "OloEngine/Asset/Asset.h"

namespace OloEngine
{
    struct AssetPackFile
    {
        static constexpr u32 MagicNumber = 0x504C4F4F; // "OLOO" in little endian

        // The one pack layout this build reads (docs/agent-rules/binary-format-versioning.md).
        // AssetPack::Load rejects any other Header.Version -- older or newer -- with a
        // "rebuild the pack" error; there is no migration and no per-field version gate.
        // Bump it whenever a record layout changes. v6: the embedded ImportedMaterialCodec
        // blob in MeshSource records changed layout (#1499), so older packs must be
        // rebuilt, not half-read. v7: the builder places the data by the ON-DISK record
        // sizes below; every v6 pack's offsets ran 4 bytes per index entry past the
        // bytes they name, so none of them can be read (#1533).
        static constexpr u32 Version = 7;

        // ON-DISK record sizes. Every field is written with WriteRaw, so a record is
        // the sum of its fields, never sizeof() of the struct: AssetInfo's 28 bytes pad
        // to 32 in memory, and an index sized with sizeof() put every asset's recorded
        // offset 4 bytes per entry past its data (#1533). The builder plans offsets
        // from these and checks the stream against the plan as it writes.
        static constexpr u64 FileHeaderRecordSize = sizeof(u32) + sizeof(u32) + sizeof(u64) + sizeof(u64);
        static constexpr u64 IndexTableRecordSize = sizeof(u32) + sizeof(u32) + sizeof(u64) + sizeof(u64);
        static constexpr u64 AssetInfoRecordSize =
            sizeof(AssetHandle) + sizeof(u64) + sizeof(u64) + sizeof(AssetType) + sizeof(u16);
        // A SceneInfo record without its asset map: Handle, PackedOffset, PackedSize,
        // Flags and the map's count.
        static constexpr u64 SceneInfoRecordSize = sizeof(AssetHandle) + sizeof(u64) + sizeof(u64) + sizeof(u16) + sizeof(u32);
        // One entry of a scene's asset map: the key, then an AssetInfo record.
        static constexpr u64 SceneAssetRecordSize = sizeof(u64) + AssetInfoRecordSize;

        struct AssetInfo
        {
            AssetHandle Handle;
            u64 PackedOffset;
            u64 PackedSize;
            AssetType Type;
            u16 Flags; // compressed type, etc.
        };

        struct SceneInfo
        {
            AssetHandle Handle;
            u64 PackedOffset = 0;
            u64 PackedSize = 0;
            u16 Flags = 0;                   // compressed type, etc.
            std::map<u64, AssetInfo> Assets; // AssetHandle->AssetInfo
        };

        struct IndexTable
        {
            u32 AssetCount = 0;
            u32 SceneCount = 0;
            u64 PackedAppBinaryOffset = 0;
            u64 PackedAppBinarySize = 0;
        };

        struct FileHeader
        {
            u32 MagicNumber = AssetPackFile::MagicNumber;
            u32 Version = AssetPackFile::Version;
            u64 BuildVersion = 0; // Usually date/time format (eg. 202210061535)
            u64 IndexOffset = 0;  // Offset to the index table
        };

        FileHeader Header;
        IndexTable Index;
        std::vector<AssetInfo> AssetInfos;
        std::vector<SceneInfo> SceneInfos;

        // Temporary data used during asset pack building, not to be serialized
        std::vector<std::pair<AssetHandle, std::filesystem::path>> TempAssetFiles;
        // Where the plan put the first asset's bytes: the writer must reach exactly
        // this offset after the index and the script module.
        u64 TempDataStartOffset = 0;
    };

} // namespace OloEngine
