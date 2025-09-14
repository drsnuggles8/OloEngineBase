#pragma once

#include "AssetSerializer.h"

#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Scene/Scene.h"

namespace OloEngine
{
    class AssetImporter
    {
    public:
        static void Init();
        static void Shutdown();
        static void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset);
        static void Serialize(const Ref<Asset>& asset);
        [[nodiscard]] static bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset);
        static void RegisterDependencies(const AssetMetadata& metadata);
        
        [[nodiscard]] static bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo);
        [[nodiscard]] static Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo);
        [[nodiscard]] static Ref<Scene> DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& assetInfo);

        // Delete constructors and assignment operators to prevent instantiation
        AssetImporter() = delete;
        AssetImporter(const AssetImporter&) = delete;
        AssetImporter(AssetImporter&&) = delete;
        AssetImporter& operator=(const AssetImporter&) = delete;
        AssetImporter& operator=(AssetImporter&&) = delete;
    };

} // namespace OloEngine
