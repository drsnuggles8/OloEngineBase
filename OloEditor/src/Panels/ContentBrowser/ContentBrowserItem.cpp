#include "OloEnginePCH.h"
#include "ContentBrowserItem.h"

#include <imgui.h>
#include <algorithm>

#ifdef OLO_PLATFORM_WINDOWS
#include <Windows.h>
#include <shellapi.h>
#endif

namespace OloEngine
{
    // Extension → ContentFileType map (shared with panel)
    static const std::unordered_map<std::string, ContentFileType> s_ExtensionToFileType = {
        // Images
        { ".png", ContentFileType::Image },
        { ".jpg", ContentFileType::Image },
        { ".jpeg", ContentFileType::Image },
        { ".tga", ContentFileType::Image },
        { ".bmp", ContentFileType::Image },
        { ".hdr", ContentFileType::Image },
        // 3D Models
        { ".obj", ContentFileType::Model3D },
        { ".fbx", ContentFileType::Model3D },
        { ".gltf", ContentFileType::Model3D },
        { ".glb", ContentFileType::Model3D },
        { ".dae", ContentFileType::Model3D },
        { ".3ds", ContentFileType::Model3D },
        { ".blend", ContentFileType::Model3D },
        // Primitives
        { ".primitive", ContentFileType::Model3D },
        // Scenes
        { ".olo", ContentFileType::Scene },
        { ".scene", ContentFileType::Scene },
        // Scripts
        { ".cs", ContentFileType::Script },
        { ".lua", ContentFileType::Script },
        // Audio
        { ".wav", ContentFileType::Audio },
        { ".mp3", ContentFileType::Audio },
        { ".ogg", ContentFileType::Audio },
        { ".flac", ContentFileType::Audio },
        // Materials
        { ".mat", ContentFileType::Material },
        { ".material", ContentFileType::Material },
        // Shaders
        { ".glsl", ContentFileType::Shader },
        { ".vert", ContentFileType::Shader },
        { ".frag", ContentFileType::Shader },
        { ".hlsl", ContentFileType::Shader },
        // Streaming Regions
        { ".oloregion", ContentFileType::StreamingRegion },
        // Dialogue
        { ".olodialogue", ContentFileType::Dialogue },
        // Shader Graphs
        { ".olosg", ContentFileType::ShaderGraph },
        // Save Games
        { ".olosave", ContentFileType::SaveGame },
    };

