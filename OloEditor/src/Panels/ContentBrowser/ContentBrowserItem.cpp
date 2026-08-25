#include "OloEnginePCH.h"
#include "ContentBrowserItem.h"

#include "OloEngine/ImGui/ImGuiLayer.h"

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

        // pl_mpeg path — always available.
        { ".mpg", ContentFileType::Video },
        { ".mpeg", ContentFileType::Video },
        { ".m1v", ContentFileType::Video },
#if defined(OLO_VIDEO_FFMPEG)
        // FFmpeg-backed containers — only advertised when the build can actually decode them.
        { ".mp4", ContentFileType::Video },
        { ".mov", ContentFileType::Video },
        { ".m4v", ContentFileType::Video },
        { ".mkv", ContentFileType::Video },
        { ".webm", ContentFileType::Video },
        { ".avi", ContentFileType::Video },
#endif
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
        // Sound Graphs
        { ".olosoundgraph", ContentFileType::SoundGraph },
        // Visual Scripts (issue #634)
        { ".olovs", ContentFileType::VisualScript },
        // Cinematic Sequences
        { ".olocine", ContentFileType::Cinematic },
        // Fluid Settings
        { ".olofluid", ContentFileType::FluidSettings },
        // Save Games
        { ".olosave", ContentFileType::SaveGame },
        // RPG progression data (issue #635)
        { ".oloskilltree", ContentFileType::SkillTree },
        { ".olocharclass", ContentFileType::CharacterClass },
        { ".oloxpcurve", ContentFileType::ExperienceCurve },
        // Volumetric density grids (issue #724): the cooked native format is
        // always browsable; the OpenVDB source extension is only offered
        // when the editor can actually cook it (OLO_WITH_OPENVDB).
        { ".olovol", ContentFileType::Volume },
#if defined(OLO_WITH_OPENVDB)
        { ".vdb", ContentFileType::Volume },
