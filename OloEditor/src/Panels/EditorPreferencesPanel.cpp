#include "OloEnginePCH.h"
#include "EditorPreferencesPanel.h"

#include <imgui.h>
#include <yaml-cpp/yaml.h>

#include <fstream>

namespace OloEngine
{
    void EditorPreferencesPanel::OnImGuiRender(EditorPreferences& prefs)
    {
        ImGui::Begin("Editor Preferences");

        ImGui::Text("Grid");
        ImGui::Separator();
        m_Dirty |= ImGui::Checkbox("Show Grid", &prefs.ShowGrid);
        if (prefs.ShowGrid)
        {
            m_Dirty |= ImGui::DragFloat("Grid Spacing", &prefs.GridSpacing, 0.1f, 0.1f, 100.0f, "%.1f");
        }

        ImGui::Spacing();
        ImGui::Text("Transform Snapping");
        ImGui::Separator();
        m_Dirty |= ImGui::DragFloat("Translate Snap", &prefs.TranslateSnap, 0.05f, 0.01f, 100.0f, "%.2f");
        m_Dirty |= ImGui::DragFloat("Rotate Snap", &prefs.RotateSnap, 1.0f, 1.0f, 180.0f, "%.1f deg");
        m_Dirty |= ImGui::DragFloat("Scale Snap", &prefs.ScaleSnap, 0.05f, 0.01f, 10.0f, "%.2f");

        ImGui::Spacing();
        ImGui::Text("Camera");
        ImGui::Separator();
        m_Dirty |= ImGui::DragFloat("Fly Speed", &prefs.CameraFlySpeed, 0.1f, 0.1f, 100.0f, "%.1f");

        ImGui::Spacing();
        ImGui::Text("Rendering");
        ImGui::Separator();
        m_Dirty |= ImGui::Checkbox("Show Physics Colliders", &prefs.ShowPhysicsColliders);
        m_Dirty |= ImGui::Checkbox("3D Mode", &prefs.Is3DMode);

        ImGui::End();
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
        out << YAML::Key << "Is3DMode" << YAML::Value << prefs.Is3DMode;

        out << YAML::EndMap;
        out << YAML::EndMap;

        std::ofstream fout(GetPrefsPath(projectDir));
        if (fout.is_open())
        {
            fout << out.c_str();
            if (fout.good())
            {
                m_Dirty = false;
            }
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
                prefs.GridSpacing = node["GridSpacing"].as<f32>();
            if (node["TranslateSnap"])
                prefs.TranslateSnap = node["TranslateSnap"].as<f32>();
            if (node["RotateSnap"])
                prefs.RotateSnap = node["RotateSnap"].as<f32>();
            if (node["ScaleSnap"])
                prefs.ScaleSnap = node["ScaleSnap"].as<f32>();
            if (node["CameraFlySpeed"])
                prefs.CameraFlySpeed = node["CameraFlySpeed"].as<f32>();
            if (node["ShowPhysicsColliders"])
                prefs.ShowPhysicsColliders = node["ShowPhysicsColliders"].as<bool>();
            if (node["Is3DMode"])
                prefs.Is3DMode = node["Is3DMode"].as<bool>();
        }
        catch (const YAML::Exception&)
        {
            OLO_CORE_WARN("EditorPreferences: failed to parse one or more values, using defaults");
            return;
        }

        m_Dirty = false;
    }
} // namespace OloEngine
