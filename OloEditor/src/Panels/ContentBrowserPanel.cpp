#include "OloEnginePCH.h"
#include "ContentBrowserPanel.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Core/YAMLConverters.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Dialogue/DialogueTreeSerializer.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphSerializer.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <yaml-cpp/yaml.h>
#include <fstream>

#ifdef OLO_PLATFORM_WINDOWS
#include <Windows.h>
#include <shellapi.h>
#endif

namespace OloEngine
{
    ContentBrowserPanel::ContentBrowserPanel()
    {
        auto assetDir = Project::GetAssetDirectory();

        // Build cached directory tree
        m_DirectoryTree.Build(assetDir);
        m_CurrentDirectory = m_DirectoryTree.GetRoot();

        // Load icons
        m_DirectoryIcon = Texture2D::Create("Resources/Icons/ContentBrowser/DirectoryIcon.png");
        m_FileIcon = Texture2D::Create("Resources/Icons/ContentBrowser/FileIcon.png");

        // Load specialized icons (fallback to file icon if not found)
        auto loadIcon = [this](const char* path) -> Ref<Texture2D>
        {
            auto tex = Texture2D::Create(path);
            if (!tex || !tex->IsLoaded())
                return m_FileIcon;
            return tex;
        };

        m_ModelIcon = loadIcon("Resources/Icons/ContentBrowser/ModelIcon.png");
        m_SceneIcon = loadIcon("Resources/Icons/ContentBrowser/SceneIcon.png");
        m_ScriptIcon = loadIcon("Resources/Icons/ContentBrowser/ScriptIcon.png");
        m_AudioIcon = loadIcon("Resources/Icons/ContentBrowser/AudioIcon.png");
        m_MaterialIcon = loadIcon("Resources/Icons/ContentBrowser/MaterialIcon.png");
        m_ShaderIcon = loadIcon("Resources/Icons/ContentBrowser/ShaderIcon.png");
        m_DialogueIcon = loadIcon("Resources/Icons/ContentBrowser/DialogueIcon.png");
        m_SaveGameIcon = loadIcon("Resources/Icons/ContentBrowser/SaveGameIcon.png");

        RebuildItemList();
        RebuildBreadcrumbs();

        RegisterSettingsHandler();
    }

    ContentBrowserPanel::~ContentBrowserPanel()
    {
        if (s_Instance == this)
            s_Instance = nullptr;
    }

    // =========================================================================
    // Main Render
    // =========================================================================

    void ContentBrowserPanel::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        ImGui::Begin("Content Browser");

        RefreshIfDirty();
        HandleKeyboardShortcuts();

        // Dual-pane layout: directory tree (left) + content grid (right)
        if (ImGui::BeginTable("ContentBrowserLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("DirectoryTree", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("ContentGrid", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();

            // Left pane: directory tree
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("DirTree", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.0f));
            DrawDirectoryTree();
            ImGui::EndChild();

            // Right pane: top bar + grid
            ImGui::TableSetColumnIndex(1);
            DrawTopBar();
            ImGui::Separator();
            ImGui::BeginChild("ItemGrid", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.0f));
            DrawItemGrid();
            ImGui::EndChild();

