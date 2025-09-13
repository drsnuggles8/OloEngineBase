#pragma once

#include "OloEngine/Asset/AssetSerializer.h"

namespace OloEngine 
{
    /**
     * @brief Serializer for AudioFile assets
     * 
     * Handles serialization and deserialization of audio file metadata including
     * duration, sampling rate, bit depth, channels, and file size information.
     * 
     * For asset pack serialization, uses file path approach to enable runtime loading
     * of audio files from the original source files.
     */
    class AudioFileSourceSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };
}
