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
        bool ShowBoundingBoxes = false;
        bool Is3DMode = true;

        // Performance — viewport render throttling
        bool ThrottleEditMode = true;
        bool ThrottlePlayMode = false;
        f32 RenderBudgetMs = 33.3f;

        // Frame pacing (#456). FrameRateCap: 0 = uncapped, else the target FPS
        // the windowed main loop paces itself to (off / 60 / 120 / 144 / custom).
        // FrameTimeSmoothing: EMA weight for the delta handed to layers, in
        // (0, 1]; 1.0 disables smoothing.
        u32 FrameRateCap = 0;
        f32 FrameTimeSmoothing = 1.0f;

        // Render interpolation (#502). When true, Play/runtime rendering blends
        // between the two most recent fixed-tick poses (alpha = accumulator /
        // fixedStep) so motion stays smooth at non-multiple refresh rates. Purely
        // presentational — never affects simulation determinism. On by default.
        bool RenderInterpolation = true;

        // Physics debug
        bool CapturePhysicsOnPlay = false;

        // Auto-save (synced with ProjectConfig)
        bool EnableAutoSave = true;
        int AutoSaveIntervalSeconds = 300;

        // MCP diagnostics server (#285). Off by default; these just remember the
        // user's last choice. McpAutoStart starting the server on launch is an
        // explicit, persisted opt-in (the default stays off).
        bool McpAutoStart = false;
        int McpPort = 7345; // matches MCP::DefaultPort
        bool McpRedactPaths = false;
    };

    class EditorPreferencesPanel
    {
      public:
        EditorPreferencesPanel() = default;

        void Open(const EditorPreferences& currentPrefs, EditorCamera* camera);
        // Returns true if preferences were applied (OK or Apply pressed)
        bool OnImGuiRender(EditorPreferences& outPrefs);

        void Save(const EditorPreferences& prefs, const std::filesystem::path& projectDir) const;
        void Load(EditorPreferences& prefs, const std::filesystem::path& projectDir) const;

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
