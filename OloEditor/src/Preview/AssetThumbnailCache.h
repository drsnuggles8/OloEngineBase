#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture.h"

#include <list>
#include <unordered_map>

namespace OloEngine
{
    class Mesh;

    // -----------------------------------------------------------------------
    // AssetThumbnailCache
    //
    // Per-`AssetHandle` cache of preview thumbnails for materials and
    // meshes. Storing previews by asset handle (not by file path) means
    // asset moves and renames don't invalidate the cache, and lets the
    // cache map cleanly onto the existing asset/event plumbing in
    // EditorAssetManager. Material and mesh thumbnails share the same
    // bounded LRU pool: a single hot Content Browser view stays under
    // the eviction limit either way.
    //
    // Eviction: hard cap of `kMaxEntries` items, LRU. The bound mirrors
    // the existing 200-entry image-thumbnail cache in `ContentBrowserPanel`
    // and keeps GPU memory bounded (256² × 4 B × 256 = 64 MB worst case).
    // -----------------------------------------------------------------------
    class AssetThumbnailCache
    {
      public:
        static constexpr sizet kMaxEntries = 256;

        AssetThumbnailCache() = default;
        ~AssetThumbnailCache() = default;
        AssetThumbnailCache(const AssetThumbnailCache&) = delete;
        AssetThumbnailCache& operator=(const AssetThumbnailCache&) = delete;

        // Return the cached thumbnail for `materialHandle`, rendering one
        // on demand if absent (and the renderer is initialised). May
        // return null when the renderer is unavailable, the handle does
        // not resolve to a `MaterialAsset`, or the on-demand render fails.
        [[nodiscard]] Ref<Texture2D> GetMaterialThumbnail(AssetHandle materialHandle);

        // Return the cached thumbnail for a mesh asset. Meshes load
        // through their own MeshSource serializer (no MaterialAsset
        // reference on the asset record), so the cache renders them with
        // a neutral default material. Cache miss → render → store.
        [[nodiscard]] Ref<Texture2D> GetMeshThumbnail(AssetHandle meshHandle);

        // Drop the cached thumbnail for `handle`. Called when the asset
        // file or one of its texture dependencies is edited so the next
        // `Get*Thumbnail` re-renders fresh data. No-op when the handle
        // is not present in the cache.
        void Invalidate(AssetHandle handle);

        // Drop everything. Used on project unload / renderer shutdown
        // and when the asset registry itself is being torn down.
        void Clear();

        [[nodiscard]] sizet Size() const
        {
            return m_Order.size();
        }

      private:
        // LRU bookkeeping. `m_Order` is most-recently-used at the front so
        // we can pop from the back for eviction in O(1). The map holds an
        // iterator into the list for O(1) lookup-and-touch.
        struct CacheEntry
        {
            Ref<Texture2D> Thumbnail;
            std::list<AssetHandle>::iterator OrderIt;
        };

        void TouchEntry(AssetHandle handle, CacheEntry& entry);
        void EvictIfFull();
        // Insert a freshly-rendered thumbnail and return a copy of the
        // shared pointer. Centralises the LRU bookkeeping shared by
        // GetMaterialThumbnail / GetMeshThumbnail.
        Ref<Texture2D> InsertEntry(AssetHandle handle, Ref<Texture2D> thumbnail);

      private:
        std::unordered_map<AssetHandle, CacheEntry> m_Entries;
        std::list<AssetHandle> m_Order;
    };
} // namespace OloEngine
