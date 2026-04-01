#pragma once

#include <filesystem>
#include <string>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/SlugData.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{

    class Font : public RendererResource
    {
      public:
        explicit Font(const std::filesystem::path& font);
        ~Font() override;

        [[nodiscard("Store this!")]] const SlugFontData* GetSlugData() const
        {
            return m_Data.get();
        }

        // Legacy accessor kept temporarily so callers compile during migration.
        // Returns nullptr — callers that used MSDFData should migrate to GetSlugData().
        [[deprecated("Use GetSlugData() instead")]] [[nodiscard("Store this!")]] const void* GetMSDFData() const
        {
            return nullptr;
        }

        Ref<Texture2D> GetCurveTexture() const
        {
            return m_Data ? m_Data->CurveTexture : nullptr;
        }

        Ref<Texture2D> GetBandTexture() const
        {
            return m_Data ? m_Data->BandTexture : nullptr;
        }

        // Legacy accessor — returns nullptr. Use GetCurveTexture() / GetBandTexture().
        [[deprecated("Use GetCurveTexture() / GetBandTexture() instead")]]
        Ref<Texture2D> GetAtlasTexture() const
        {
            return nullptr;
        }

        const std::string& GetName() const
        {
            return m_Name;
        }
        const std::string& GetPath() const
        {
            return m_Path;
        }

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Font;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        static Ref<Font> GetDefault();
        static Ref<Font> Create(const std::filesystem::path& font);

      private:
        Scope<SlugFontData> m_Data;
        std::string m_Name;
        std::string m_Path;
        bool m_IsLoaded = false;

      public:
        [[nodiscard]] bool IsLoaded() const
        {
            return m_IsLoaded;
        }
    };
} // namespace OloEngine
