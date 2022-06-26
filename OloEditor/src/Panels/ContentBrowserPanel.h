#pragma once

#include "OloEngine/Renderer/Texture.h"

#include <filesystem>
#include <unordered_map>

namespace OloEngine {

	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();

		void OnImGuiRender();
	private:
		Ref<Texture2D>& GetFileIcon(const std::filesystem::path& filepath);
	private:
		std::filesystem::path m_CurrentDirectory;

		Ref<Texture2D> m_DirectoryIcon;
		Ref<Texture2D> m_FileIcon;
		std::unordered_map<std::filesystem::path, Ref<Texture2D>> m_ImageIcons;
	};

}
