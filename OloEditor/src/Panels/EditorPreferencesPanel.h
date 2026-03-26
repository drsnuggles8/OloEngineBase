#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/vec3.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine
{
    class EditorCamera;

    struct CameraBookmark
    {
        std::string Name;
        glm::vec3 Position{};
        f32 Pitch = 0.0f;
        f32 Yaw = 0.0f;
        f32 Distance = 10.0f;
    };

    struct EditorPreferences
    {
        // Grid settings
        bool ShowGrid = true;
        f32 GridSpacing = 1.0f;

        // Transform snapping
        f32 TranslateSnap = 0.5f;
        f32 RotateSnap = 45.0f;
        f32 ScaleSnap = 0.5f;

        // Camera
        f32 CameraFlySpeed = 5.0f;
        std::vector<CameraBookmark> Bookmarks;

        // Visuals
        bool ShowPhysicsColliders = false;
        bool ShowLightGizmos = true;
        bool Is3DMode = true;

        // Performance — viewport render throttling
        bool ThrottleEditMode = true;
        bool ThrottlePlayMode = false;
        f32 RenderBudgetMs = 33.3f;

        // Physics debug
        bool CapturePhysicsOnPlay = false;

        // Auto-save (synced with ProjectConfig)
        bool EnableAutoSave = true;
        int AutoSaveIntervalSeconds = 300;
    };

    class EditorPreferencesPanel
    {
      public:
        EditorPreferencesPanel() = default;

        void Open(const EditorPreferences& currentPrefs, EditorCamera* camera);
        // Returns true if preferences were applied (OK or Apply pressed)
        bool OnImGuiRender(EditorPreferences& outPrefs);

        void Save(const EditorPreferences& prefs, const std::filesystem::path& projectDir);
        void Load(EditorPreferences& prefs, const std::filesystem::path& projectDir);

      private:
        static std::filesystem::path GetPrefsPath(const std::filesystem::path& projectDir);

        void DrawGeneralTab();
        void DrawCameraTab();
        void DrawPhysicsTab();
        void DrawPerformanceTab();

        bool m_IsOpen = false;
        EditorPreferences m_Draft;
        EditorCamera* m_Camera = nullptr;
        char m_BookmarkNameBuffer[64] = {};
    };
} // namespace OloEngine
