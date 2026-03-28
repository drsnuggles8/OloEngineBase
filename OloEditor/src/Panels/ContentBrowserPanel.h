#pragma once

#include "ContentBrowser/ContentBrowserItem.h"
#include "ContentBrowser/DirectoryTree.h"
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

        // Existing helpers
        Ref<Texture2D>& GetFileIcon(const std::filesystem::path& filepath);
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

        AssetSelectedCallback m_AssetSelectedCallback;
    };

} // namespace OloEngine
