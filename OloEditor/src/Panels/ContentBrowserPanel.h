#pragma once

#include "OloEngine/Renderer/Texture.h"

#include <filesystem>
#include <unordered_map>
#include <functional>

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
        Shader
    };

    class ContentBrowserPanel
    {
      public:
        ContentBrowserPanel();

        void OnImGuiRender();

        // Callback for when an asset is selected (optional)
        using AssetSelectedCallback = std::function<void(const std::filesystem::path&, ContentFileType)>;
        void SetAssetSelectedCallback(AssetSelectedCallback callback)
        {
            m_AssetSelectedCallback = callback;
        }

      private:
        Ref<Texture2D>& GetFileIcon(const std::filesystem::path& filepath);
        ContentFileType GetFileType(const std::filesystem::path& filepath) const;
        void DrawFileContextMenu(const std::filesystem::path& path, ContentFileType fileType);
        void DrawCreateMenu();
        void CreateMeshPrimitiveFile(const std::string& primitiveType);

      private:
        std::filesystem::path m_BaseDirectory;
        std::filesystem::path m_CurrentDirectory;

        Ref<Texture2D> m_DirectoryIcon;
        Ref<Texture2D> m_FileIcon;
        Ref<Texture2D> m_ModelIcon;
        Ref<Texture2D> m_SceneIcon;
        Ref<Texture2D> m_ScriptIcon;
        Ref<Texture2D> m_AudioIcon;
        Ref<Texture2D> m_MaterialIcon;
        Ref<Texture2D> m_ShaderIcon;
        std::unordered_map<std::filesystem::path, Ref<Texture2D>> m_ImageIcons;

        AssetSelectedCallback m_AssetSelectedCallback;
    };

} // namespace OloEngine
