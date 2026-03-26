#include "OloEnginePCH.h"
#include "EditorPreferencesPanel.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"

#include <imgui.h>
#include <yaml-cpp/yaml.h>

#include <fstream>

namespace OloEngine
{
    void EditorPreferencesPanel::Open(const EditorPreferences& currentPrefs, EditorCamera* camera)
    {
        m_Draft = currentPrefs;
        m_Camera = camera;
        m_BookmarkNameBuffer[0] = '\0';
        m_IsOpen = true;
    }

    bool EditorPreferencesPanel::OnImGuiRender(EditorPreferences& outPrefs)
    {
        if (!m_IsOpen)
        {
            return false;
        }

        bool applied = false;

        ImGui::OpenPopup("Editor Preferences");
        ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);

        if (ImGui::BeginPopupModal("Editor Preferences", &m_IsOpen, ImGuiWindowFlags_NoCollapse))
        {
            if (ImGui::BeginTabBar("PreferencesTabs"))
            {
                if (ImGui::BeginTabItem("General"))
                {
                    DrawGeneralTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Camera"))
                {
                    DrawCameraTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Physics"))
                {
                    DrawPhysicsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Performance"))
                {
                    DrawPerformanceTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            ImGui::Separator();

            // Bottom buttons — right-aligned
            constexpr f32 buttonWidth = 80.0f;
            const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
            constexpr f32 totalWidth = buttonWidth * 3 + 2 * 8.0f; // approximate spacing
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - totalWidth - spacing);

            if (ImGui::Button("OK", ImVec2(buttonWidth, 0)))
            {
                outPrefs = m_Draft;
                applied = true;
                m_IsOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply", ImVec2(buttonWidth, 0)))
            {
                outPrefs = m_Draft;
                applied = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)))
            {
                m_IsOpen = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        return applied;
    }

    void EditorPreferencesPanel::DrawGeneralTab()
    {
        ImGui::Spacing();

        ImGui::Text("Grid");
        ImGui::Separator();
        ImGui::Checkbox("Show Grid", &m_Draft.ShowGrid);
        if (m_Draft.ShowGrid)
        {
            ImGui::DragFloat("Grid Spacing", &m_Draft.GridSpacing, 0.1f, 0.1f, 100.0f, "%.1f");
        }

        ImGui::Spacing();
        ImGui::Text("Rendering");
        ImGui::Separator();
        ImGui::Checkbox("3D Mode", &m_Draft.Is3DMode);
        ImGui::Checkbox("Show Physics Colliders", &m_Draft.ShowPhysicsColliders);
        ImGui::Checkbox("Show Light Gizmos", &m_Draft.ShowLightGizmos);

        ImGui::Spacing();
        ImGui::Text("Transform Snapping");
        ImGui::Separator();
        ImGui::DragFloat("Translate Snap", &m_Draft.TranslateSnap, 0.05f, 0.01f, 100.0f, "%.2f");
        ImGui::DragFloat("Rotate Snap", &m_Draft.RotateSnap, 1.0f, 1.0f, 180.0f, "%.1f deg");
        ImGui::DragFloat("Scale Snap", &m_Draft.ScaleSnap, 0.05f, 0.01f, 10.0f, "%.2f");

        ImGui::Spacing();
        ImGui::Text("Auto-Save");
        ImGui::Separator();
        ImGui::Checkbox("Enable Auto-Save", &m_Draft.EnableAutoSave);
        if (m_Draft.EnableAutoSave)
        {
            ImGui::SliderInt("Interval (seconds)", &m_Draft.AutoSaveIntervalSeconds, 10, 7200, "%d s");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Auto-saves the scene every N seconds while in Edit mode.\nRequires a prior manual save (scene must have a file path).\nRange: 10 s – 7200 s (2 hours).");
            }
        }
    }

    void EditorPreferencesPanel::DrawCameraTab()
    {
        ImGui::Spacing();

        ImGui::Text("Camera Settings");
        ImGui::Separator();
        ImGui::DragFloat("Fly Speed", &m_Draft.CameraFlySpeed, 0.1f, 0.1f, 100.0f, "%.1f");

        ImGui::Spacing();
        ImGui::Text("Camera Bookmarks");
        ImGui::Separator();

        ImGui::InputTextWithHint("##BookmarkName", "Bookmark name...", m_BookmarkNameBuffer, sizeof(m_BookmarkNameBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Save") && m_BookmarkNameBuffer[0] != '\0' && m_Camera)
        {
            m_Draft.Bookmarks.push_back({ std::string(m_BookmarkNameBuffer), m_Camera->GetPosition(), m_Camera->GetPitch(), m_Camera->GetYaw(),
                                          m_Camera->GetDistance() });
            m_BookmarkNameBuffer[0] = '\0';
        }

        i32 deleteIndex = -1;
        for (i32 i = 0; i < static_cast<i32>(m_Draft.Bookmarks.size()); ++i)
        {
            ImGui::PushID(i);
            auto& bm = m_Draft.Bookmarks[static_cast<sizet>(i)];
            if (ImGui::Button(bm.Name.c_str()) && m_Camera)
            {
                m_Camera->SetPosition(bm.Position);
                m_Camera->SetPitch(bm.Pitch);
                m_Camera->SetYaw(bm.Yaw);
                m_Camera->SetDistance(bm.Distance);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                deleteIndex = i;
            }
            ImGui::PopID();
        }
        if (deleteIndex >= 0)
        {
            m_Draft.Bookmarks.erase(m_Draft.Bookmarks.begin() + deleteIndex);
        }
    }

    void EditorPreferencesPanel::DrawPhysicsTab()
    {
        ImGui::Spacing();

        ImGui::Text("Physics Debug");
        ImGui::Separator();
        ImGui::Checkbox("Capture physics on play", &m_Draft.CapturePhysicsOnPlay);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Enable expensive physics debug capture during play mode.\nOff by default for production performance.");
        }
    }

    void EditorPreferencesPanel::DrawPerformanceTab()
    {
        ImGui::Spacing();

        ImGui::Text("Viewport Render Throttling");
        ImGui::Separator();
        ImGui::TextWrapped("Skip scene rendering when the previous frame exceeds the time budget. "
                           "The viewport shows the last rendered image until the GPU catches up.");
        ImGui::Spacing();

        ImGui::Checkbox("Throttle in Edit mode", &m_Draft.ThrottleEditMode);
        ImGui::Checkbox("Throttle in Play / Simulate mode", &m_Draft.ThrottlePlayMode);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("When enabled, rendering is skipped but simulation\n(physics, scripts, audio) continues running.");
        }

        ImGui::DragFloat("Budget (ms)", &m_Draft.RenderBudgetMs, 0.5f, 8.0f, 100.0f, "%.1f ms");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Rendering is skipped when the previous frame\nexceeded this duration. Lower values throttle more\naggressively. 33.3 ms = ~30 FPS threshold.");
        }
    }

    std::filesystem::path EditorPreferencesPanel::GetPrefsPath(const std::filesystem::path& projectDir)
    {
        return projectDir / "EditorPreferences.yaml";
    }

    void EditorPreferencesPanel::Save(const EditorPreferences& prefs, const std::filesystem::path& projectDir)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "EditorPreferences" << YAML::Value << YAML::BeginMap;

        out << YAML::Key << "ShowGrid" << YAML::Value << prefs.ShowGrid;
        out << YAML::Key << "GridSpacing" << YAML::Value << prefs.GridSpacing;
        out << YAML::Key << "TranslateSnap" << YAML::Value << prefs.TranslateSnap;
        out << YAML::Key << "RotateSnap" << YAML::Value << prefs.RotateSnap;
        out << YAML::Key << "ScaleSnap" << YAML::Value << prefs.ScaleSnap;
        out << YAML::Key << "CameraFlySpeed" << YAML::Value << prefs.CameraFlySpeed;
        out << YAML::Key << "ShowPhysicsColliders" << YAML::Value << prefs.ShowPhysicsColliders;
        out << YAML::Key << "ShowLightGizmos" << YAML::Value << prefs.ShowLightGizmos;
        out << YAML::Key << "Is3DMode" << YAML::Value << prefs.Is3DMode;
        out << YAML::Key << "CapturePhysicsOnPlay" << YAML::Value << prefs.CapturePhysicsOnPlay;
        out << YAML::Key << "ThrottleEditMode" << YAML::Value << prefs.ThrottleEditMode;
        out << YAML::Key << "ThrottlePlayMode" << YAML::Value << prefs.ThrottlePlayMode;
        out << YAML::Key << "RenderBudgetMs" << YAML::Value << prefs.RenderBudgetMs;
        out << YAML::Key << "EnableAutoSave" << YAML::Value << prefs.EnableAutoSave;
        out << YAML::Key << "AutoSaveIntervalSeconds" << YAML::Value << prefs.AutoSaveIntervalSeconds;

        // Camera bookmarks
        out << YAML::Key << "Bookmarks" << YAML::Value << YAML::BeginSeq;
        for (const auto& bm : prefs.Bookmarks)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << bm.Name;
            out << YAML::Key << "PositionX" << YAML::Value << bm.Position.x;
            out << YAML::Key << "PositionY" << YAML::Value << bm.Position.y;
            out << YAML::Key << "PositionZ" << YAML::Value << bm.Position.z;
            out << YAML::Key << "Pitch" << YAML::Value << bm.Pitch;
            out << YAML::Key << "Yaw" << YAML::Value << bm.Yaw;
            out << YAML::Key << "Distance" << YAML::Value << bm.Distance;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;
        out << YAML::EndMap;

        auto const path = GetPrefsPath(projectDir);
        std::ofstream fout(path);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("EditorPreferencesPanel::Save: failed to open '{}'", path.string());
            return;
        }
        fout << out.c_str();
        if (!fout.good())
        {
            OLO_CORE_ERROR("EditorPreferencesPanel::Save: write failed for '{}'", path.string());
        }
    }

