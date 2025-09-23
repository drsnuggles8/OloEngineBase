#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Audio/AudioLoader.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine 
{
    void AudioFileSourceSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // AudioFile assets don't require explicit serialization to file
        // as they're loaded based on metadata analysis of the source file
    }

    bool AudioFileSourceSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        // Get the file path for this asset
        auto filePath = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(filePath))
        {
            OLO_CORE_ERROR("AudioFileSourceSerializer: File does not exist: {}", filePath.string());
            // Create a default AudioFile asset
            asset = Ref<AudioFile>::Create();
            asset->SetHandle(metadata.Handle);
            return false;
        }

        // Use AudioLoader to get audio file information
        u32 numChannels = 0;
        u32 numFrames = 0; 
        f64 sampleRate = 0.0;
        f64 duration = 0.0;
        
        if (!Audio::AudioLoader::GetAudioFileInfo(filePath, numChannels, numFrames, sampleRate, duration))
        {
            OLO_CORE_ERROR("AudioFileSourceSerializer: Failed to get audio file info for: {}", filePath.string());
            // Create a default AudioFile asset
            asset = Ref<AudioFile>::Create();
            asset->SetHandle(metadata.Handle);
            return false;
        }

        // Get file size
        std::error_code ec;
        u64 fileSize = std::filesystem::file_size(filePath, ec);
        if (ec)
        {
            OLO_CORE_WARN("AudioFileSourceSerializer: Could not get file size for: {}", filePath.string());
            fileSize = 0;
        }

        // Create AudioFile asset with loaded metadata
        asset = Ref<AudioFile>::Create(duration, static_cast<u32>(sampleRate), 16, static_cast<u16>(numChannels), fileSize);
        asset->SetHandle(metadata.Handle);
        
        OLO_CORE_TRACE("AudioFileSourceSerializer: Loaded AudioFile asset {} - Duration: {:.2f}s, Channels: {}, SampleRate: {}", 
                      metadata.Handle, duration, numChannels, sampleRate);
        return true;
    }

    bool AudioFileSourceSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        outInfo.Offset = stream.GetStreamPosition();

        Ref<AudioFile> audioFile = AssetManager::GetAsset<AudioFile>(handle);
        if (!audioFile)
        {
            OLO_CORE_ERROR("AudioFileSourceSerializer: Failed to get AudioFile asset for handle {0}", handle);
            return false;
        }

        // Get the file path for this asset
        auto path = Project::GetAssetDirectory() / Project::GetAssetManager()->GetAssetMetadata(handle).FilePath;
        auto relativePath = std::filesystem::relative(path, Project::GetAssetDirectory());
        
        std::string filePath;
        if (relativePath.empty())
            filePath = path.string();
        else
            filePath = relativePath.string();

        // Serialize the file path so runtime can load the audio file
        stream.WriteString(filePath);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        
        OLO_CORE_TRACE("AudioFileSourceSerializer: Serialized AudioFile to pack - Handle: {0}, Path: {1}, Size: {2}", 
                       handle, filePath, outInfo.Size);
        return true;
    }

    Ref<Asset> AudioFileSourceSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        
        std::string filePath;
        stream.ReadString(filePath);

        // Create AudioFile asset with file path information
        // TODO: In runtime, analyze the audio file to get proper metadata
        Ref<AudioFile> audioFile = Ref<AudioFile>::Create();
        audioFile->SetHandle(assetInfo.Handle);

        OLO_CORE_TRACE("AudioFileSourceSerializer: Deserialized AudioFile from pack - Handle: {0}, Path: {1}", 
                       assetInfo.Handle, filePath);
        return audioFile;
    }
}
