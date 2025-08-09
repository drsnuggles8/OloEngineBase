#include "AssetManager.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/EnvironmentMap.h"

namespace OloEngine
{
    // Placeholder asset creation functions - similar to Hazel's approach
    static std::unordered_map<AssetType, std::function<Ref<Asset>()>> s_AssetPlaceholderTable =
    {
        { AssetType::Texture2D, []() -> Ref<Asset> {
            // Create a simple white texture
            TextureSpecification spec;
            spec.Width = 1;
            spec.Height = 1;
            spec.Format = ImageFormat::RGBA8;
            auto whiteTexture = Texture2D::Create(spec);
            u32 whitePixel = 0xFFFFFFFF;
            whiteTexture->SetData(&whitePixel, sizeof(u32));
            return whiteTexture;
        }},
        { AssetType::Font, []() -> Ref<Asset> {
            return Font::GetDefault();
        }},
        // Add more placeholder types as needed
    };

    Ref<Asset> AssetManager::GetPlaceholderAsset(AssetType type)
    {
        auto it = s_AssetPlaceholderTable.find(type);
        if (it != s_AssetPlaceholderTable.end())
            return it->second();

        OLO_CORE_WARN("No placeholder asset available for type: {}", AssetUtils::AssetTypeToString(type));
        return nullptr;
    }

}