            ImGui::EndTable();
        }

        DrawBottomBar();

        // Delete confirmation modal
        if (m_ShowDeleteConfirmation)
        {
            ImGui::OpenPopup("Delete Items?");
            m_ShowDeleteConfirmation = false;
        }

        if (ImGui::BeginPopupModal("Delete Items?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            // Count directories in selection
            size_t dirCount = 0;
            for (auto& path : m_SelectedItems)
            {
                std::error_code ec;
                if (std::filesystem::is_directory(path, ec))
                    ++dirCount;
            }

            if (dirCount > 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "WARNING: %zu director%s will be RECURSIVELY deleted!",
                                   dirCount, dirCount == 1 ? "y" : "ies");
                ImGui::Spacing();
            }

            ImGui::Text("Delete %zu item(s)?", m_SelectedItems.size());

            // Show first few filenames
            size_t shown = std::min(m_SelectedItems.size(), static_cast<size_t>(5));
            for (size_t i = 0; i < shown; ++i)
                ImGui::BulletText("%s", m_SelectedItems[i].filename().string().c_str());
            if (m_SelectedItems.size() > 5)
                ImGui::Text("... and %zu more", m_SelectedItems.size() - 5);

            ImGui::Spacing();

            if (ImGui::Button("Yes", ImVec2(120, 0)))
            {
                DeleteSelectedItems();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();
    }

    // =========================================================================
    // Top Bar: back/forward/refresh + search + breadcrumbs
    // =========================================================================

    void ContentBrowserPanel::DrawTopBar()
    {
        bool canGoBack = m_CurrentDirectory && m_CurrentDirectory->Parent != nullptr;
        bool canGoForward = m_ForwardDirectory != nullptr;

        // Back button
        ImGui::BeginDisabled(!canGoBack);
        if (ImGui::Button("<"))
            NavigateBack();
        ImGui::EndDisabled();
        ImGui::SameLine();

        // Forward button
        ImGui::BeginDisabled(!canGoForward);
        if (ImGui::Button(">"))
            NavigateForward();
        ImGui::EndDisabled();
        ImGui::SameLine();

        // Refresh button
        if (ImGui::Button("R"))
        {
            m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
            RebuildItemList();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Refresh (F5)");
        ImGui::SameLine();

        // Create button
        if (ImGui::Button("+ Create"))
            ImGui::OpenPopup("CreateMenu");
        DrawCreateMenu();
        ImGui::SameLine();

        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Search box
        ImGui::SetNextItemWidth(200.0f);
        if (m_FocusSearchNextFrame)
        {
            ImGui::SetKeyboardFocusHere();
            m_FocusSearchNextFrame = false;
        }

        bool searchChanged = ImGui::InputTextWithHint("##Search", "Search...", m_SearchBuffer, sizeof(m_SearchBuffer));

        ImGui::SameLine();
        if (m_SearchBuffer[0] != '\0')
        {
            if (ImGui::Button("X##ClearSearch"))
            {
                ClearSearch();
                searchChanged = false; // Already handled
            }
        }

        if (searchChanged)
        {
            if (m_SearchBuffer[0] != '\0')
            {
                m_IsSearchActive = true;
                UpdateSearchResults();
            }
            else
            {
                ClearSearch();
            }
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Breadcrumbs
        if (m_IsSearchActive)
        {
            ImGui::TextDisabled("Search results for \"%s\"", m_SearchBuffer);
        }
        else
        {
            for (size_t i = 0; i < m_BreadcrumbTrail.size(); ++i)
            {
                if (i > 0)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("/");
                    ImGui::SameLine();
                }

                DirectoryInfo* crumb = m_BreadcrumbTrail[i];
                const std::string& name = crumb->Name;

                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton(name.c_str()))
                {
                    ChangeDirectory(crumb);
                }
                ImGui::PopID();
            }
        }
    }

    // =========================================================================
    // Directory Tree (left pane)
    // =========================================================================

    void ContentBrowserPanel::DrawDirectoryTree()
    {
        if (auto* root = m_DirectoryTree.GetRoot())
        {
            DrawDirectoryTreeNode(root);
        }
    }

    void ContentBrowserPanel::DrawDirectoryTreeNode(DirectoryInfo* dir)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

        if (dir->SubDirectories.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;

        // Highlight current directory
        if (dir == m_CurrentDirectory)
            flags |= ImGuiTreeNodeFlags_Selected;

        // Root is open by default
        if (dir->Parent == nullptr)
            flags |= ImGuiTreeNodeFlags_DefaultOpen;

        bool isOpen = ImGui::TreeNodeEx(dir->Name.c_str(), flags);

        // Click to navigate
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            ChangeDirectory(dir);
        }

        if (isOpen)
        {
            for (auto& child : dir->SubDirectories)
            {
                DrawDirectoryTreeNode(child.get());
            }
            ImGui::TreePop();
        }
    }

    // =========================================================================
    // Item Grid (right pane)
    // =========================================================================

    void ContentBrowserPanel::DrawItemGrid()
    {
        const f32 cellSize = m_ThumbnailSize + m_Padding;
        const f32 panelWidth = ImGui::GetContentRegionAvail().x;
        auto columnCount = static_cast<int>(panelWidth / cellSize);
        columnCount = std::max(columnCount, 1);

        ImGui::Columns(columnCount, nullptr, false);

        for (size_t i = 0; i < m_Items.size(); ++i)
        {
            auto& item = m_Items[i];
            bool selected = IsSelected(item.GetPath());
            bool renaming = (m_RenamingItem == item.GetPath());

            CBActionResult actions = item.Render(m_ThumbnailSize, selected, renaming);

            // Process actions
            if (HasAction(actions, ContentBrowserAction::ClearSelections))
                DeselectAll();

            if (HasAction(actions, ContentBrowserAction::Selected))
            {
                Select(item.GetPath());
                m_LastSelectedIndex = i;
            }

            if (HasAction(actions, ContentBrowserAction::Deselected))
                Deselect(item.GetPath());

            if (HasAction(actions, ContentBrowserAction::SelectToHere))
            {
                DeselectAll();
                SelectRange(m_LastSelectedIndex, i);
            }

            if (HasAction(actions, ContentBrowserAction::Activated))
            {
                if (item.IsDirectory())
                {
                    // Find the DirectoryInfo for this path
                    auto relativePath = std::filesystem::relative(item.GetPath(), m_DirectoryTree.GetAssetRoot());
                    if (auto* dirInfo = m_DirectoryTree.FindDirectory(relativePath))
                    {
                        ChangeDirectory(dirInfo);
                    }
                }
                else if (m_AssetSelectedCallback)
                {
                    m_AssetSelectedCallback(item.GetPath(), item.GetType());
                }
            }

            if (HasAction(actions, ContentBrowserAction::StartRenaming))
            {
                m_RenamingItem = item.GetPath();
                m_Items[i].SetWantRenameFocus();
            }

            if (HasAction(actions, ContentBrowserAction::Renamed))
            {
                if (renaming)
                {
                    std::string newName(item.GetRenameBuffer());
                    if (newName != item.GetDisplayName())
                    {
                        auto& mutableItem = m_Items[i];
                        mutableItem.CommitRename(newName);
                        m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
                        m_RenamingItem.clear();
                        RebuildItemList();
                        break; // Items changed, exit loop
                    }
                    m_RenamingItem.clear();
                }
            }

            if (HasAction(actions, ContentBrowserAction::Deleted))
            {
                if (!IsSelected(item.GetPath()))
                {
                    DeselectAll();
                    Select(item.GetPath());
                }
                m_ShowDeleteConfirmation = true;
            }

            if (HasAction(actions, ContentBrowserAction::ShowInExplorer))
                OpenInExplorer(item.GetPath());

            if (HasAction(actions, ContentBrowserAction::OpenExternal))
                OpenExternally(item.GetPath());

            ImGui::NextColumn();
        }

        ImGui::Columns(1);

        // Click on empty space deselects
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        {
            DeselectAll();
        }
    }

    // =========================================================================
    // Bottom Bar: status + thumbnail slider
    // =========================================================================

    void ContentBrowserPanel::DrawBottomBar()
    {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.25f, 0.29f, 0.5f));
        ImGui::BeginChild("StatusBar", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 20), true);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));

        // Item count
        size_t dirCount = 0;
        size_t fileCount = 0;
        for (const auto& item : m_Items)
        {
            if (item.IsDirectory())
                dirCount++;
            else
                fileCount++;
        }

        if (!m_SelectedItems.empty())
        {
            ImGui::Text("%zu selected", m_SelectedItems.size());
        }
        else
        {
            ImGui::Text("%zu folders, %zu files", dirCount, fileCount);
        }

        ImGui::SameLine();

        // Current path
        if (m_CurrentDirectory)
        {
            ImGui::TextDisabled("| %s", m_CurrentDirectory->RelativePath.empty()
                                            ? m_DirectoryTree.GetAssetRoot().string().c_str()
                                            : (m_DirectoryTree.GetAssetRoot() / m_CurrentDirectory->RelativePath).string().c_str());
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 350.0f);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("Size", &m_ThumbnailSize, 48.0f, 256.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderFloat("Pad", &m_Padding, 0.0f, 32.0f, "%.0f");

        ImGui::PopStyleColor(2);
        ImGui::EndChild();
    }

    // =========================================================================
    // Navigation
    // =========================================================================

    void ContentBrowserPanel::ChangeDirectory(DirectoryInfo* dir)
    {
        if (!dir || dir == m_CurrentDirectory)
            return;

        m_ForwardDirectory = nullptr; // Clear forward on new navigation
        m_CurrentDirectory = dir;
        m_RenamingItem.clear();
        DeselectAll();
        ClearSearch();
        RebuildItemList();
        RebuildBreadcrumbs();
    }

    void ContentBrowserPanel::NavigateBack()
    {
        if (m_CurrentDirectory && m_CurrentDirectory->Parent)
        {
            m_ForwardDirectory = m_CurrentDirectory;
            m_CurrentDirectory = m_CurrentDirectory->Parent;
            m_RenamingItem.clear();
            DeselectAll();
            RebuildItemList();
            RebuildBreadcrumbs();
        }
    }

    void ContentBrowserPanel::NavigateForward()
    {
        if (m_ForwardDirectory)
        {
            m_CurrentDirectory = m_ForwardDirectory;
            m_ForwardDirectory = nullptr;
            m_RenamingItem.clear();
            DeselectAll();
            RebuildItemList();
            RebuildBreadcrumbs();
        }
    }

    // =========================================================================
    // Data Management
    // =========================================================================

    void ContentBrowserPanel::RebuildItemList()
    {
        OLO_PROFILE_FUNCTION();
        m_Items.clear();

        if (!m_CurrentDirectory)
            return;

        // Add subdirectories
        for (auto& subDir : m_CurrentDirectory->SubDirectories)
        {
            std::filesystem::path absPath = m_DirectoryTree.GetAssetRoot() / subDir->RelativePath;
            m_Items.emplace_back(absPath, ContentFileType::Directory, m_DirectoryIcon);
        }

        // Add files
        for (auto& file : m_CurrentDirectory->Files)
        {
            ContentFileType type = GetFileTypeFromExtension(file);
            Ref<Texture2D>& icon = GetFileIcon(file);
            m_Items.emplace_back(file, type, icon);
        }

        SortContentBrowserItems(m_Items);
    }

    void ContentBrowserPanel::RebuildBreadcrumbs()
    {
        m_BreadcrumbTrail.clear();

        for (auto* d = m_CurrentDirectory; d != nullptr; d = d->Parent)
            m_BreadcrumbTrail.push_back(d);

        std::reverse(m_BreadcrumbTrail.begin(), m_BreadcrumbTrail.end());
    }

    void ContentBrowserPanel::RefreshIfDirty()
    {
        if (m_CurrentDirectory && m_CurrentDirectory->NeedsRefresh)
        {
            m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
            RebuildItemList();
        }
    }

    // =========================================================================
    // Search
    // =========================================================================

    void ContentBrowserPanel::UpdateSearchResults()
    {
        OLO_PROFILE_FUNCTION();
        m_Items.clear();

        auto results = m_DirectoryTree.Search(m_SearchBuffer);

        for (auto& path : results)
        {
            ContentFileType type = GetFileTypeFromExtension(path);
            if (type == ContentFileType::Directory)
            {
                m_Items.emplace_back(path, type, m_DirectoryIcon);
            }
            else
            {
                Ref<Texture2D>& icon = GetFileIcon(path);
                m_Items.emplace_back(path, type, icon);
            }
        }

        SortContentBrowserItems(m_Items);
    }

    void ContentBrowserPanel::ClearSearch()
    {
        m_SearchBuffer[0] = '\0';
        m_IsSearchActive = false;
        RebuildItemList();
    }

    // =========================================================================
    // Selection
    // =========================================================================

    bool ContentBrowserPanel::IsSelected(const std::filesystem::path& path) const
    {
        return std::find(m_SelectedItems.begin(), m_SelectedItems.end(), path) != m_SelectedItems.end();
    }

    void ContentBrowserPanel::Select(const std::filesystem::path& path)
    {
        if (!IsSelected(path))
            m_SelectedItems.push_back(path);
    }

    void ContentBrowserPanel::Deselect(const std::filesystem::path& path)
    {
        auto it = std::find(m_SelectedItems.begin(), m_SelectedItems.end(), path);
        if (it != m_SelectedItems.end())
            m_SelectedItems.erase(it);
    }

    void ContentBrowserPanel::DeselectAll()
    {
        m_SelectedItems.clear();
    }

    void ContentBrowserPanel::SelectRange(size_t fromIndex, size_t toIndex)
    {
        if (m_Items.empty())
            return;

        if (fromIndex > toIndex)
            std::swap(fromIndex, toIndex);

        fromIndex = std::min(fromIndex, m_Items.size() - 1);
        toIndex = std::min(toIndex, m_Items.size() - 1);

        for (size_t i = fromIndex; i <= toIndex; ++i)
            Select(m_Items[i].GetPath());
    }

    // =========================================================================
    // Item Operations
    // =========================================================================

    void ContentBrowserPanel::DeleteSelectedItems()
    {
        for (auto& path : m_SelectedItems)
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            if (ec)
            {
                OLO_CORE_ERROR("Failed to delete '{}': {}", path.string(), ec.message());
            }
            else
            {
                OLO_CORE_INFO("Deleted: {}", path.string());
            }
        }

        DeselectAll();
        m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
        RebuildItemList();
    }

    void ContentBrowserPanel::HandleKeyboardShortcuts()
    {
        // Only handle shortcuts when the Content Browser window is focused
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
            return;

        auto& io = ImGui::GetIO();

        // Ctrl+F: focus search
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))
        {
            m_FocusSearchNextFrame = true;
        }

        // F5: refresh
        if (ImGui::IsKeyPressed(ImGuiKey_F5))
        {
            m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
            RebuildItemList();
        }

        // F2: rename selected (single)
        if (ImGui::IsKeyPressed(ImGuiKey_F2) && m_SelectedItems.size() == 1)
        {
            m_RenamingItem = m_SelectedItems[0];
            for (auto& item : m_Items)
            {
                if (item.GetPath() == m_RenamingItem)
                {
                    item.SetWantRenameFocus();
                    break;
                }
            }
        }

        // Delete: delete selected
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_SelectedItems.empty())
        {
            m_ShowDeleteConfirmation = true;
        }

        // Ctrl+A: select all
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
        {
            DeselectAll();
            for (auto& item : m_Items)
                Select(item.GetPath());
        }

        // Escape: deselect / clear search
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (m_IsSearchActive)
                ClearSearch();
            else if (!m_RenamingItem.empty())
                m_RenamingItem.clear();
            else
                DeselectAll();
        }

        // Alt+Left: back
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            NavigateBack();

        // Alt+Right: forward
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            NavigateForward();
    }

    // =========================================================================
    // OS Integration
    // =========================================================================

    void ContentBrowserPanel::OpenInExplorer([[maybe_unused]] const std::filesystem::path& path)
    {
#ifdef OLO_PLATFORM_WINDOWS
        std::error_code ecCanonical;
        auto canonical = std::filesystem::canonical(path, ecCanonical);
        if (ecCanonical)
        {
            OLO_CORE_ERROR("OpenInExplorer: canonical failed for '{}': {}", path.string(), ecCanonical.message());
            return;
        }
        std::error_code ecExists;
        if (!std::filesystem::exists(canonical, ecExists))
        {
            OLO_CORE_ERROR("OpenInExplorer: path does not exist '{}': {}", canonical.string(), ecExists.message());
            return;
        }
        std::wstring args = L"/select,\"" + canonical.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#endif
    }

    void ContentBrowserPanel::OpenExternally([[maybe_unused]] const std::filesystem::path& path)
    {
#ifdef OLO_PLATFORM_WINDOWS
        std::error_code ecCanonical;
        auto canonical = std::filesystem::canonical(path, ecCanonical);
        if (ecCanonical)
        {
            OLO_CORE_ERROR("OpenExternally: canonical failed for '{}': {}", path.string(), ecCanonical.message());
            return;
        }
        std::error_code ecIsFile;
        if (!std::filesystem::is_regular_file(canonical, ecIsFile))
        {
            OLO_CORE_ERROR("OpenExternally: not a regular file '{}': {}", canonical.string(), ecIsFile.message());
            return;
        }
        ShellExecuteW(nullptr, L"open", canonical.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }

    // =========================================================================
    // Icon Resolution
    // =========================================================================

    Ref<Texture2D>& ContentBrowserPanel::GetFileIcon(const std::filesystem::path& filepath)
    {
        // Check cached image thumbnails
        if (m_ImageIcons.contains(filepath))
            return m_ImageIcons[filepath];

        ContentFileType fileType = GetFileTypeFromExtension(filepath);

        switch (fileType)
        {
            case ContentFileType::Image:
            {
                // Limit thumbnail cache to avoid unbounded growth
                if (m_ImageIcons.size() < 200)
                {
                    auto imageIcon = Texture2D::Create(filepath.string());
                    if (imageIcon->IsLoaded())
                    {
                        auto& icon = m_ImageIcons[filepath] = imageIcon;
                        return icon;
                    }
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
            case ContentFileType::StreamingRegion:
                return m_SceneIcon;
            case ContentFileType::Dialogue:
                return m_DialogueIcon;
            case ContentFileType::SaveGame:
                return m_SaveGameIcon;
            case ContentFileType::ShaderGraph:
                return m_ShaderIcon;
            default:
                return m_FileIcon;
        }
    }

    // =========================================================================
    // Create Menu (preserved from original)
    // =========================================================================

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
                    std::filesystem::path newFolder =
                        (m_DirectoryTree.GetAssetRoot() / (m_CurrentDirectory ? m_CurrentDirectory->RelativePath : "")) / folderName;
                    std::filesystem::create_directories(newFolder);
                    m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
                    RebuildItemList();
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
                    std::filesystem::path currentDir =
                        m_DirectoryTree.GetAssetRoot() / (m_CurrentDirectory ? m_CurrentDirectory->RelativePath : "");
                    std::filesystem::path matPath = currentDir / (std::string(matName) + ".material");
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
                    m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
                    RebuildItemList();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Dialogue Tree"))
            {
                static char dialogueName[256] = "NewDialogue";
                ImGui::InputText("Name", dialogueName, sizeof(dialogueName));
                if (ImGui::Button("Create##DialogueTree"))
                {
                    std::filesystem::path currentDir =
                        m_DirectoryTree.GetAssetRoot() / (m_CurrentDirectory ? m_CurrentDirectory->RelativePath : "");
                    std::string baseName = dialogueName;
                    std::filesystem::path dialoguePath = currentDir / (baseName + ".olodialogue");
                    int counter = 1;
                    while (std::filesystem::exists(dialoguePath))
                    {
                        dialoguePath = currentDir / (baseName + "_" + std::to_string(counter++) + ".olodialogue");
                    }

                    auto dialogueAsset = Ref<DialogueTreeAsset>::Create();
                    DialogueNodeData rootNode;
                    rootNode.ID = UUID(1);
                    rootNode.Type = "dialogue";
                    rootNode.Name = "Start";
                    rootNode.Properties["speaker"] = std::string("NPC");
                    rootNode.Properties["text"] = std::string("Hello there!");
                    rootNode.EditorPosition = { 0.0f, 0.0f };
                    dialogueAsset->GetNodesWritable().push_back(std::move(rootNode));
                    dialogueAsset->SetRootNodeID(UUID(1));
                    dialogueAsset->RebuildNodeIndex();

                    AssetMetadata metadata;
                    metadata.FilePath = std::filesystem::relative(dialoguePath, Project::GetAssetDirectory());
                    metadata.Type = AssetType::DialogueTree;

                    DialogueTreeSerializer serializer;
                    serializer.Serialize(metadata, dialogueAsset);

                    OLO_CORE_INFO("Created dialogue tree: {}", dialoguePath.string());
                    m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
                    RebuildItemList();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Shader Graph"))
            {
                static char shaderGraphName[256] = "NewShaderGraph";
                ImGui::InputText("Name", shaderGraphName, sizeof(shaderGraphName));
                if (ImGui::Button("Create##ShaderGraph"))
                {
                    std::filesystem::path currentDir =
                        m_DirectoryTree.GetAssetRoot() / (m_CurrentDirectory ? m_CurrentDirectory->RelativePath : "");
                    std::string baseName = shaderGraphName;
                    std::filesystem::path sgPath = currentDir / (baseName + ".olosg");
                    int counter = 1;
                    while (std::filesystem::exists(sgPath))
                    {
                        sgPath = currentDir / (baseName + "_" + std::to_string(counter++) + ".olosg");
                    }

                    auto sgAsset = Ref<ShaderGraphAsset>::Create();
                    sgAsset->GetMutableGraph().SetName(baseName);

                    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
                    if (!outputNode)
                    {
                        OLO_CORE_ERROR("ContentBrowser: Failed to create PBROutput node");
                        return;
                    }
                    outputNode->EditorPosition = glm::vec2(400.0f, 200.0f);
                    sgAsset->GetMutableGraph().AddNode(std::move(outputNode));

                    AssetMetadata metadata;
                    metadata.FilePath = std::filesystem::relative(sgPath, Project::GetAssetDirectory());
                    metadata.Type = AssetType::ShaderGraph;

                    ShaderGraphSerializer serializer;
                    serializer.Serialize(metadata, sgAsset);

                    OLO_CORE_INFO("Created shader graph: {}", sgPath.string());
                    m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
                    RebuildItemList();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }

            ImGui::EndPopup();
        }
    }

    void ContentBrowserPanel::CreateMeshPrimitiveFile(const std::string& primitiveType)
    {
        std::filesystem::path currentDir =
            m_DirectoryTree.GetAssetRoot() / (m_CurrentDirectory ? m_CurrentDirectory->RelativePath : "");

        // Generate unique filename
        std::string baseName = primitiveType;
        std::filesystem::path filePath = currentDir / (baseName + ".primitive");
        int counter = 1;
        while (std::filesystem::exists(filePath))
        {
            filePath = currentDir / (baseName + "_" + std::to_string(counter++) + ".primitive");
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
        m_DirectoryTree.RefreshSubtree(m_CurrentDirectory);
        RebuildItemList();
    }

    // =========================================================================
    // Settings Persistence (imgui.ini)
    // =========================================================================

    ContentBrowserPanel* ContentBrowserPanel::s_Instance = nullptr;

    void ContentBrowserPanel::RegisterSettingsHandler()
    {
        s_Instance = this;

        // Guard against double-registration
        auto* ctx = ImGui::GetCurrentContext();
        for (auto& h : ctx->SettingsHandlers)
        {
            if (h.TypeHash == ImHashStr("ContentBrowser"))
                return;
        }

        ImGuiSettingsHandler handler{};
        handler.TypeName = "ContentBrowser";
        handler.TypeHash = ImHashStr("ContentBrowser");
        handler.ReadOpenFn = SettingsHandler_ReadOpen;
        handler.ReadLineFn = SettingsHandler_ReadLine;
        handler.WriteAllFn = SettingsHandler_WriteAll;
        ctx->SettingsHandlers.push_back(handler);
    }

    void* ContentBrowserPanel::SettingsHandler_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char*)
    {
        return s_Instance ? static_cast<void*>(s_Instance) : nullptr;
    }

    void ContentBrowserPanel::SettingsHandler_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        auto* panel = static_cast<ContentBrowserPanel*>(entry);
        if (!panel)
            return;

        f32 value = 0.0f;
        if (std::sscanf(line, "ThumbnailSize=%f", &value) == 1)
            panel->m_ThumbnailSize = value;
        else if (std::sscanf(line, "Padding=%f", &value) == 1)
            panel->m_Padding = value;
    }

    void ContentBrowserPanel::SettingsHandler_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
    {
        if (!s_Instance)
            return;

        buf->appendf("[%s][Settings]\n", handler->TypeName);
        buf->appendf("ThumbnailSize=%.1f\n", s_Instance->m_ThumbnailSize);
        buf->appendf("Padding=%.1f\n", s_Instance->m_Padding);
        buf->append("\n");
    }

} // namespace OloEngine
