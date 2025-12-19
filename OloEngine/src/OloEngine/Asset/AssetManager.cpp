#include "AssetManager.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/PlaceholderAsset.h"

namespace OloEngine
{
    Ref<Asset> AssetManager::GetPlaceholderAsset(AssetType type)
    {
        return PlaceholderAssetManager::GetPlaceholderAsset(type);
    }

} // namespace OloEngine
