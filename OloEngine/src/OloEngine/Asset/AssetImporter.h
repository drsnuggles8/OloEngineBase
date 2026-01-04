#pragma once

#include "AssetSerializer.h"

#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Scene/Scene.h"

namespace OloEngine
{
    /**
     * @brief Static utility class for importing and serializing assets
     *
     * AssetImporter manages a registry of AssetSerializer instances and routes
     * serialization/deserialization requests to the appropriate serializer based
     * on asset type.
     *
     * For async-safe loading, use the two-phase loading methods:
     * - TryLoadRawData(): Loads asset data without GPU resources (thread-safe)
     * - FinalizeFromRawData(): Creates GPU resources from raw data (main thread only)
     */
    class AssetImporter
    {
      public:
        static void Init();
        static void Shutdown();
        static void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset);
        static void Serialize(const Ref<Asset>& asset);

        /**
         * @brief Load asset data synchronously (creates GPU resources)
         * @param metadata The asset metadata
         * @param asset Output reference to the loaded asset
         * @return True if loading succeeded
         *
         * WARNING: This method may create GPU resources and should only be called
         * from the main thread for assets that don't support async loading.
         */
        [[nodiscard]] static bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset);

        /**
         * @brief Check if an asset type supports async loading
         * @param type The asset type to check
         * @return True if the asset type can be loaded asynchronously
         */
        [[nodiscard]] static bool SupportsAsyncLoading(AssetType type);

        /**
         * @brief Load raw asset data without creating GPU resources (thread-safe)
         * @param metadata The asset metadata
         * @param outRawData Output raw asset data
         * @return True if loading succeeded
         *
         * This method is safe to call from any thread. The returned raw data must
         * be finalized on the main thread using FinalizeFromRawData().
         */
        [[nodiscard]] static bool TryLoadRawData(const AssetMetadata& metadata, RawAssetData& outRawData);

        /**
         * @brief Create GPU resources from raw asset data (main thread only)
         * @param metadata The asset metadata
         * @param rawData The raw asset data from TryLoadRawData()
         * @param outAsset Output reference to the finalized asset
         * @return True if finalization succeeded
         *
         * WARNING: This method creates GPU resources and MUST be called from
         * the main thread (render thread).
         */
        [[nodiscard]] static bool FinalizeFromRawData(const AssetMetadata& metadata, RawAssetData& rawData, Ref<Asset>& outAsset);

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
