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

        // v4 (issue #629): MeshSource records carry a trailing virtualized-geometry
        // (cluster LOD DAG) blob. Appended at the END of the MeshSource payload and
        // gated on `stream.GetArchiveVersion() >= kVirtualMeshPackVersion` at the read
        // site, so a v1-v3 pack — which never wrote those bytes — still reads cleanly.
        static constexpr u32 Version = 4;

        // The pack version that introduced the MeshSource virtual-mesh blob. Read sites
        // gate on this rather than on `Version` so the constant stays meaningful after
        // the next bump.
        static constexpr u32 VirtualMeshPackVersion = 4;

        // Oldest FileHeader::Version this build will still load (issue #454). A pack
        // built by a newer engine (Header.Version > Version) is rejected outright --
        // this build doesn't know its layout and guessing would corrupt asset data.
        // A pack in [MinSupportedVersion, Version) is accepted; AssetPack::Load runs it
        // through MigrateAssetPackIndex (a no-op today, same shape as
        // SceneSerializer's MigrateSceneYAML / SaveGame's per-field HasFieldsSince gate)
        // so the next version bump has a place to add real migration instead of quietly
        // misreading old field layouts. If a future version adds/removes a fixed-layout
        // field in AssetInfo/SceneInfo/IndexTable, gate that read behind
        // `Header.Version >= <version it was introduced in>` at the read site in
        // AssetPack::Load, mirroring SaveGameComponentSerializer's HasFieldsSince pattern.
        static constexpr u32 MinSupportedVersion = 1;

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
    };

} // namespace OloEngine
