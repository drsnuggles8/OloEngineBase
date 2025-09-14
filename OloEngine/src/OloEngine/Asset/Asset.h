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
        Asset() = default;
        virtual ~Asset() = default;

        // Delete copy and move operations to prevent slicing and identity issues
        Asset(const Asset&) = delete;
        Asset& operator=(const Asset&) = delete;
        Asset(Asset&&) = delete;
        Asset& operator=(Asset&&) = delete;

        static AssetType GetStaticType() { return AssetType::None; }
        virtual AssetType GetAssetType() const { return AssetType::None; }

        /**
         * @brief Called when a dependency of this asset is updated
         * @param handle Handle of the updated dependency
         */
        virtual void OnDependencyUpdated(AssetHandle handle) { (void)handle; }

        /**
         * @brief Get the asset handle
         * @return The unique UUID handle for this asset
         */
        AssetHandle GetHandle() const { return m_Handle; }

        /**
         * @brief Get the asset flags
         * @return The current asset flags
         */
        u16 GetFlags() const { return m_Flags; }

    protected:
        /**
         * @brief Set the asset flags
         * @param flags The new asset flags
         */
        void SetFlags(u16 flags) { m_Flags = flags; }

        /**
         * @brief Set the asset handle
         * @param handle The unique UUID handle for this asset
         */
        void SetHandle(AssetHandle handle) { m_Handle = handle; }

    public:

        bool operator==(const Asset& other) const
        {
            return m_Handle == other.m_Handle;
        }

        bool operator!=(const Asset& other) const
        {
            return !(*this == other);
        }

        virtual bool operator==(AssetHandle handle) const
        {
            return m_Handle == handle;
        }

        virtual bool operator!=(AssetHandle handle) const
        {
            return m_Handle != handle;
        }

    private:
        AssetHandle m_Handle = 0;
        u16 m_Flags = (u16)AssetFlag::None;

        // If you want to find out whether assets are valid or missing, use AssetManager::IsAssetValid(handle), IsAssetMissing(handle)
        // This cleans up and removes inconsistencies from rest of the code.
        // You simply go AssetManager::GetAsset<Whatever>(handle), and so long as you get a non-null pointer back, you're good to go.
        // No IsValid(), IsFlagSet(AssetFlag::Missing) etc. etc. all throughout the code.
        friend class AssetManager;
        friend class EditorAssetManager;
        friend class RuntimeAssetManager;
        friend class AssimpMeshImporter;
        friend class TextureSerializer;
        friend class AssetImporter;
        friend class AssetSerializer;
        friend class EditorAssetSystem;
        friend class FontSerializer;
        friend class MaterialAssetSerializer;
        friend class EnvironmentSerializer;
        friend class AudioFileSourceSerializer;
        friend class SoundConfigSerializer;
        friend class PrefabSerializer;
        friend class SceneAssetSerializer;
        friend class MeshColliderSerializer;
        friend class ScriptFileSerializer;
        friend class MeshSourceSerializer;
        friend class MeshSerializer;
        friend class StaticMeshSerializer;
        friend class AnimationAssetSerializer;
        friend class AnimationGraphAssetSerializer;
        friend class SoundGraphSerializer;

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
    private:
        double m_Duration;
        u32    m_SamplingRate;
        u16    m_BitDepth;
        u16    m_NumChannels;
        u64    m_FileSize;

    public:
        AudioFile() : m_Duration(0.0), m_SamplingRate(0), m_BitDepth(0), m_NumChannels(0), m_FileSize(0) {}
        AudioFile(double duration, u32 samplingRate, u16 bitDepth, u16 numChannels, u64 fileSize)
            : m_Duration(duration), m_SamplingRate(samplingRate), m_BitDepth(bitDepth), m_NumChannels(numChannels), m_FileSize(fileSize)
        {
        }

        double GetDuration()      const { return m_Duration; }
        u32    GetSamplingRate()  const { return m_SamplingRate; }
        u16    GetBitDepth()      const { return m_BitDepth; }
        u16    GetNumChannels()   const { return m_NumChannels; }
        u64    GetFileSize()      const { return m_FileSize; }

        static AssetType GetStaticType() { return AssetType::Audio; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }
    };

    /**
     * @brief Script file asset containing metadata about script files
     * 
     * Stores script class namespace and name information for C# script assets
     * to enable proper script component instantiation and management.
     */
    class ScriptFileAsset : public Asset
    {
    private:
        std::string m_ClassNamespace;
        std::string m_ClassName;

    public:
        ScriptFileAsset() = default;
        ScriptFileAsset(const std::string& classNamespace, const std::string& className)
            : m_ClassNamespace(classNamespace), m_ClassName(className)
        {
        }
        ScriptFileAsset(std::string&& classNamespace, std::string&& className)
            : m_ClassNamespace(std::move(classNamespace)), m_ClassName(std::move(className))
        {
        }

        const std::string& GetClassNamespace() const { return m_ClassNamespace; }
        const std::string& GetClassName() const { return m_ClassName; }

        void SetClassNamespace(const std::string& classNamespace) { m_ClassNamespace = classNamespace; }
        void SetClassNamespace(std::string&& classNamespace) noexcept { m_ClassNamespace = std::move(classNamespace); }
        void SetClassName(const std::string& className) { m_ClassName = className; }
        void SetClassName(std::string&& className) noexcept { m_ClassName = std::move(className); }

        static AssetType GetStaticType() { return AssetType::ScriptFile; }
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
        Ref<T> Ptr;
        bool IsReady = false;

        AsyncAssetResult() = default;
        AsyncAssetResult(const AsyncAssetResult<T>& other) = default;

        AsyncAssetResult(Ref<T> asset, bool isReady = false)
            : Ptr(asset), IsReady(isReady) {}

        template<typename T2, typename = std::enable_if_t<
            std::is_base_of_v<::OloEngine::Asset, T2> && 
            std::is_base_of_v<::OloEngine::Asset, T> &&
            (std::is_base_of_v<T, T2> || std::is_base_of_v<T2, T>)
        >>
        AsyncAssetResult(const AsyncAssetResult<T2>& other)
            : Ptr(other.Ptr.template As<T>()), IsReady(other.IsReady) {}

        explicit operator Ref<T>() const { return Ptr; }
        explicit operator bool() const { return IsReady; }
    };

}
