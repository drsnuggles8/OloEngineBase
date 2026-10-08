#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Scene/Scene.h"

#include <filesystem>
#include "OloEngine/Containers/String.h"
#include <string_view>

namespace OloEngine::SceneTransition
{
    // Runtime scene load/switch support (issue #642).
    //
    // A script requests a scene change by name (`Scene::SetPendingSceneLoad`);
    // the host — OloRuntime's RuntimeLayer or the editor's Play mode — services
    // the request once the tick has returned. These two helpers are the shared
    // half of that servicing, so both hosts resolve names and validate the
    // loaded scene the same way.

    /// Scene file extensions the loader accepts, matching the editor's
    /// "Open Scene" filter.
    [[nodiscard]] bool IsSceneFileExtension(const std::filesystem::path& path);

    /// Resolve a script-supplied scene request to an existing file on disk.
    ///
    /// `request` is whatever the script passed — a bare name ("Level2"), a
    /// file name ("Level2.olo"), or a relative path ("Scenes/Level2.olo").
    /// Candidates are tried in this order, each with and without an appended
    /// `.olo`:
    ///   1. `rootDirectory / request`
    ///   2. `rootDirectory / "Scenes" / request`
    ///   3. `request` as given (relative to the current working directory)
    ///   4. a recursive search of `rootDirectory / "Scenes"` for a file whose
    ///      name matches (sorted, so the answer is deterministic)
    ///
    /// An empty `rootDirectory` means the current working directory, which is
    /// where a shipped game's data lives.
    ///
    /// Returns an empty path when nothing matches, when the request names a
    /// non-scene file, or when it is ABSOLUTE or contains a `..` component — a
    /// scene request has no legitimate reason to climb out of the game's data
    /// directory. Both forms are refused, not just `..`: appending an absolute
    /// path replaces the root, so allowing one would make the `..` check
    /// pointless.
    [[nodiscard]] std::filesystem::path ResolveScenePath(std::string_view request,
                                                         const std::filesystem::path& rootDirectory = {});

    struct LoadResult
    {
        /// The deserialized scene, or null when the load failed. It has NOT
        /// been started — the caller still owns `OnViewportResize` /
        /// `OnRuntimeStart` and the ordering against the outgoing scene's
        /// `OnRuntimeStop`.
        Ref<Scene> LoadedScene;
        /// Human-readable reason the load failed. Empty on success.
        FString Error;

        [[nodiscard]] explicit operator bool() const
        {
            return static_cast<bool>(LoadedScene);
        }
    };

    /// Deserialize `path` into a brand-new Scene.
    ///
    /// Deliberately loads into a fresh Scene and validates it BEFORE the caller
    /// tears anything down, so a bad path or a camera-less scene leaves the
    /// running game on its current scene instead of dropping it into a black
    /// screen (or, worse, closing it).
    ///
    /// `requirePrimaryCamera` refuses a scene with no primary CameraComponent —
    /// the runtime has no editor camera to fall back on, so such a scene would
    /// render nothing.
    /// When `saveSlot` is non-empty, restore it into the successfully
    /// deserialized scene before returning. A restore failure discards the new
    /// scene so the host has not yet stopped its current one.
    [[nodiscard]] LoadResult LoadSceneFile(const std::filesystem::path& path, bool requirePrimaryCamera,
                                           std::string_view saveSlot = {});

    /// The scene-level render settings Renderer3D mirrors as process-global
    /// state. A scene stores them; the renderer only sees them once they are
    /// published, which every host must do when a scene becomes the one it
    /// draws (issue #1563).
    struct SceneRenderSettings
    {
        PostProcessSettings PostProcess;
        SnowSettings Snow;
        WindSettings Wind;
        SnowAccumulationSettings SnowAccumulation;
        SnowEjectaSettings SnowEjecta;
        PrecipitationSettings Precipitation;
        FogSettings Fog;

        [[nodiscard]] static SceneRenderSettings Capture(const Scene& scene);
        /// What the renderer currently draws with, quality tiering included.
        [[nodiscard]] static SceneRenderSettings CaptureRenderer();
        void Apply(Scene& scene) const;
        void PublishToRenderer() const;
    };

    /// Make `scene`'s authored render settings the renderer's effective ones:
    /// publish them, overlay the quality tier (when `tiering` is non-null), then
    /// push the result into the render graph if Renderer3D is up. The order is
    /// the contract: authored first, tier on top. The engine has no player
    /// graphics preferences yet; when it does, they go on top of both.
    ///
    /// The shared activation seam for the editor's scene open, the editor's
    /// Play-mode scene switch, and the runtime's initial load, reload and
    /// script-driven switch.
    void ApplySceneRenderSettings(const Scene& scene, const QualityTieringSettings* tiering);

    /// As above, with the active project's quality tier (none without a project).
    void ApplySceneRenderSettings(const Scene& scene);
} // namespace OloEngine::SceneTransition
