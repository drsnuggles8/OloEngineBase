#include "OloEnginePCH.h"
#include "AssetManager.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <unordered_set>

namespace OloEngine
{
    Ref<Asset> AssetManager::GetPlaceholderAsset(AssetType type)
    {
        return PlaceholderAssetManager::GetPlaceholderAsset(type);
    }

    Ref<Asset> AssetManager::UnwrapPlaceholder(const Ref<Asset>& asset)
    {
        if (!asset)
            return nullptr;

        // Placeholders wrap a concrete renderable; hand back that inner asset so the
        // typed GetAsset<T>() can cast it to the requested type.
        if (auto texture = asset.As<PlaceholderTexture>())
            return texture->GetTexture();
        if (auto material = asset.As<PlaceholderMaterial>())
            return material->GetMaterial();
        if (auto mesh = asset.As<PlaceholderMesh>())
            return mesh->GetMesh();

        // GenericPlaceholder / PlaceholderAudio / non-placeholder assets have no
        // castable inner renderable.
        return nullptr;
    }

    Ref<Asset> AssetManager::ResolveAssetOrPlaceholder(AssetHandle assetHandle, const Ref<Asset>& resolved, AssetType type)
    {
        if (resolved)
        {
            // A manager already substituted a placeholder on load (missing file /
            // corrupt payload). Unwrap it to its visible inner asset; generic
            // placeholders have none, so the caller gets null without a cast warning
            // (the substituting manager already warned once).
            if (PlaceholderAssetManager::IsPlaceholderAsset(resolved))
                return UnwrapPlaceholder(resolved);

            // A genuine asset whose type doesn't match the request — a real bug at the
            // call site, so keep surfacing it.
            OLO_CORE_WARN("AssetManager::GetAsset - Failed to cast asset {} from type {} to requested type {}",
                          assetHandle, AssetUtils::AssetTypeToString(resolved->GetAssetType()), AssetUtils::AssetTypeToString(type));
            return nullptr;
        }

        // resolved == null with a non-zero handle (the template already short-circuits
        // handle 0): a dangling/unresolvable reference. Warn once per handle so a broken
        // reference touched every frame doesn't flood the log, then substitute.
        {
            static FMutex s_WarnedMutex;
            static std::unordered_set<AssetHandle> s_WarnedHandles;
            TUniqueLock<FMutex> lock(s_WarnedMutex);
            if (s_WarnedHandles.insert(assetHandle).second)
            {
                OLO_CORE_WARN("AssetManager::GetAsset - Asset {} ({}) is missing/unresolvable; "
                              "substituting a placeholder. Check the scene/asset references.",
                              assetHandle, AssetUtils::AssetTypeToString(type));
            }
        }

        Ref<Asset> placeholder = PlaceholderAssetManager::GetPlaceholderAsset(type);
        Ref<Asset> inner = UnwrapPlaceholder(placeholder);
        // Renderable types unwrap to a visible stand-in; generic types have none, so
        // return the wrapper itself (non-null, won't cast to T but never crashes).
        return inner ? inner : placeholder;
    }

} // namespace OloEngine
