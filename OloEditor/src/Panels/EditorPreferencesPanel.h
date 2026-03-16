#pragma once

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <string>

namespace OloEngine
{
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

        // Visuals
        bool ShowPhysicsColliders = false;
        bool Is3DMode = true;
    };

    class EditorPreferencesPanel
    {
      public:
        EditorPreferencesPanel() = default;

        void OnImGuiRender(EditorPreferences& prefs);

        void Save(const EditorPreferences& prefs, const std::filesystem::path& projectDir);
        void Load(EditorPreferences& prefs, const std::filesystem::path& projectDir);

      private:
        static std::filesystem::path GetPrefsPath(const std::filesystem::path& projectDir);

        bool m_Dirty = false;
    };
} // namespace OloEngine
