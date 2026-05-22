#include "OloEnginePCH.h"
#include "Preview/AssetThumbnailCache.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Preview/AssetPreviewRenderer.h"

namespace OloEngine
{
    Ref<Texture2D> AssetThumbnailCache::GetMaterialThumbnail(AssetHandle materialHandle)
    {
        if (!materialHandle)
            return nullptr;

        if (auto it = m_Entries.find(materialHandle); it != m_Entries.end())
        {
            TouchEntry(materialHandle, it->second);
            return it->second.Thumbnail;
        }

        if (!AssetPreviewRenderer::IsInitialized())
            return nullptr;

        auto material = AssetManager::GetAsset<MaterialAsset>(materialHandle);
        if (!material)
            return nullptr;

        Ref<Texture2D> thumbnail = AssetPreviewRenderer::RenderMaterialPreview(material);
        if (!thumbnail)
            return nullptr;

        return InsertEntry(materialHandle, std::move(thumbnail));
    }

    Ref<Texture2D> AssetThumbnailCache::GetMeshThumbnail(AssetHandle meshHandle)
    {
        if (!meshHandle)
            return nullptr;

        if (auto it = m_Entries.find(meshHandle); it != m_Entries.end())
        {
            TouchEntry(meshHandle, it->second);
            return it->second.Thumbnail;
        }

        if (!AssetPreviewRenderer::IsInitialized())
            return nullptr;

        auto mesh = AssetManager::GetAsset<Mesh>(meshHandle);
        if (!mesh)
            return nullptr;

        Ref<Texture2D> thumbnail = AssetPreviewRenderer::RenderMeshPreview(mesh, nullptr);
        if (!thumbnail)
            return nullptr;

        return InsertEntry(meshHandle, std::move(thumbnail));
    }

    void AssetThumbnailCache::Invalidate(AssetHandle handle)
    {
        auto it = m_Entries.find(handle);
        if (it == m_Entries.end())
            return;
        m_Order.erase(it->second.OrderIt);
        m_Entries.erase(it);
    }

    void AssetThumbnailCache::Clear()
    {
        m_Entries.clear();
        m_Order.clear();
    }

    void AssetThumbnailCache::TouchEntry(AssetHandle, CacheEntry& entry)
    {
        // Move to front of LRU list. `splice` keeps the iterator valid,
        // which we then stash back into the entry so subsequent erases
        // / touches still locate the right list node.
        m_Order.splice(m_Order.begin(), m_Order, entry.OrderIt);
        entry.OrderIt = m_Order.begin();
    }

    void AssetThumbnailCache::EvictIfFull()
    {
        while (m_Entries.size() >= kMaxEntries)
        {
            if (m_Order.empty())
                break;
            const AssetHandle victim = m_Order.back();
            m_Order.pop_back();
            m_Entries.erase(victim);
        }
    }

    Ref<Texture2D> AssetThumbnailCache::InsertEntry(AssetHandle handle, Ref<Texture2D> thumbnail)
    {
        EvictIfFull();
        m_Order.push_front(handle);
        m_Entries.emplace(handle, CacheEntry{ thumbnail, m_Order.begin() });
        return thumbnail;
    }
} // namespace OloEngine