#endif
    };

    ContentFileType GetFileTypeFromExtension(const std::filesystem::path& filepath)
    {
        if (std::error_code ec; std::filesystem::is_directory(filepath, ec))
            return ContentFileType::Directory;

        std::string ext = filepath.extension().string();
        std::ranges::transform(ext, ext.begin(),
                               [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });

        if (auto it = s_ExtensionToFileType.find(ext); it != s_ExtensionToFileType.end())
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
            case ContentFileType::SoundGraph:
                return "CONTENT_BROWSER_SOUNDGRAPH";
            // Deliberately the GENERIC payload: SceneHierarchyPanel's
            // VisualScriptComponent drop target accepts CONTENT_BROWSER_ITEM (as
            // every other asset-assign target in that panel does), so a bespoke
            // payload name would make .olovs the one asset type that cannot be
            // dragged onto its own component.
            case ContentFileType::VisualScript:
                return "CONTENT_BROWSER_ITEM";
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
            case ContentFileType::Video:
                ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.5f, 1.0f), "Video");
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
            case ContentFileType::VisualScript:
                ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "Visual Script");
                break;
            case ContentFileType::SaveGame:
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.4f, 1.0f), "Save Game");
                break;
            case ContentFileType::Cinematic:
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "Cinematic Sequence");
                break;
            case ContentFileType::FluidSettings:
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.95f, 1.0f), "Fluid Settings");
                break;
            case ContentFileType::SkillTree:
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), "Skill Tree");
                break;
            case ContentFileType::CharacterClass:
                ImGui::TextColored(ImVec4(0.7f, 0.5f, 0.9f, 1.0f), "Character Class Database");
                break;
            case ContentFileType::ExperienceCurve:
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.7f, 1.0f), "Experience Curve");
                break;
            default:
                break;
        }
        ImGui::EndTooltip();
    }

    ContentBrowserItem::ContentBrowserItem(const std::filesystem::path& absolutePath, ContentFileType type,
                                           const Ref<Texture2D>& icon, bool iconIsRenderTarget)
        : m_Path(absolutePath), m_Type(type), m_Icon(icon), m_IconIsRenderTarget(iconIsRenderTarget)
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

        // Thumbnail button. Backend-neutral ImTextureID (#691): 0
        // means no binding exists on this backend — fall back to a plain
        // button rather than hand imgui_impl_vulkan a null descriptor set.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (const u64 thumbId = ImGuiLayer::GetTextureID(*m_Icon); thumbId != 0)
        {
            // Loaded icons are uploaded pre-flipped (fixed GL-convention uv);
            // a RENDERED material/mesh preview follows the backend's render-
            // target row order (ADR 0011 amendment (85)) — under Vulkan it is
            // top-down and the V flip must not apply, which is exactly the
            // case the old hard-coded pair got wrong.
            const bool flipV = !m_IconIsRenderTarget || ImGuiLayer::RenderTargetRowsAreBottomUp();
            const ImVec2 uv0 = flipV ? ImVec2{ 0, 1 } : ImVec2{ 0, 0 };
            const ImVec2 uv1 = flipV ? ImVec2{ 1, 0 } : ImVec2{ 1, 1 };
            ImGui::ImageButton(m_DisplayName.c_str(), static_cast<ImTextureID>(thumbId),
                               { thumbnailSize, thumbnailSize }, uv0, uv1);
        }
        else
        {
            ImGui::Button(m_DisplayName.c_str(), { thumbnailSize, thumbnailSize });
        }
        ImGui::PopStyleColor();

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

        // Interaction checks apply to the full group (thumbnail + label).
        // The last submitted item may be a Text widget (ID=0), so allow a
        // fallback temporary ID to avoid ImGui assert in BeginDragDropSource().
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            auto itemPathU8 = m_Path.u8string();
            const char* payloadType = GetDragDropPayloadType(m_Type);
            ImGui::SetDragDropPayload(payloadType, itemPathU8.c_str(), itemPathU8.size() + 1);
            ImGui::Text("%s", m_DisplayName.c_str());
            ImGui::EndDragDropSource();
        }

        // Context menu
        if (ImGui::BeginPopupContextItem("##CBItemCtx"))
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
            else
            {
                // No additional handling required.
            }

            DrawFileTypeTooltip(m_DisplayName, m_Type);
        }

        ImGui::PopID();

        return result;
    }

    bool ContentBrowserItem::CommitRename(const std::string& newName)
    {
        if (newName.empty() || newName == m_DisplayName)
            return false;

        // Convert newName (UTF-8 from ImGui) once for both validation and path construction
        auto requestedU8 = std::u8string(newName.begin(), newName.end());
        std::filesystem::path requestedPath(requestedU8);

        // Reject names with path separators, traversal, or directory components
        if (newName.find('/') != std::string::npos || newName.find('\\') != std::string::npos || newName == "." || newName == ".." || requestedPath.filename().u8string() != requestedU8)
        {
            OLO_CORE_WARN("ContentBrowser: Invalid rename — '{}' contains path components", newName);
            return false;
        }

        std::filesystem::path newPath = m_Path.parent_path() / requestedPath;

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

    void ContentBrowserItem::RenderContextMenu(CBActionResult& result) const
    {
        if (ImGui::MenuItem("Open in Explorer"))
        {
            SetAction(result, ContentBrowserAction::ShowInExplorer);
        }

        if (ImGui::MenuItem("Open Externally", nullptr, false, !IsDirectory()))
        {
            SetAction(result, ContentBrowserAction::OpenExternal);
        }

        if (ImGui::MenuItem("Copy Path"))
        {
            auto u8path = m_Path.u8string();
            ImGui::SetClipboardText(reinterpret_cast<char const*>(u8path.c_str()));
        }

        // Reimport option for 3D model files
        if (m_Type == ContentFileType::Model3D)
        {
            if (ImGui::MenuItem("Reimport"))
            {
                SetAction(result, ContentBrowserAction::Reimport);
            }
        }

#if defined(OLO_WITH_OPENVDB)
        // OpenVDB is editor/cook-only (never linked into OloEngine — see the
        // OLO_WITH_OPENVDB option comment in the root CMakeLists.txt), so a
        // .vdb needs an explicit cook step rather than the generic
        // AssetManager import path every other source format uses.
        if (m_Type == ContentFileType::Volume && m_Path.extension() == ".vdb")
        {
            if (ImGui::MenuItem("Import as Volume"))
            {
                SetAction(result, ContentBrowserAction::ImportVolume);
            }
        }
#endif

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
        std::ranges::sort(items,
                          [](const ContentBrowserItem& a, const ContentBrowserItem& b)
                          {
                              // Directories first
                              if (a.IsDirectory() != b.IsDirectory())
                                  return a.IsDirectory();

                              // Then alphabetical (case-insensitive)
                              std::string aName = a.GetDisplayName();
                              std::string bName = b.GetDisplayName();
                              std::ranges::transform(aName, aName.begin(),
                                                     [](unsigned char c)
                                                     { return static_cast<char>(std::tolower(c)); });
                              std::ranges::transform(bName, bName.begin(),
                                                     [](unsigned char c)
                                                     { return static_cast<char>(std::tolower(c)); });
                              return aName < bName;
                          });
    }

} // namespace OloEngine
