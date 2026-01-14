#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Audio/AudioSource.h"

#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <glm/glm.hpp>
#include <unordered_map>

namespace OloEngine
{
    /**
     * @brief Base class for placeholder assets
     *
     * Placeholder assets are used as fallbacks when:
     * - An asset fails to load
     * - An asset is missing
     * - During async loading (temporary placeholder)
     * - Asset is corrupted or invalid
     *
     * Each placeholder provides basic functionality and visual/audio feedback
     * to indicate that it's a temporary substitute.
     */
    class PlaceholderAsset : public Asset
    {
      public:
        PlaceholderAsset(AssetType type) : m_PlaceholderType(type)
        {
            // Placeholder assets explicitly use handle 0 (invalid)
            SetHandle(0);
        }
        virtual ~PlaceholderAsset() = default;

        AssetType GetAssetType() const override
        {
            return m_PlaceholderType;
        }
        bool IsPlaceholder() const
        {
            return true;
        }

      protected:
        AssetType m_PlaceholderType;
    };

    /**
     * @brief Placeholder texture with a distinctive pattern
     */
    class PlaceholderTexture : public PlaceholderAsset
    {
      public:
        PlaceholderTexture();

        Ref<Texture2D> GetTexture() const
        {
            return m_Texture;
        }

      private:
        Ref<Texture2D> m_Texture;
        void CreatePlaceholderTexture();
    };

    /**
     * @brief Placeholder material with default properties
     */
    class PlaceholderMaterial : public PlaceholderAsset
    {
      public:
        PlaceholderMaterial();
        virtual ~PlaceholderMaterial() = default;

        Ref<MaterialAsset> GetMaterial() const
        {
            return m_Material;
        }

      private:
        Ref<MaterialAsset> m_Material;
        void CreatePlaceholderMaterial();
    };

    /**
     * @brief Placeholder mesh (simple cube or sphere)
     */
    class PlaceholderMesh : public PlaceholderAsset
    {
      public:
        PlaceholderMesh();
        virtual ~PlaceholderMesh() = default;

        Ref<Mesh> GetMesh() const
        {
            return m_Mesh;
        }

      private:
        Ref<Mesh> m_Mesh;
        void CreatePlaceholderMesh();
    };

    /**
     * @brief Placeholder audio source (silent or beep)
     */
    class PlaceholderAudio : public PlaceholderAsset
    {
      public:
        PlaceholderAudio();
        virtual ~PlaceholderAudio() = default;

        Ref<AudioSource> GetAudioSource() const
        {
            return m_AudioSource;
        }

      private:
        Ref<AudioSource> m_AudioSource;
        void CreatePlaceholderAudio();
    };

    /**
     * @brief Generic placeholder for other asset types
     */
    class GenericPlaceholder : public PlaceholderAsset
    {
      public:
        GenericPlaceholder(AssetType type) : PlaceholderAsset(type) {}
        virtual ~GenericPlaceholder() = default;
    };

    /**
     * @brief Placeholder asset manager
     *
     * Manages creation and caching of placeholder assets for different types.
     * Ensures only one placeholder instance per asset type to save memory.
     */
    class PlaceholderAssetManager
    {
      public:
        static void Initialize();
        static void Shutdown();

        /**
         * @brief Get a placeholder asset for the specified type
         * @param type Asset type to get placeholder for
         * @return Reference to placeholder asset, never null
         */
        static Ref<Asset> GetPlaceholderAsset(AssetType type);

        /**
         * @brief Check if an asset is a placeholder
         * @param asset Asset to check
         * @return True if the asset is a placeholder
         */
        static bool IsPlaceholderAsset(const Ref<Asset>& asset);

        /**
         * @brief Get statistics about placeholder usage
         */
        static sizet GetPlaceholderCount()
        {
            TUniqueLock<FMutex> lock(s_PlaceholderMutex);
            return s_PlaceholderAssets.size();
        }

      private:
        static std::unordered_map<AssetType, Ref<Asset>> s_PlaceholderAssets;
        static FMutex s_PlaceholderMutex;
        static bool s_Initialized;

        static Ref<Asset> CreatePlaceholderAsset(AssetType type);
    };

} // namespace OloEngine
