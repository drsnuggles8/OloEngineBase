#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Scene.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine
{
    class CommandHistory;
}

namespace OloEngine::Automation
{
    // The scene settings mirrored by the editor renderer. Keep authored settings
    // separate from their live quality-tier overlay when retaining a document.
    struct SceneDocumentSettings
    {
        PostProcessSettings PostProcess;
        SnowSettings Snow;
        WindSettings Wind;
        SnowAccumulationSettings SnowAccumulation;
        SnowEjectaSettings SnowEjecta;
        PrecipitationSettings Precipitation;
        FogSettings Fog;

        [[nodiscard]] static SceneDocumentSettings Capture(const Scene& scene);
        void Apply(Scene& scene) const;
    };

    // Retain the actual scene for lifecycle undo: serializing/copying a scene
    // loses editor-only component state and breaks commands holding its Ref.
    struct SceneDocumentSnapshot
    {
        Ref<Scene> SceneRef;
        std::filesystem::path Path;
        std::string Name;
        std::vector<UUID> Selection;
        SceneDocumentSettings AuthoredSettings;
        SceneDocumentSettings RenderedSettings;
    };

    [[nodiscard]] SceneDocumentSnapshot CaptureSceneDocument(const Ref<Scene>& scene,
                                                             const std::filesystem::path& path = {});
    void ApplySceneDocument(const SceneDocumentSnapshot& document);

    // Main-thread callbacks supplied by EditorLayer or a scene-owning test host.
    // Install rebinds panels/renderer without clearing CommandHistory. PrepareSave
    // returns a value snapshot and must not mutate the current scene or files.
    struct SceneDocumentAccess
    {
        std::function<SceneDocumentSnapshot()> Capture;
        std::function<void(const SceneDocumentSnapshot&)> Install;
        std::function<SceneDocumentSnapshot(const SceneDocumentSnapshot&)> PrepareSave;
        std::function<std::filesystem::path()> AssetDirectory;
        // Unlike the mutation history accessor, authored dirty state remains
        // readable while Play/Simulate is using a transient scene copy.
        std::function<bool()> IsDirty;
    };

    // These functions and their undo/redo commands run only on the main thread.
    void NewSceneDocument(const SceneDocumentAccess& access, CommandHistory& history, const std::string& name);
    // An empty path saves to the current destination. Returns whether an undo
    // entry was added; an identical save only establishes the saved checkpoint.
    [[nodiscard]] bool SaveSceneDocument(const SceneDocumentAccess& access, CommandHistory& history,
                                         const std::filesystem::path& path = {});
} // namespace OloEngine::Automation