    void EditorPreferencesPanel::Load(EditorPreferences& prefs, const std::filesystem::path& projectDir)
    {
        auto path = GetPrefsPath(projectDir);
        if (!std::filesystem::exists(path))
        {
            return;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            return;
        }

        YAML::Node data;
        try
        {
            data = YAML::Load(stream);
        }
        catch (const YAML::Exception&)
        {
            return;
        }

        auto node = data["EditorPreferences"];
        if (!node)
        {
            return;
        }

        try
        {
            if (node["ShowGrid"])
                prefs.ShowGrid = node["ShowGrid"].as<bool>();
            if (node["GridSpacing"])
                prefs.GridSpacing = std::clamp(node["GridSpacing"].as<f32>(), 0.1f, 100.0f);
            if (node["TranslateSnap"])
                prefs.TranslateSnap = std::clamp(node["TranslateSnap"].as<f32>(), 0.01f, 100.0f);
            if (node["RotateSnap"])
                prefs.RotateSnap = std::clamp(node["RotateSnap"].as<f32>(), 1.0f, 180.0f);
            if (node["ScaleSnap"])
                prefs.ScaleSnap = std::clamp(node["ScaleSnap"].as<f32>(), 0.01f, 10.0f);
            if (node["CameraFlySpeed"])
                prefs.CameraFlySpeed = std::clamp(node["CameraFlySpeed"].as<f32>(), 0.1f, 100.0f);
            if (node["ShowPhysicsColliders"])
                prefs.ShowPhysicsColliders = node["ShowPhysicsColliders"].as<bool>();
            if (node["ShowLightGizmos"])
                prefs.ShowLightGizmos = node["ShowLightGizmos"].as<bool>();
            if (node["Is3DMode"])
                prefs.Is3DMode = node["Is3DMode"].as<bool>();
            if (node["CapturePhysicsOnPlay"])
                prefs.CapturePhysicsOnPlay = node["CapturePhysicsOnPlay"].as<bool>();
            if (node["ThrottleEditMode"])
                prefs.ThrottleEditMode = node["ThrottleEditMode"].as<bool>();
            if (node["ThrottlePlayMode"])
                prefs.ThrottlePlayMode = node["ThrottlePlayMode"].as<bool>();
            if (node["RenderBudgetMs"])
                prefs.RenderBudgetMs = std::clamp(node["RenderBudgetMs"].as<f32>(), 8.0f, 100.0f);
            if (node["EnableAutoSave"])
                prefs.EnableAutoSave = node["EnableAutoSave"].as<bool>();
            if (node["AutoSaveIntervalSeconds"])
                prefs.AutoSaveIntervalSeconds = std::clamp(node["AutoSaveIntervalSeconds"].as<int>(), 10, 7200);

            // Camera bookmarks
            if (auto bookmarks = node["Bookmarks"])
            {
                prefs.Bookmarks.clear();
                for (const auto& bmNode : bookmarks)
                {
                    CameraBookmark bm;
                    bm.Name = bmNode["Name"].as<std::string>();
                    bm.Position.x = bmNode["PositionX"].as<f32>();
                    bm.Position.y = bmNode["PositionY"].as<f32>();
                    bm.Position.z = bmNode["PositionZ"].as<f32>();
                    bm.Pitch = bmNode["Pitch"].as<f32>();
                    bm.Yaw = bmNode["Yaw"].as<f32>();
                    bm.Distance = bmNode["Distance"].as<f32>();
                    prefs.Bookmarks.push_back(std::move(bm));
                }
            }
        }
        catch (const YAML::Exception&)
        {
            OLO_CORE_WARN("EditorPreferences: failed to parse one or more values, using defaults");
            return;
        }
    }
} // namespace OloEngine
