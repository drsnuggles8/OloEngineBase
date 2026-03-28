#pragma once

#include "OloEngine/Renderer/Texture.h"

#include <filesystem>
#include <string>

namespace OloEngine
{
    // File type classification for ContentBrowser
    enum class ContentFileType
    {
        Unknown,
        Directory,
        Image,
        Model3D,
        Scene,
        Script,
        Audio,
        Material,
        Shader,
        StreamingRegion,
        Dialogue,
        ShaderGraph,
        SaveGame
    };

    // Bitflag actions returned by ContentBrowserItem::Render()
    enum class ContentBrowserAction : u16
    {
        None = 0,
        Selected = 1 << 0,
        Deselected = 1 << 1,
        ClearSelections = 1 << 2,
        SelectToHere = 1 << 3,
        Hovered = 1 << 4,
        Activated = 1 << 5,
        StartRenaming = 1 << 6,
        Renamed = 1 << 7,
        Deleted = 1 << 8,
        ShowInExplorer = 1 << 9,
        OpenExternal = 1 << 10,
        Refresh = 1 << 11,
    };

    using CBActionResult = u16;

    inline CBActionResult operator|(CBActionResult a, ContentBrowserAction b)
    {
        return a | static_cast<CBActionResult>(b);
    }

    inline bool HasAction(CBActionResult result, ContentBrowserAction action)
    {
        return (result & static_cast<CBActionResult>(action)) != 0;
    }

    inline void SetAction(CBActionResult& result, ContentBrowserAction action)
    {
        result |= static_cast<CBActionResult>(action);
    }

    // Maps a file extension to a ContentFileType.
    ContentFileType GetFileTypeFromExtension(const std::filesystem::path& filepath);

    // Returns the drag-drop payload tag for a given file type.
    const char* GetDragDropPayloadType(ContentFileType type);

    // A single item (file or directory) displayed in the content browser grid.
    class ContentBrowserItem
    {
      public:
        ContentBrowserItem(const std::filesystem::path& absolutePath, ContentFileType type, const Ref<Texture2D>& icon);

        // Renders the item and returns action flags for the panel to process.
        CBActionResult Render(f32 thumbnailSize, bool isSelected, bool isRenaming);

        // Apply a committed rename. Returns true on success.
        bool CommitRename(const std::string& newName);

        [[nodiscard]] const std::filesystem::path& GetPath() const { return m_Path; }
        [[nodiscard]] ContentFileType GetType() const { return m_Type; }
        [[nodiscard]] const std::string& GetDisplayName() const { return m_DisplayName; }
        [[nodiscard]] bool IsDirectory() const { return m_Type == ContentFileType::Directory; }

        char* GetRenameBuffer() { return m_RenameBuffer; }

      private:
        void RenderContextMenu(CBActionResult& result);

        std::filesystem::path m_Path;
        std::string m_DisplayName;
        ContentFileType m_Type;
        Ref<Texture2D> m_Icon;
        char m_RenameBuffer[256] = {};
    };

    // Sorts items: directories first, then alphabetical by display name (case-insensitive).
    void SortContentBrowserItems(std::vector<ContentBrowserItem>& items);

} // namespace OloEngine
