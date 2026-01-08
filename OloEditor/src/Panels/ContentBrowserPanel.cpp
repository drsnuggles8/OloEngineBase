#include "OloEnginePCH.h"
#include "ContentBrowserPanel.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <imgui.h>
#include <yaml-cpp/yaml.h>
#include <fstream>

#ifdef OLO_PLATFORM_WINDOWS
#include <Windows.h>
#include <shellapi.h>
#endif

namespace OloEngine
{
    // Static helper to map extensions to file types
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
    };

    ContentBrowserPanel::ContentBrowserPanel()
    {
        m_BaseDirectory = Project::GetAssetDirectory();
        m_CurrentDirectory = m_BaseDirectory;

        // Load icons
        m_DirectoryIcon = Texture2D::Create("Resources/Icons/ContentBrowser/DirectoryIcon.png");
        m_FileIcon = Texture2D::Create("Resources/Icons/ContentBrowser/FileIcon.png");

        // Load specialized icons (fallback to file icon if not found)
        m_ModelIcon = Texture2D::Create("Resources/Icons/ContentBrowser/ModelIcon.png");
        if (!m_ModelIcon || !m_ModelIcon->IsLoaded())
            m_ModelIcon = m_FileIcon;

        m_SceneIcon = Texture2D::Create("Resources/Icons/ContentBrowser/SceneIcon.png");
        if (!m_SceneIcon || !m_SceneIcon->IsLoaded())
            m_SceneIcon = m_FileIcon;

        m_ScriptIcon = Texture2D::Create("Resources/Icons/ContentBrowser/ScriptIcon.png");
        if (!m_ScriptIcon || !m_ScriptIcon->IsLoaded())
            m_ScriptIcon = m_FileIcon;

        m_AudioIcon = Texture2D::Create("Resources/Icons/ContentBrowser/AudioIcon.png");
        if (!m_AudioIcon || !m_AudioIcon->IsLoaded())
            m_AudioIcon = m_FileIcon;

        m_MaterialIcon = Texture2D::Create("Resources/Icons/ContentBrowser/MaterialIcon.png");
        if (!m_MaterialIcon || !m_MaterialIcon->IsLoaded())
            m_MaterialIcon = m_FileIcon;

        m_ShaderIcon = Texture2D::Create("Resources/Icons/ContentBrowser/ShaderIcon.png");
        if (!m_ShaderIcon || !m_ShaderIcon->IsLoaded())
            m_ShaderIcon = m_FileIcon;
    }

    ContentFileType ContentBrowserPanel::GetFileType(const std::filesystem::path& filepath) const
    {
        if (std::filesystem::is_directory(filepath))
            return ContentFileType::Directory;

        std::string ext = filepath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto it = s_ExtensionToFileType.find(ext);
        if (it != s_ExtensionToFileType.end())
            return it->second;

        return ContentFileType::Unknown;
    }

    void ContentBrowserPanel::OnImGuiRender()
    {
        ImGui::Begin("Content Browser");

        // Back button
        if ((m_CurrentDirectory != std::filesystem::path(m_BaseDirectory)) && (ImGui::Button("<-")))
        {
            m_CurrentDirectory = m_CurrentDirectory.parent_path();
        }

        ImGui::SameLine();

        // Create menu
        if (ImGui::Button("+ Create"))
        {
            ImGui::OpenPopup("CreateMenu");
        }

        DrawCreateMenu();

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
            const ContentFileType fileType = GetFileType(path);

            ImGui::PushID(filenameString.c_str());
            const Ref<Texture2D> icon = directoryEntry.is_directory() ? m_DirectoryIcon : GetFileIcon(directoryEntry.path());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::ImageButton(filenameString.c_str(), (ImTextureID)(icon->GetRendererID()), { thumbnailSize, thumbnailSize }, { 0, 1 }, { 1, 0 });

            // Drag-drop source for all file types
            if (ImGui::BeginDragDropSource())
            {
                std::filesystem::path relativePath(path);
                wchar_t const* const itemPath = relativePath.c_str();

                // Use different payload types for different file types
                const char* payloadType = "CONTENT_BROWSER_ITEM";
                switch (fileType)
                {
                    case ContentFileType::Model3D:
                        payloadType = "CONTENT_BROWSER_MODEL";
                        break;
                    case ContentFileType::Scene:
                        payloadType = "CONTENT_BROWSER_SCENE";
                        break;
                    case ContentFileType::Script:
                        payloadType = "CONTENT_BROWSER_SCRIPT";
                        break;
                    case ContentFileType::Material:
                        payloadType = "CONTENT_BROWSER_MATERIAL";
                        break;
                    case ContentFileType::Audio:
                        payloadType = "CONTENT_BROWSER_AUDIO";
                        break;
                    default:
                        break;
                }

                ImGui::SetDragDropPayload(payloadType, itemPath, (std::wcslen(itemPath) + 1) * sizeof(wchar_t));

                // Show preview tooltip
                ImGui::Text("%s", filenameString.c_str());
                ImGui::EndDragDropSource();
            }

            ImGui::PopStyleColor();

            // Context menu for right-click
            if (ImGui::BeginPopupContextItem())
            {
                DrawFileContextMenu(path, fileType);
                ImGui::EndPopup();
            }

            // Double-click handling
            if (ImGui::IsItemHovered())
            {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (directoryEntry.is_directory())
                    {
                        m_CurrentDirectory /= path.filename();
                    }
                    else if (m_AssetSelectedCallback)
                    {
                        m_AssetSelectedCallback(path, fileType);
                    }
                }

                // Show tooltip with file info
                ImGui::BeginTooltip();
                ImGui::Text("%s", filenameString.c_str());
                switch (fileType)
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
                    default:
                        break;
                }
                ImGui::EndTooltip();
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
        int modelCount = 0;
        int sceneCount = 0;
        for (auto& entry : std::filesystem::directory_iterator(m_CurrentDirectory))
        {
            totalCount++;
            ContentFileType type = GetFileType(entry.path());
            if (type == ContentFileType::Model3D)
                modelCount++;
            if (type == ContentFileType::Scene)
                sceneCount++;
        }

        ImGui::Separator(); // Draw a line to separate the status bar from the rest of the content.

        // Change the background color for the status bar.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.25f, 0.29f, 0.5f));

        // Create a child window for the status bar.
        ImGui::BeginChild("status_bar", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 20), true, 0);

        // Change the text color for the status bar.
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));

        // Add status bar information.
        ImGui::Text("Total: %d", totalCount);
        if (modelCount > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "| Models: %d", modelCount);
        }
        if (sceneCount > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.6f, 0.9f, 1.0f), "| Scenes: %d", sceneCount);
        }
        ImGui::SameLine();
        ImGui::Text("| %s", m_CurrentDirectory.string().c_str());

        // Restore the colors we changed.
        ImGui::PopStyleColor(2);

        ImGui::EndChild(); // End of child window.
        ImGui::End();
    }

    void ContentBrowserPanel::DrawCreateMenu()
    {
        if (ImGui::BeginPopup("CreateMenu"))
        {
            if (ImGui::BeginMenu("Create Folder"))
            {
                static char folderName[256] = "New Folder";
                ImGui::InputText("Name", folderName, sizeof(folderName));
                if (ImGui::Button("Create##Folder"))
                {
                    std::filesystem::path newFolder = m_CurrentDirectory / folderName;
                    std::filesystem::create_directories(newFolder);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("3D Primitive"))
            {
                if (ImGui::MenuItem("Cube"))
                    CreateMeshPrimitiveFile("Cube");
                if (ImGui::MenuItem("Sphere"))
                    CreateMeshPrimitiveFile("Sphere");
                if (ImGui::MenuItem("Plane"))
                    CreateMeshPrimitiveFile("Plane");
                if (ImGui::MenuItem("Cylinder"))
                    CreateMeshPrimitiveFile("Cylinder");
                if (ImGui::MenuItem("Cone"))
                    CreateMeshPrimitiveFile("Cone");
                if (ImGui::MenuItem("Icosphere"))
                    CreateMeshPrimitiveFile("Icosphere");
                if (ImGui::MenuItem("Torus"))
                    CreateMeshPrimitiveFile("Torus");
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Material"))
            {
                static char matName[256] = "NewMaterial";
                ImGui::InputText("Name", matName, sizeof(matName));
                if (ImGui::Button("Create##Material"))
                {
                    // Create a basic material file
                    std::filesystem::path matPath = m_CurrentDirectory / (std::string(matName) + ".material");
                    YAML::Emitter out;
                    out << YAML::BeginMap;
                    out << YAML::Key << "Material" << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "Name" << YAML::Value << matName;
                    out << YAML::Key << "BaseColor" << YAML::Value << glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                    out << YAML::Key << "Metallic" << YAML::Value << 0.0f;
                    out << YAML::Key << "Roughness" << YAML::Value << 0.5f;
                    out << YAML::EndMap;
                    out << YAML::EndMap;

                    std::ofstream fout(matPath);
                    if (!fout)
                    {
                        OLO_CORE_ERROR("Failed to create material file: {}", matPath.string());
                        ImGui::CloseCurrentPopup();
                        ImGui::EndMenu();
                        return;
                    }
                    fout << out.c_str();
                    fout.close();

                    OLO_CORE_INFO("Created material: {}", matPath.string());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }

            ImGui::EndPopup();
        }
    }

    void ContentBrowserPanel::DrawFileContextMenu(const std::filesystem::path& path, ContentFileType fileType)
    {
        if (ImGui::MenuItem("Open in Explorer"))
        {
#ifdef OLO_PLATFORM_WINDOWS
            // Use ShellExecuteW to avoid command injection vulnerability
            std::wstring args = L"/select,\"" + path.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#endif
        }

        if (ImGui::MenuItem("Copy Path"))
        {
            ImGui::SetClipboardText(path.string().c_str());
        }

        ImGui::Separator();

        switch (fileType)
        {
            case ContentFileType::Model3D:
                if (ImGui::MenuItem("Import to Scene"))
                {
                    // Trigger callback if set
                    if (m_AssetSelectedCallback)
                        m_AssetSelectedCallback(path, fileType);
                }
                break;
            case ContentFileType::Scene:
                if (ImGui::MenuItem("Open Scene"))
                {
                    if (m_AssetSelectedCallback)
                        m_AssetSelectedCallback(path, fileType);
                }
                break;
            default:
                break;
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Delete", nullptr, false, !std::filesystem::is_directory(path)))
        {
            try
            {
                std::filesystem::remove(path);
                OLO_CORE_INFO("Deleted: {}", path.string());
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                OLO_CORE_ERROR("Failed to delete {}: {}", path.string(), e.what());
            }
        }
    }

    void ContentBrowserPanel::CreateMeshPrimitiveFile(const std::string& primitiveType)
    {
        // Generate unique filename
        std::string baseName = primitiveType;
        std::filesystem::path filePath = m_CurrentDirectory / (baseName + ".primitive");
        int counter = 1;
        while (std::filesystem::exists(filePath))
        {
            filePath = m_CurrentDirectory / (baseName + "_" + std::to_string(counter++) + ".primitive");
        }

        // Create primitive definition file
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Primitive" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Type" << YAML::Value << primitiveType;

        // Add default parameters based on type
        if (primitiveType == "Sphere" || primitiveType == "Icosphere")
        {
            out << YAML::Key << "Radius" << YAML::Value << 1.0f;
            if (primitiveType == "Sphere")
                out << YAML::Key << "Segments" << YAML::Value << 16;
            else
                out << YAML::Key << "Subdivisions" << YAML::Value << 2;
        }
        else if (primitiveType == "Plane")
        {
            out << YAML::Key << "Width" << YAML::Value << 1.0f;
            out << YAML::Key << "Length" << YAML::Value << 1.0f;
        }
        else if (primitiveType == "Cylinder" || primitiveType == "Cone")
        {
            out << YAML::Key << "Radius" << YAML::Value << 1.0f;
            out << YAML::Key << "Height" << YAML::Value << 2.0f;
            out << YAML::Key << "Segments" << YAML::Value << 16;
        }
        else if (primitiveType == "Torus")
        {
            out << YAML::Key << "MajorRadius" << YAML::Value << 1.0f;
            out << YAML::Key << "MinorRadius" << YAML::Value << 0.3f;
            out << YAML::Key << "MajorSegments" << YAML::Value << 24;
            out << YAML::Key << "MinorSegments" << YAML::Value << 12;
        }

        out << YAML::EndMap;
        out << YAML::EndMap;

        std::ofstream fout(filePath);
        if (!fout)
        {
            OLO_CORE_ERROR("Failed to create primitive file: {}", filePath.string());
            return;
        }
        fout << out.c_str();
        fout.close();

        OLO_CORE_INFO("Created primitive: {}", filePath.string());
    }

    Ref<Texture2D>& ContentBrowserPanel::GetFileIcon(const std::filesystem::path& filepath)
    {
        // First check if we have a cached thumbnail
        if (m_ImageIcons.contains(filepath))
        {
            return m_ImageIcons[filepath];
        }

        ContentFileType fileType = GetFileType(filepath);

        // Return type-specific icons
        switch (fileType)
        {
            case ContentFileType::Image:
            {
                // Try to load image as thumbnail
                auto imageIcon = Texture2D::Create(filepath.string());
                if (imageIcon->IsLoaded())
                {
                    auto& icon = m_ImageIcons[filepath] = imageIcon;
                    return icon;
                }
                return m_FileIcon;
            }
            case ContentFileType::Model3D:
                return m_ModelIcon;
            case ContentFileType::Scene:
                return m_SceneIcon;
            case ContentFileType::Script:
                return m_ScriptIcon;
            case ContentFileType::Audio:
                return m_AudioIcon;
            case ContentFileType::Material:
                return m_MaterialIcon;
            case ContentFileType::Shader:
                return m_ShaderIcon;
            default:
                return m_FileIcon;
        }
    }

} // namespace OloEngine
