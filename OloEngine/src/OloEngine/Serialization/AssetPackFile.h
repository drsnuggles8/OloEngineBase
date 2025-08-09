#pragma once

#include <map>
#include <vector>
#include "OloEngine/Asset/Asset.h"

namespace OloEngine
{
    struct AssetPackFile
    {
        static constexpr uint32_t MagicNumber = 0x504C4F4F; // "OLOO" in little endian
        static constexpr uint32_t Version = 3;

        struct AssetInfo
        {
            AssetHandle Handle;
            uint64_t PackedOffset;
            uint64_t PackedSize;
            AssetType Type;
            uint16_t Flags; // compressed type, etc.
        };
        
        struct SceneInfo
        {
            AssetHandle Handle;
            uint64_t PackedOffset = 0;
            uint64_t PackedSize = 0;
            uint16_t Flags = 0; // compressed type, etc.
            std::map<uint64_t, AssetInfo> Assets; // AssetHandle->AssetInfo
        };

        struct IndexTable
        {
            uint32_t AssetCount = 0;
            uint32_t SceneCount = 0;
            uint64_t PackedAppBinaryOffset = 0;
            uint64_t PackedAppBinarySize = 0;
        };

        struct FileHeader
        {
            uint32_t MagicNumber = AssetPackFile::MagicNumber;
            uint32_t Version = AssetPackFile::Version;
            uint64_t BuildVersion = 0; // Usually date/time format (eg. 202210061535)
            uint64_t IndexOffset = 0; // Offset to the index table
        };

        FileHeader Header;
        IndexTable Index;
        std::vector<AssetInfo> AssetInfos;
        std::vector<SceneInfo> SceneInfos;
    };

} // namespace OloEngine
