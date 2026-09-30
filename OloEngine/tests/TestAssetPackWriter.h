#pragma once

// Writes a synthetic .olopack for tests that need a cooked pack without the
// AssetPackBuilder: every entry's bytes are laid out exactly as AssetPack::Load
// reads them. Shared by RuntimeAssetPackTest and RuntimeAssetLoadCancelTest.

#include <gtest/gtest.h>

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    // One asset entry to embed in a synthetic pack. Every entry goes into the
    // AssetInfo table; scene entries additionally get a SceneInfo whose offset
    // points at the data (the AssetInfo offset is intentionally left 0 to mirror
    // AssetPackBuilder, which never backfills scene AssetInfo offsets).
    struct PackEntry
    {
        AssetHandle Handle = 0;
        AssetType Type = AssetType::None;
        std::string Data; // raw bytes written at the entry's data offset
        bool IsScene = false;
    };

    // Write a valid .olopack with the given entries, computing data offsets up
    // front (no seek-back) so the layout matches what AssetPack::Load reads.
    inline void WritePack(const std::filesystem::path& path, const std::vector<PackEntry>& entries)
    {
        const u64 assetCount = entries.size();
        u64 sceneCount = 0;
        for (const auto& e : entries)
        {
            if (e.IsScene)
                ++sceneCount;
        }

        // Per-record byte sizes must match the field-by-field reads in AssetPack::Load.
        const u64 headerBytes = sizeof(AssetPackFile::FileHeader);
        const u64 indexBytes = sizeof(u32) * 2 + sizeof(u64) * 2;
        const u64 assetInfoBytes = sizeof(AssetHandle) + sizeof(u64) * 2 + sizeof(AssetType) + sizeof(u16);
        const u64 sceneInfoBytes = sizeof(AssetHandle) + sizeof(u64) * 2 + sizeof(u16) + sizeof(u32); // nested count = 0

        u64 cursor = headerBytes + indexBytes + assetCount * assetInfoBytes + sceneCount * sceneInfoBytes;

        std::vector<u64> dataOffset(entries.size());
        std::vector<u64> dataSize(entries.size());
        for (sizet i = 0; i < entries.size(); ++i)
        {
            dataOffset[i] = cursor;
            dataSize[i] = entries[i].Data.size();
            cursor += dataSize[i];
        }

        FileStreamWriter writer(path);
        ASSERT_TRUE(writer.IsStreamGood());

        // Header
        u32 magic = AssetPackFile::MagicNumber;
        u32 version = AssetPackFile::Version;
        u64 buildVersion = 1;
        u64 indexOffset = headerBytes;
        writer.WriteRaw(magic);
        writer.WriteRaw(version);
        writer.WriteRaw(buildVersion);
        writer.WriteRaw(indexOffset);

        // Index table
        u32 assetCount32 = static_cast<u32>(assetCount);
        u32 sceneCount32 = static_cast<u32>(sceneCount);
        u64 zero64 = 0;
        writer.WriteRaw(assetCount32);
        writer.WriteRaw(sceneCount32);
        writer.WriteRaw(zero64); // PackedAppBinaryOffset
        writer.WriteRaw(zero64); // PackedAppBinarySize

        // Asset infos: Handle, PackedOffset, PackedSize, Type, Flags
        for (sizet i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            // Scenes mirror the builder: their AssetInfo offset/size stay 0 so the
            // load path is forced through the SceneInfo table.
            u64 off = e.IsScene ? 0ull : dataOffset[i];
            u64 sz = e.IsScene ? 0ull : dataSize[i];
            u16 flags = 0;
            AssetHandle handle = e.Handle;
            AssetType type = e.Type;
            writer.WriteRaw(handle);
            writer.WriteRaw(off);
            writer.WriteRaw(sz);
            writer.WriteRaw(type);
            writer.WriteRaw(flags);
        }

        // Scene infos: Handle, PackedOffset, PackedSize, Flags, u32 sceneAssetCount
        for (sizet i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            if (!e.IsScene)
                continue;
            u16 flags = 0;
            u32 sceneAssetCount = 0;
            AssetHandle handle = e.Handle;
            u64 off = dataOffset[i];
            u64 sz = dataSize[i];
            writer.WriteRaw(handle);
            writer.WriteRaw(off);
            writer.WriteRaw(sz);
            writer.WriteRaw(flags);
            writer.WriteRaw(sceneAssetCount);
        }

        // Data section
        for (const auto& e : entries)
        {
            if (!e.Data.empty())
                writer.WriteData(e.Data.data(), e.Data.size());
        }

        ASSERT_TRUE(writer.IsStreamGood());
    }

} // namespace OloEngine::Tests
