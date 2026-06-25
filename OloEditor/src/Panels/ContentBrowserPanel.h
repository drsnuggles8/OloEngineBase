#pragma once

#include "ContentBrowser/ContentBrowserItem.h"
#include "ContentBrowser/DirectoryTree.h"
#include "Preview/AssetThumbnailCache.h"
#include "OloEngine/Renderer/Texture.h"

#include <filesystem>
#include <unordered_map>
#include <functional>

struct ImGuiContext;
struct ImGuiSettingsHandler;
struct ImGuiTextBuffer;

namespace OloEngine
{
    class ContentBrowserPanel
    {
      public:
        ContentBrowserPanel();
        ~ContentBrowserPanel();

        void OnImGuiRender();

        // Invalidate any cached preview thumbnail bound to this asset
        // handle. Also drops the corresponding file-path entry from the
        // panel's icon cache so the next render re-resolves through the
        // thumbnail cache and triggers a fresh PBR-sphere render.
        //
        // Called from EditorLayer's `OnAssetReloaded` so material edits
        // (texture re-imports, factor tweaks via the editor inspector,
        // etc.) propagate visually without restarting the editor.
        void InvalidateThumbnail(AssetHandle handle, const std::filesystem::path& path);

        // React to a brand-new asset auto-imported from disk by the editor's
        // filesystem watcher (EditorAssetManager::AutoImportNewAsset →
        // AssetImportedEvent → EditorLayer). Marks the directory that should
        // now contain `absoluteAssetPath` as needing a rescan, so the file
        // shows up on the next paint without a manual import or F5. The path is
        // absolute and resolved against this panel's asset root; paths outside
        // the asset tree are ignored.
        void OnAssetImported(const std::filesystem::path& absoluteAssetPath);

        // Drop every preview thumbnail — both the handle-keyed cache
        // and the path-keyed fast path. Called when a *texture* changes
        // because we don't track which materials reference which
        // textures; the cheap fix is to invalidate all material
        // thumbnails and re-render lazily on next paint.
        void ClearThumbnails();

        // Callback for when an asset is activated (double-click)
        using AssetSelectedCallback = std::function<void(const std::filesystem::path&, ContentFileType)>;
        void SetAssetSelectedCallback(AssetSelectedCallback callback)
        {
            m_AssetSelectedCallback = callback;
        }

      private:
        // Layout sections
        void DrawTopBar();
        void DrawDirectoryTree();
        void DrawDirectoryTreeNode(DirectoryInfo* dir);
        void DrawItemGrid();
        void DrawBottomBar();

        // Navigation
        void ChangeDirectory(DirectoryInfo* dir);
        void NavigateBack();
        void NavigateForward();

        // Data management
        void RebuildItemList();
        void RebuildBreadcrumbs();
        void RefreshIfDirty();
        void RefreshVisibleItems();
        void SafeRefreshSubtree(DirectoryInfo* node);

        // Search
        void UpdateSearchResults();
        void ClearSearch();

        // Selection
        [[nodiscard]] bool IsSelected(const std::filesystem::path& path) const;
        void Select(const std::filesystem::path& path);
        void Deselect(const std::filesystem::path& path);
        void DeselectAll();
        void SelectRange(size_t fromIndex, size_t toIndex);

        // Item operations
        void DeleteSelectedItems();
        void HandleKeyboardShortcuts();

        // Existing helpers. Returns by value (cheap — a `Ref<>` copy is
        // an atomic refcount bump) so the call site does not hold a
        // long-lived reference into `m_ImageIcons` or the thumbnail
        // cache; that lets `AssetThumbnailCache` evict its LRU entries
        // without leaving a dangling reference inside the panel.
        Ref<Texture2D> GetFileIcon(const std::filesystem::path& filepath);
        // Decode a video file's first frame into a preview texture (null on failure).
        Ref<Texture2D> DecodeVideoThumbnail(const std::filesystem::path& filepath);
        void DrawCreateMenu();
        void CreateMeshPrimitiveFile(const std::string& primitiveType);

        // Open in OS
        static void OpenInExplorer(const std::filesystem::path& path);
        static void OpenExternally(const std::filesystem::path& path);

        // Settings persistence (imgui.ini)
        static void* SettingsHandler_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name);
        static void SettingsHandler_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line);
        static void SettingsHandler_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf);
        void RegisterSettingsHandler();

        // Singleton instance pointer used by ImGui settings handler callbacks
        // (SettingsHandler_ReadOpen/ReadLine/WriteAll). Set in the constructor,
        // cleared in the destructor. Only one ContentBrowserPanel should exist
        // at a time. Not thread-safe — all access is on the main/UI thread.
        static ContentBrowserPanel* s_Instance;

      private:
        // Directory tree (cached — no per-frame filesystem I/O)
        DirectoryTree m_DirectoryTree;
        DirectoryInfo* m_CurrentDirectory = nullptr;
        DirectoryInfo* m_ForwardDirectory = nullptr;

        // Breadcrumbs
        std::vector<DirectoryInfo*> m_BreadcrumbTrail;

        // Items (rebuilt on directory change or search)
        std::vector<ContentBrowserItem> m_Items;

        // Selection
        std::vector<std::filesystem::path> m_SelectedItems;
        size_t m_LastSelectedIndex = 0;

        // Rename state
        std::filesystem::path m_RenamingItem;

        // Search
        char m_SearchBuffer[256] = {};
        bool m_IsSearchActive = false;
        bool m_FocusSearchNextFrame = false;

        // Delete confirmation
        bool m_ShowDeleteConfirmation = false;

        // Settings
        f32 m_ThumbnailSize = 128.0f;
        f32 m_Padding = 16.0f;

        // Icons
        Ref<Texture2D> m_DirectoryIcon;
        Ref<Texture2D> m_FileIcon;
        Ref<Texture2D> m_ModelIcon;
        Ref<Texture2D> m_SceneIcon;
        Ref<Texture2D> m_ScriptIcon;
        Ref<Texture2D> m_AudioIcon;
        Ref<Texture2D> m_MaterialIcon;
        Ref<Texture2D> m_ShaderIcon;
        Ref<Texture2D> m_DialogueIcon;
        Ref<Texture2D> m_SaveGameIcon;
        std::unordered_map<std::filesystem::path, Ref<Texture2D>> m_ImageIcons;
        std::unordered_map<ContentFileType, Ref<Texture2D>*> m_FileTypeIconMap;

        // Live previews for assets that can be rendered (materials today;
        // meshes deferred). Keyed by `AssetHandle` so a rename of the
        // backing file doesn't invalidate the entry.
        AssetThumbnailCache m_ThumbnailCache;

        // Set by `InvalidateThumbnail` / `ClearThumbnails` when the cache
        // changes; checked in `RefreshIfDirty` (top of OnImGuiRender)
        // which calls `RefreshVisibleItems` once per frame. Coalesces
        // bursts of asset-reload events (texture re-import dispatches
        // one event per touched texture) into a single grid rebuild.
        bool m_PendingVisibleItemsRefresh = false;

        AssetSelectedCallback m_AssetSelectedCallback;
    };

} // namespace OloEngine
