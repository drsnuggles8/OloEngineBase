// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "ContentBrowserPanel.h"
#include "OloEngine/Project/Project.h"

#include <imgui.h>

namespace OloEngine
{
	ContentBrowserPanel::ContentBrowserPanel()
	{
		m_BaseDirectory = Project::GetAssetDirectory();
		m_CurrentDirectory = m_BaseDirectory;
		m_DirectoryIcon = Texture2D::Create("Resources/Icons/ContentBrowser/DirectoryIcon.png");
		m_FileIcon = Texture2D::Create("Resources/Icons/ContentBrowser/FileIcon.png");
	}

	void ContentBrowserPanel::OnImGuiRender()
	{
		ImGui::Begin("Content Browser");

		if ((m_CurrentDirectory != std::filesystem::path(m_BaseDirectory)) && (ImGui::Button("<-")))
		{
			m_CurrentDirectory = m_CurrentDirectory.parent_path();
		}

		static f32 padding = 16.0f;
		static f32 thumbnailSize = 128.0f;
		const f32 cellSize = thumbnailSize + padding;

		const f32 panelWidth = ImGui::GetContentRegionAvail().x;
		auto columnCount = static_cast<int>(panelWidth / cellSize);
		columnCount = std::max(columnCount, 1);

		ImGui::Columns(columnCount, nullptr, false);

		for (auto& directoryEntry : std::filesystem::directory_iterator(m_CurrentDirectory))
		{
			const auto& path = directoryEntry.path();
			const std::string filenameString = path.filename().string();

			ImGui::PushID(filenameString.c_str());
			const Ref<Texture2D> icon = directoryEntry.is_directory() ? m_DirectoryIcon : GetFileIcon(directoryEntry.path());
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			ImGui::ImageButton(reinterpret_cast<ImTextureID>(static_cast<u64>(icon->GetRendererID())), { thumbnailSize, thumbnailSize }, { 0, 1 }, { 1, 0 });

			if (ImGui::BeginDragDropSource())
			{
				std::filesystem::path relativePath(path);
				wchar_t const* const itemPath = relativePath.c_str();
				ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath, (std::wcslen(itemPath) + 1) * sizeof(wchar_t));
				ImGui::EndDragDropSource();
			}

			ImGui::PopStyleColor();

			if ((ImGui::IsItemHovered()) && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) && (directoryEntry.is_directory()))
			{
				m_CurrentDirectory /= path.filename();
			}
			ImGui::TextWrapped(filenameString.c_str());

			ImGui::NextColumn();

			ImGui::PopID();
		}

		ImGui::Columns(1);

		ImGui::SliderFloat("Thumbnail Size", &thumbnailSize, 16, 512);
		ImGui::SliderFloat("Padding", &padding, 0, 32);

		// Get the total count of the files in the current directory.
		int totalCount = 0;
		for (auto& _ : std::filesystem::directory_iterator(m_CurrentDirectory))
		{
			totalCount++;
		}

		ImGui::Separator();  // Draw a line to separate the status bar from the rest of the content.

		// Change the background color for the status bar.
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.25f, 0.29f, 0.5f));

		// Create a child window for the status bar.
		ImGui::BeginChild("status_bar", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 20), true, 0);

		// Change the text color for the status bar.
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));

		// Add status bar information.
		ImGui::Text("Total files: %d", totalCount);
		ImGui::SameLine();
		ImGui::Text("Current path: %s", m_CurrentDirectory.string().c_str());

		// Restore the colors we changed.
		ImGui::PopStyleColor(2);

		ImGui::EndChild();  // End of child window.
		ImGui::End();
	}

	Ref<Texture2D>& ContentBrowserPanel::GetFileIcon(const std::filesystem::path& filepath)
	{
		if (m_ImageIcons.contains(filepath))
		{
			return m_ImageIcons[filepath];
		}

		if (std::string extension = filepath.extension().string(); (extension == ".png") || (extension == ".jpg"))
		{
			auto imageIcon = Texture2D::Create(filepath.string());
			if (imageIcon->IsLoaded())
			{
				auto& icon = m_ImageIcons[filepath] = imageIcon;
				return icon;
			}
			else
			{
				OLO_WARN("Could not load texture {0}", extension);
			}
		}
		return m_FileIcon;
	}

}