    ContentFileType GetFileTypeFromExtension(const std::filesystem::path& filepath)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(filepath, ec))
            return ContentFileType::Directory;

        std::string ext = filepath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });

        auto it = s_ExtensionToFileType.find(ext);
        if (it != s_ExtensionToFileType.end())
            return it->second;

        return ContentFileType::Unknown;
    }

    const char* GetDragDropPayloadType(ContentFileType type)
    {
        switch (type)
        {
            case ContentFileType::Model3D:
                return "CONTENT_BROWSER_MODEL";
            case ContentFileType::Scene:
                return "CONTENT_BROWSER_SCENE";
            case ContentFileType::Script:
                return "CONTENT_BROWSER_SCRIPT";
            case ContentFileType::Material:
                return "CONTENT_BROWSER_MATERIAL";
            case ContentFileType::Audio:
                return "CONTENT_BROWSER_AUDIO";
            case ContentFileType::StreamingRegion:
                return "CONTENT_BROWSER_REGION";
            case ContentFileType::SaveGame:
                return "CONTENT_BROWSER_SAVEGAME";
            case ContentFileType::ShaderGraph:
                return "CONTENT_BROWSER_SHADERGRAPH";
            default:
                return "CONTENT_BROWSER_ITEM";
        }
    }

    // Tooltip color per file type
    static void DrawFileTypeTooltip(const std::string& filename, ContentFileType type)
    {
        ImGui::BeginTooltip();
        ImGui::Text("%s", filename.c_str());
        switch (type)
        {
            case ContentFileType::Model3D:
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "3D Model");
                break;
            case ContentFileType::Image:
                ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.2f, 1.0f), "Image");
                break;
            case ContentFileType::Scene:
                ImGui::TextColored(ImVec4(0.2f, 0.6f, 0.9f, 1.0f), "Scene");
                break;
            case ContentFileType::Script:
                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "Script");
                break;
            case ContentFileType::Audio:
                ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1.0f), "Audio");
                break;
            case ContentFileType::Material:
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.9f, 1.0f), "Material");
                break;
            case ContentFileType::Shader:
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "Shader");
                break;
            case ContentFileType::StreamingRegion:
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.8f, 1.0f), "Streaming Region");
                break;
            case ContentFileType::Dialogue:
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.6f, 1.0f), "Dialogue Tree");
                break;
            case ContentFileType::ShaderGraph:
                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "Shader Graph");
                break;
            case ContentFileType::SaveGame:
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.4f, 1.0f), "Save Game");
                break;
            default:
                break;
        }
        ImGui::EndTooltip();
    }

    ContentBrowserItem::ContentBrowserItem(const std::filesystem::path& absolutePath, ContentFileType type,
                                           const Ref<Texture2D>& icon)
        : m_Path(absolutePath), m_Type(type), m_Icon(icon)
    {
        // Store display name as UTF-8
        auto u8name = absolutePath.filename().u8string();
        m_DisplayName.assign(u8name.begin(), u8name.end());

        // Pre-fill rename buffer
        std::string name = m_DisplayName;
        size_t len = std::min(name.size(), sizeof(m_RenameBuffer) - 1);
        std::memcpy(m_RenameBuffer, name.c_str(), len);
        m_RenameBuffer[len] = '\0';
    }

    CBActionResult ContentBrowserItem::Render(f32 thumbnailSize, bool isSelected, bool isRenaming)
    {
        CBActionResult result = 0;

        ImGui::PushID(m_Path.string().c_str());
        ImGui::BeginGroup();

        // Selection highlight
        if (isSelected)
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(thumbnailSize + 8.0f, thumbnailSize + ImGui::GetTextLineHeightWithSpacing() + 12.0f);
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(pos.x - 4.0f, pos.y - 4.0f),
                ImVec2(pos.x + size.x - 4.0f, pos.y + size.y - 4.0f),
                IM_COL32(230, 150, 40, 200), 4.0f, 0, 2.0f);
        }

        // Thumbnail button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::ImageButton(m_DisplayName.c_str(),
                           (ImTextureID)(m_Icon->GetRendererID()),
                           { thumbnailSize, thumbnailSize }, { 0, 1 }, { 1, 0 });
        ImGui::PopStyleColor();

        // Drag-drop source
        if (ImGui::BeginDragDropSource())
        {
            auto itemPathU8 = m_Path.u8string();
            const char* payloadType = GetDragDropPayloadType(m_Type);
            ImGui::SetDragDropPayload(payloadType, itemPathU8.c_str(), itemPathU8.size() + 1);
            ImGui::Text("%s", m_DisplayName.c_str());
            ImGui::EndDragDropSource();
        }

        // Context menu
        if (ImGui::BeginPopupContextItem())
        {
            RenderContextMenu(result);
            ImGui::EndPopup();
        }

        // Click handling
        if (ImGui::IsItemHovered())
        {
            SetAction(result, ContentBrowserAction::Hovered);

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                SetAction(result, ContentBrowserAction::Activated);
            }
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                bool ctrl = ImGui::GetIO().KeyCtrl;
                bool shift = ImGui::GetIO().KeyShift;

                if (shift)
                {
                    SetAction(result, ContentBrowserAction::SelectToHere);
                }
                else if (ctrl && isSelected)
                {
                    SetAction(result, ContentBrowserAction::Deselected);
                }
                else if (ctrl)
                {
                    SetAction(result, ContentBrowserAction::Selected);
                }
                else
                {
                    SetAction(result, ContentBrowserAction::ClearSelections);
                    SetAction(result, ContentBrowserAction::Selected);
                }
            }

            DrawFileTypeTooltip(m_DisplayName, m_Type);
        }

        // Label or rename input
        if (isRenaming)
        {
            ImGui::SetNextItemWidth(thumbnailSize);
            if (ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                SetAction(result, ContentBrowserAction::Renamed);
            }

            // Auto-focus the rename field on first frame
            if (m_WantRenameFocus)
            {
                ImGui::SetKeyboardFocusHere(-1);
                m_WantRenameFocus = false;
            }

            // Escape cancels rename
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                // Restore original name
                std::string name = m_DisplayName;
                size_t len = std::min(name.size(), sizeof(m_RenameBuffer) - 1);
                std::memcpy(m_RenameBuffer, name.c_str(), len);
                m_RenameBuffer[len] = '\0';
                SetAction(result, ContentBrowserAction::Renamed); // exit rename mode
            }
        }
        else
        {
            ImGui::TextWrapped("%s", m_DisplayName.c_str());
        }

        ImGui::EndGroup();
        ImGui::PopID();

        return result;
    }

    bool ContentBrowserItem::CommitRename(const std::string& newName)
    {
        if (newName.empty() || newName == m_DisplayName)
            return false;

        // Reject names with path separators, traversal, or directory components
        if (newName.find('/') != std::string::npos || newName.find('\\') != std::string::npos || newName == "." || newName == ".." || std::filesystem::path(newName).filename().string() != newName)
        {
            OLO_CORE_WARN("ContentBrowser: Invalid rename — '{}' contains path components", newName);
            return false;
        }

        std::filesystem::path newPath = m_Path.parent_path() / std::filesystem::path(std::u8string(newName.begin(), newName.end()));

        std::error_code ec;
        if (std::filesystem::exists(newPath, ec) && !std::filesystem::equivalent(m_Path, newPath, ec))
        {
            OLO_CORE_WARN("ContentBrowser: Cannot rename — '{}' already exists", newPath.string());
            return false;
        }

        std::filesystem::rename(m_Path, newPath, ec);
        if (ec)
        {
            OLO_CORE_ERROR("ContentBrowser: Rename failed: {}", ec.message());
            return false;
        }

        OLO_CORE_INFO("ContentBrowser: Renamed '{}' -> '{}'", m_DisplayName, newName);
        m_Path = newPath;
        m_DisplayName = newName;
        m_Type = GetFileTypeFromExtension(m_Path);
        return true;
    }

    void ContentBrowserItem::RenderContextMenu(CBActionResult& result)
    {
        if (ImGui::MenuItem("Open in Explorer"))
        {
            SetAction(result, ContentBrowserAction::ShowInExplorer);
        }

        if (ImGui::MenuItem("Open Externally") && !IsDirectory())
        {
            SetAction(result, ContentBrowserAction::OpenExternal);
        }

        if (ImGui::MenuItem("Copy Path"))
        {
            auto u8path = m_Path.u8string();
            ImGui::SetClipboardText(reinterpret_cast<char const*>(u8path.c_str()));
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Rename"))
        {
            SetAction(result, ContentBrowserAction::StartRenaming);
        }

        if (ImGui::MenuItem("Delete"))
        {
            SetAction(result, ContentBrowserAction::Deleted);
        }
    }

    void SortContentBrowserItems(std::vector<ContentBrowserItem>& items)
    {
        std::sort(items.begin(), items.end(),
                  [](const ContentBrowserItem& a, const ContentBrowserItem& b)
                  {
                      // Directories first
                      if (a.IsDirectory() != b.IsDirectory())
                          return a.IsDirectory();

                      // Then alphabetical (case-insensitive)
                      std::string aName = a.GetDisplayName();
                      std::string bName = b.GetDisplayName();
                      std::transform(aName.begin(), aName.end(), aName.begin(),
                                     [](unsigned char c)
                                     { return static_cast<char>(std::tolower(c)); });
                      std::transform(bName.begin(), bName.end(), bName.begin(),
                                     [](unsigned char c)
                                     { return static_cast<char>(std::tolower(c)); });
                      return aName < bName;
                  });
    }

} // namespace OloEngine
