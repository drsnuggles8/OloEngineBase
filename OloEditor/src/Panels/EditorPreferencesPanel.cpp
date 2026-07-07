#include "OloEnginePCH.h"
#include "EditorPreferencesPanel.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"

#include <imgui.h>
#include <yaml-cpp/yaml.h>

#include <cmath>
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
        ImGui::Checkbox("Show Bounding Boxes", &m_Draft.ShowBoundingBoxes);

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
            const auto& bm = m_Draft.Bookmarks[static_cast<sizet>(i)];
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

        ImGui::Spacing();
        ImGui::Text("Frame-Rate Limiter");
        ImGui::Separator();
        ImGui::TextWrapped("Cap the windowed main loop to a target frame rate (sleep + short spin). "
                           "Composes with vsync — the limiter only sleeps for time vsync didn't "
                           "already consume, so it never double-throttles.");
        ImGui::Spacing();

        static const char* const kCapLabels[] = { "Off", "60 FPS", "120 FPS", "144 FPS", "Custom" };
        static constexpr u32 kCapValues[] = { 0u, 60u, 120u, 144u };
        constexpr int kCustomIndex = 4;

        int capIndex = kCustomIndex; // any non-preset value shows as "Custom"
        for (int i = 0; i < kCustomIndex; ++i)
        {
            if (m_Draft.FrameRateCap == kCapValues[i])
            {
                capIndex = i;
                break;
            }
        }

        if (ImGui::Combo("Frame Cap", &capIndex, kCapLabels, IM_ARRAYSIZE(kCapLabels)))
        {
            // Seed Custom with a non-preset value so the selection sticks.
            m_Draft.FrameRateCap = (capIndex < kCustomIndex) ? kCapValues[capIndex] : 90u;
        }
        if (capIndex == kCustomIndex)
        {
            int customFps = static_cast<int>(m_Draft.FrameRateCap);
            if (ImGui::DragInt("Custom FPS", &customFps, 1.0f, 1, 1000, "%d FPS"))
            {
                m_Draft.FrameRateCap = static_cast<u32>(std::clamp(customFps, 1, 1000));
            }
        }

        ImGui::Spacing();
        ImGui::Text("Frame-Time Smoothing");
        ImGui::Separator();
        ImGui::TextWrapped("Damp frame-time jitter with an exponential moving average on the "
                           "delta handed to gameplay/camera. Off by default.");
        ImGui::Spacing();

        bool smoothingOn = m_Draft.FrameTimeSmoothing < 1.0f;
        if (ImGui::Checkbox("Smooth frame delta (EMA)", &smoothingOn))
        {
            m_Draft.FrameTimeSmoothing = smoothingOn ? 0.2f : 1.0f;
        }
        if (smoothingOn)
        {
            ImGui::DragFloat("Smoothing (alpha)", &m_Draft.FrameTimeSmoothing, 0.01f, 0.01f, 0.99f, "%.2f");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("EMA weight for the newest frame's delta.\nLower = smoother but laggier. 1.0 disables smoothing.");
            }
        }

        ImGui::Spacing();
        ImGui::Text("Render Interpolation");
        ImGui::Separator();
        ImGui::TextWrapped("Decouple rendering from the fixed simulation tick: the sim runs at a "
                           "stable fixed rate while each displayed frame renders a pose "
                           "interpolated between the last two ticks. Keeps motion smooth when the "
                           "refresh rate isn't a multiple of the sim rate (e.g. 60 Hz sim / 144 Hz "
                           "display). On by default.");
        ImGui::Spacing();
        ImGui::Checkbox("Interpolate render pose between fixed ticks", &m_Draft.RenderInterpolation);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Off = one render per sim tick (may judder at non-multiple refresh rates).\n"
                              "Purely a presentation blend — never affects simulation determinism.");
        }
    }

    std::filesystem::path EditorPreferencesPanel::GetPrefsPath(const std::filesystem::path& projectDir)
    {
        return projectDir / "EditorPreferences.yaml";
    }

    void EditorPreferencesPanel::Save(const EditorPreferences& prefs, const std::filesystem::path& projectDir) const
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
        out << YAML::Key << "ShowBoundingBoxes" << YAML::Value << prefs.ShowBoundingBoxes;
        out << YAML::Key << "Is3DMode" << YAML::Value << prefs.Is3DMode;
        out << YAML::Key << "CapturePhysicsOnPlay" << YAML::Value << prefs.CapturePhysicsOnPlay;
        out << YAML::Key << "ThrottleEditMode" << YAML::Value << prefs.ThrottleEditMode;
        out << YAML::Key << "ThrottlePlayMode" << YAML::Value << prefs.ThrottlePlayMode;
        out << YAML::Key << "RenderBudgetMs" << YAML::Value << prefs.RenderBudgetMs;
        out << YAML::Key << "FrameRateCap" << YAML::Value << prefs.FrameRateCap;
        out << YAML::Key << "FrameTimeSmoothing" << YAML::Value << prefs.FrameTimeSmoothing;
        out << YAML::Key << "RenderInterpolation" << YAML::Value << prefs.RenderInterpolation;
        out << YAML::Key << "EnableAutoSave" << YAML::Value << prefs.EnableAutoSave;
        out << YAML::Key << "AutoSaveIntervalSeconds" << YAML::Value << prefs.AutoSaveIntervalSeconds;
        out << YAML::Key << "McpAutoStart" << YAML::Value << prefs.McpAutoStart;
        out << YAML::Key << "McpPort" << YAML::Value << prefs.McpPort;
        out << YAML::Key << "McpRedactPaths" << YAML::Value << prefs.McpRedactPaths;

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

    void EditorPreferencesPanel::Load(EditorPreferences& prefs, const std::filesystem::path& projectDir) const
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
                if (f32 v = node["GridSpacing"].as<f32>(); std::isfinite(v))
                    prefs.GridSpacing = std::clamp(v, 0.1f, 100.0f);
            if (node["TranslateSnap"])
                if (f32 v = node["TranslateSnap"].as<f32>(); std::isfinite(v))
                    prefs.TranslateSnap = std::clamp(v, 0.01f, 100.0f);
            if (node["RotateSnap"])
                if (f32 v = node["RotateSnap"].as<f32>(); std::isfinite(v))
                    prefs.RotateSnap = std::clamp(v, 1.0f, 180.0f);
            if (node["ScaleSnap"])
                if (f32 v = node["ScaleSnap"].as<f32>(); std::isfinite(v))
                    prefs.ScaleSnap = std::clamp(v, 0.01f, 10.0f);
            if (node["CameraFlySpeed"])
                if (f32 v = node["CameraFlySpeed"].as<f32>(); std::isfinite(v))
                    prefs.CameraFlySpeed = std::clamp(v, 0.1f, 100.0f);
            if (node["ShowPhysicsColliders"])
                prefs.ShowPhysicsColliders = node["ShowPhysicsColliders"].as<bool>();
            if (node["ShowLightGizmos"])
                prefs.ShowLightGizmos = node["ShowLightGizmos"].as<bool>();
            if (node["ShowBoundingBoxes"])
                prefs.ShowBoundingBoxes = node["ShowBoundingBoxes"].as<bool>();
            if (node["Is3DMode"])
                prefs.Is3DMode = node["Is3DMode"].as<bool>();
            if (node["CapturePhysicsOnPlay"])
                prefs.CapturePhysicsOnPlay = node["CapturePhysicsOnPlay"].as<bool>();
            if (node["ThrottleEditMode"])
                prefs.ThrottleEditMode = node["ThrottleEditMode"].as<bool>();
            if (node["ThrottlePlayMode"])
                prefs.ThrottlePlayMode = node["ThrottlePlayMode"].as<bool>();
            if (node["RenderBudgetMs"])
                if (f32 v = node["RenderBudgetMs"].as<f32>(); std::isfinite(v))
                    prefs.RenderBudgetMs = std::clamp(v, 8.0f, 100.0f);
            if (node["FrameRateCap"])
                prefs.FrameRateCap = std::min(node["FrameRateCap"].as<u32>(), 1000u);
            if (node["FrameTimeSmoothing"])
                if (f32 v = node["FrameTimeSmoothing"].as<f32>(); std::isfinite(v))
                    prefs.FrameTimeSmoothing = std::clamp(v, 0.01f, 1.0f);
            if (node["RenderInterpolation"])
                prefs.RenderInterpolation = node["RenderInterpolation"].as<bool>();
            if (node["EnableAutoSave"])
                prefs.EnableAutoSave = node["EnableAutoSave"].as<bool>();
            if (node["AutoSaveIntervalSeconds"])
                prefs.AutoSaveIntervalSeconds = std::clamp(node["AutoSaveIntervalSeconds"].as<int>(), 10, 7200);
            if (node["McpAutoStart"])
                prefs.McpAutoStart = node["McpAutoStart"].as<bool>();
            if (node["McpPort"])
                prefs.McpPort = std::clamp(node["McpPort"].as<int>(), 1024, 65535);
            if (node["McpRedactPaths"])
                prefs.McpRedactPaths = node["McpRedactPaths"].as<bool>();

            // Camera bookmarks
            if (auto bookmarks = node["Bookmarks"])
            {
                prefs.Bookmarks.clear();
                for (const auto& bmNode : bookmarks)
                {
                    CameraBookmark bm;
                    bm.Name = bmNode["Name"].as<std::string>();
                    if (f32 v = bmNode["PositionX"].as<f32>(); std::isfinite(v))
                        bm.Position.x = v;
                    if (f32 v = bmNode["PositionY"].as<f32>(); std::isfinite(v))
                        bm.Position.y = v;
                    if (f32 v = bmNode["PositionZ"].as<f32>(); std::isfinite(v))
                        bm.Position.z = v;
                    if (f32 v = bmNode["Pitch"].as<f32>(); std::isfinite(v))
                        bm.Pitch = v;
                    if (f32 v = bmNode["Yaw"].as<f32>(); std::isfinite(v))
                        bm.Yaw = v;
                    if (f32 v = bmNode["Distance"].as<f32>(); std::isfinite(v))
                        bm.Distance = v;
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
