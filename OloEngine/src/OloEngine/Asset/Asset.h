#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/AssetTypes.h"
#include <type_traits>

namespace OloEngine
{
    using AssetHandle = UUID;

    /**
     * @brief Base class for all assets in the OloEngine asset management system.
     * 
     * All assets are identified by a unique UUID handle and provide type information
     * for the asset management system. Assets are reference counted and thread-safe.
     */
    class Asset : public RefCounted
    {
    public:
        AssetHandle m_Handle = 0;
        u16 m_Flags = (u16)AssetFlag::None;

        Asset() = default;
        virtual ~Asset() = default;

        static AssetType GetStaticType() { return AssetType::None; }
        virtual AssetType GetAssetType() const { return AssetType::None; }

        /**
         * @brief Called when a dependency of this asset is updated
         * @param handle Handle of the updated dependency
         */
        virtual void OnDependencyUpdated(AssetHandle handle) { (void)handle; }

        bool operator==(const Asset& other) const
        {
            return m_Handle == other.m_Handle;
        }

        bool operator!=(const Asset& other) const
        {
            return !(*this == other);
        }

    private:
        // If you want to find out whether assets are valid or missing, use AssetManager::IsAssetValid(handle), IsAssetMissing(handle)
        // This cleans up and removes inconsistencies from rest of the code.
        // You simply go AssetManager::GetAsset<Whatever>(handle), and so long as you get a non-null pointer back, you're good to go.
        // No IsValid(), IsFlagSet(AssetFlag::Missing) etc. etc. all throughout the code.
        friend class AssetManager;
        friend class EditorAssetManager;
        friend class RuntimeAssetManager;
        friend class AssimpMeshImporter;
        friend class TextureSerializer;

        bool IsValid() const { return ((m_Flags & (u16)AssetFlag::Missing) | (m_Flags & (u16)AssetFlag::Invalid)) == 0; }

        bool IsFlagSet(AssetFlag flag) const { return (u16)flag & m_Flags; }
        void SetFlag(AssetFlag flag, bool value = true)
        {
            if (value)
                m_Flags |= (u16)flag;
            else
                m_Flags &= ~(u16)flag;
        }
    };

    /**
     * @brief Audio file asset containing metadata about audio files
     * 
     * Stores audio file properties such as duration, sampling rate, bit depth,
     * number of channels, and file size for audio asset management.
     */
    class AudioFile : public Asset
    {
    public:
        double m_Duration;
        u32 m_SamplingRate;
        u16 m_BitDepth;
        u16 m_NumChannels;
        u64 m_FileSize;

        AudioFile() : m_Duration(0.0), m_SamplingRate(0), m_BitDepth(0), m_NumChannels(0), m_FileSize(0) {}
        AudioFile(double duration, u32 samplingRate, u16 bitDepth, u16 numChannels, u64 fileSize)
            : m_Duration(duration), m_SamplingRate(samplingRate), m_BitDepth(bitDepth), m_NumChannels(numChannels), m_FileSize(fileSize)
        {
        }

        static AssetType GetStaticType() { return AssetType::Audio; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }
    };

    /**
     * @brief Asynchronous asset loading result container
     * 
     * Used for async asset loading operations, contains the loaded asset
     * and a flag indicating whether the loading operation is complete.
     * 
     * @tparam T Asset type being loaded
     */
    template<typename T>
    struct AsyncAssetResult
    {
        Ref<T> Asset;
        bool IsReady = false;

        AsyncAssetResult() = default;
        AsyncAssetResult(const AsyncAssetResult<T>& other) = default;

        AsyncAssetResult(Ref<T> asset, bool isReady = false)
            : Asset(asset), IsReady(isReady) {}

        template<typename T2, typename = std::enable_if_t<
            std::is_base_of_v<::OloEngine::Asset, T2> && 
            std::is_base_of_v<::OloEngine::Asset, T>
        >>
        AsyncAssetResult(const AsyncAssetResult<T2>& other)
            : Asset(other.Asset.template As<T>()), IsReady(other.IsReady) {}

        operator Ref<T>() const { return Asset; }
        operator bool() const { return IsReady; }
    };

}
