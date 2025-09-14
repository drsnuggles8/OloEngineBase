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
        static constexpr u32 Version = 3;

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
            u16 Flags = 0; // compressed type, etc.
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
            u64 IndexOffset = 0; // Offset to the index table
        };

        FileHeader Header;
        IndexTable Index;
        std::vector<AssetInfo> AssetInfos;
        std::vector<SceneInfo> SceneInfos;
        
        // Temporary data used during asset pack building, not to be serialized
        std::vector<std::pair<AssetHandle, std::filesystem::path>> TempAssetFiles;
    };

} // namespace OloEngine
