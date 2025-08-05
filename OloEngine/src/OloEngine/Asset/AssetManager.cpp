#include "AssetManager.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Font.h"

namespace OloEngine
{
    static std::unordered_map<AssetType, std::function<Ref<Asset>()>> s_AssetPlaceholderTable =
    {
        { AssetType::Texture2D, []() { return Renderer::GetWhiteTexture(); }},
        { AssetType::TextureCube, []() { return Renderer::GetWhiteTexture(); }},
        { AssetType::Environment, []() { return Renderer::GetEmptyEnvironment(); }},
        { AssetType::Font, []() { return Font::GetDefaultFont(); }},
    };

    Ref<Asset> AssetManager::GetPlaceholderAsset(AssetType type)
    {
        if (s_AssetPlaceholderTable.contains(type))
            return s_AssetPlaceholderTable.at(type)();

        return nullptr;
    }

}
