#pragma once

#include "OloEngine/Renderer/Texture.h"

#include <filesystem>
#include <unordered_map>

namespace OloEngine
{
	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();

		void OnImGuiRender();
	private:
		AssetRef<Texture2D>& GetFileIcon(const std::filesystem::path& filepath);
	private:
		std::filesystem::path m_BaseDirectory;
		std::filesystem::path m_CurrentDirectory;

		AssetRef<Texture2D> m_DirectoryIcon;
		AssetRef<Texture2D> m_FileIcon;
		std::unordered_map<std::filesystem::path, AssetRef<Texture2D>> m_ImageIcons;
	};

}
