#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Build/GameBuildSettings.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine
{
    /**
     * @brief Result of a game build operation
     */
    struct [[nodiscard]] GameBuildResult
    {
        bool Success = false;
        std::string ErrorMessage;
        std::filesystem::path OutputPath;
        sizet AssetCount = 0;
        sizet SceneCount = 0;
        sizet TotalSizeBytes = 0;
        f64 BuildTimeSeconds = 0.0;
    };

    /**
     * @brief Stage the dynamic libraries placed beside a built runtime.
     *
     * Kept as a small filesystem seam so packaging tests can exercise the same
     * dependency-discovery rule as the full editor build without constructing
     * an active project or asset pack.
     */
    bool StageRuntimeDependencyLibraries(
        BuildTargetPlatform targetPlatform,
        const std::filesystem::path& runtimeBinDir,
        const std::filesystem::path& outputDir,
        sizet& copiedCount,
        std::string& errorMessage);

    // Stage a managed DLL and its optional matching PDB. A rebuild without
    // symbols removes the old destination PDB so it cannot describe a different DLL.
    bool StageManagedAssembly(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        std::string& errorMessage);

    /**
     * @brief Stage project textures needed by legacy path-based scene components.
     *
     * Runtime scene deserialization still resolves UIImage/Sprite texture paths
     * through Texture2D::Create, so those files must remain available beside the
     * asset pack until scene serialization stores pack handles instead.
     */
    bool StageLooseRuntimeTextures(
        const std::filesystem::path& projectAssetsDir,
        const std::filesystem::path& outputAssetsDir,
        sizet& copiedCount,
        std::string& errorMessage);

    /**
     * @brief Stage the loose files shipped scenes read by PATH rather than through the asset pack.
     *
     * A foliage layer's MeshPath, AlbedoPath and leaf maps, an animated mesh's
     * FilePath and a terrain layer's textures are stored in the scene as content
     * paths and opened through ResolveContentPath, never through the asset pack
     * (issue #1392). A packaged game that lacks one of those files loads its
     * scene and draws the designed fallback: every authored plant becomes a flat
     * card and its impostor is never baked, with only a log line to say so.
     *
     * Every scalar in every scene is read with ResolveContentPath's spelling rule:
     * `<assetDirectoryName>/...` is project content under `projectDir`, `assets/...`
     * is engine content under `engineRoot` (the editor working directory), and any
     * other value naming a file under the asset directory is the asset-relative
     * spelling an audio source's Filepath uses and stages as
     * `<assetDirectoryName>/<value>`. A value
     * naming an existing file is copied to `outputDir / <value>`, which is where the
     * runtime resolves it: the runtime mounts its project at the game directory and
     * runs with that directory as its working directory. A value naming a directory
     * stages the files directly inside it (a cubemap face folder).
     *
     * The files a staged file opens by itself come along: an .obj's `mtllib`, an
     * .mtl's texture maps, a .gltf's buffer and image URIs, and a `<file>.oloimport`
     * import-settings sidecar.
     *
     * A reference that cannot ship — missing, absolute, or spelled `../` out of the
     * project — is appended to `unresolved` as "<scene>: <reference> (<why>)" and
     * does not fail the step; the editor cannot open it either. A copy that fails
     * does fail it.
     */
    bool StageSceneReferencedContent(
        const std::vector<std::filesystem::path>& sceneFiles,
        const std::filesystem::path& projectDir,
        const std::filesystem::path& assetDirectoryName,
        const std::filesystem::path& engineRoot,
        const std::filesystem::path& outputDir,
        sizet& copiedCount,
        std::vector<std::string>& unresolved,
        std::string& errorMessage);

    /**
     * @brief Stage the CI-baked shader pack (.osp), if one was built (issue #908).
     *
     * A shader pack is a portable, content-hash-validated cache of
     * pre-compiled SPIR-V — optional, not required. `packStaged` reports
     * whether one was found and copied; when `shaderPackSrc` doesn't exist
     * this returns true with `packStaged == false` (not an error) because a
     * packaged runtime with no pack falls back to compiling from the shipped
     * `assets/shaders` source tree, same as it always has.
     */
    bool StageShaderPack(
        const std::filesystem::path& shaderPackSrc,
        const std::filesystem::path& outputAssetsDir,
        bool& packStaged,
        std::string& errorMessage);

    /**
     * @brief Orchestrates the full game build pipeline
     *
     * The GameBuildPipeline is responsible for taking the active project
     * in the editor and producing a self-contained, distributable game folder.
     *
     * ## Build Steps
     * 1. **Validate** — Check the requested target platform is one this host can
     *    produce (#891), then that the project has scenes and a valid configuration
     * 2. **Pack Assets** — Use AssetPackBuilder to create the .olopack file
     * 3. **Copy Runtime** — Copy the OloRuntime binary to the output directory
     *    (`OloRuntime.exe` on Windows, `OloRuntime` on Linux — see
     *    GetHostExecutableFileName), then embed a custom icon (Windows) or write
     *    a .desktop launcher entry (Linux)
     * 4. **Copy Dependencies** — Copy runtime-adjacent DLLs — Windows only
     * 5. **Stage Runtime Content** — StageRuntimeContent: engine shaders,
     *    textures and fonts, the .olo scenes, loose Lua scripts, loose project
     *    textures, writable project runtime configuration, and every file a
     *    scene references by path (StageSceneReferencedContent)
     * 6. **Copy Mono Runtime** — Copy mono/lib and mono/etc for C# scripting
     *    (skipped when IsScriptingAvailableOnPlatform is false for the target)
     * 7. **Copy ScriptCore** — Copy the C# ScriptCore assembly (same skip)
     * 8. (folded into step 5)
     * 9. **Write Manifest** — Write game.manifest with game name, start scene,
     *    target platform and C# scripting availability, etc.
     *
     * ## Output Structure (Windows target shown; Linux drops the .exe suffix,
     * the mono/ and Resources/Scripts/ directories, and adds a relocatable
     * .desktop entry plus OloGameLauncher.sh and an optional icons/game-icon.*)
     * ```
     * OutputDirectory/GameName/
     * ├── GameName.exe            (renamed OloRuntime.exe)
     * ├── *.dll                   (runtime-adjacent dynamic libraries)
     * ├── game.manifest           (YAML config: game name, start scene)
     * ├── Assets/
     * │   ├── AssetPack.olopack   (textures, meshes, etc.)
     * │   └── ...                 (loose textures, .lua, files scenes reference by path)
     * ├── Config/
     * │   └── InputActions.yaml   (writable persisted control bindings)
     * ├── Scenes/
     * │   └── *.olo               (scene files from project)
     * ├── assets/
     * │   ├── shaders/            (GLSL shader files)
     * │   └── fonts/              (font files)
     * ├── mono/
     * │   ├── lib/                (Mono runtime libraries)
     * │   └── etc/                (Mono configuration)
     * └── Resources/
     *     └── Scripts/
     *         └── OloEngine-ScriptCore.dll
     * ```
     *
     * ## Thread Safety
     * Build operations run on a background thread. Use the progress/cancel
     * atomics for inter-thread communication with the UI.
     */
    class GameBuildPipeline final
    {
      public:
        // Static utility class — no instantiation
        GameBuildPipeline() = delete;

        /**
         * @brief Execute the full game build pipeline
         *
         * @param settings Build configuration
         * @param progress Atomic progress tracker (0.0 to 1.0), updated during build
         * @param cancelToken Optional cancellation token; set to true to cancel
         * @return GameBuildResult with success/failure info and output path
         */
        static GameBuildResult Build(
            const GameBuildSettings& settings,
            std::atomic<f32>& progress,
            const std::atomic<bool>* cancelToken = nullptr);

        /**
         * @brief Lay out every loose file the packaged runtime reads, around the asset pack
         *
         * Engine resources (shaders, textures, fonts), the scenes, the Lua scripts,
         * the loose project textures, the input-action config and the files the
         * scenes reference by path (StageSceneReferencedContent). Build() runs
         * exactly this; it is public so a test can stage the same layout without an
         * asset pack, a runtime binary or a Mono runtime. Needs an active project
         * and the editor working directory, like Build().
         */
        static bool StageRuntimeContent(
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

      private:
        /**
         * @brief Validate the project is ready for building
         */
        static bool ValidateProject(std::string& errorMessage);

        /**
         * @brief Build the asset pack into the output directory
         */
        static bool BuildAssetPack(
            const GameBuildSettings& settings,
            const std::filesystem::path& outputDir,
            sizet& assetCount,
            sizet& sceneCount,
            std::atomic<f32>& progress,
            const std::atomic<bool>* cancelToken);

        /**
         * @brief Copy the runtime executable to the output directory
         */
        static bool CopyRuntimeExecutable(
            const GameBuildSettings& settings,
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy required shared libraries (DLLs) to the output directory
         */
        static bool CopyDependencyDLLs(
            const GameBuildSettings& settings,
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy engine runtime resources (shaders, fonts) to the output directory
         */
        static bool CopyEngineResources(
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy the Mono runtime files needed for C# scripting
         *
         * A no-op (returns true without copying anything) when the target
         * platform doesn't support C# scripting — see
         * IsScriptingAvailableOnPlatform.
         */
        static bool CopyMonoRuntime(
            const GameBuildSettings& settings,
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy the C# ScriptCore assembly
         *
         * A no-op (returns true without copying anything) when the target
         * platform doesn't support C# scripting — see
         * IsScriptingAvailableOnPlatform.
         */
        static bool CopyScriptCoreAssembly(
            const GameBuildSettings& settings,
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy scene files (.olo) from the project to the output directory
         *
         * Scenes are loaded from disk at runtime (not packed into the asset pack)
         * because the asset registry doesn't track .olo scene files.
         */
        static bool CopySceneFiles(
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy Lua script files (.lua) from the project to the output directory
         *
         * `LuaScriptEngine::OnCreateEntity` loads a script with `lua_State::load_file`
         * — a plain filesystem read, not an asset-pack lookup — and
         * `Scene::OnRuntimeStart` resolves the component's project-relative
         * `ScriptFile` through `Project::GetAssetFileSystemPath`. So the shipped game
         * needs the loose .lua files laid out under `<game>/Assets/` at the SAME
         * asset-relative paths they had in the project, which is exactly what the
         * runtime's in-memory project (`AssetDirectory = "Assets"`) resolves to.
         *
         * Non-fatal when the project has no scripts — plenty of games don't use Lua.
         */
        static bool CopyScriptFiles(
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy writable project runtime configuration
         *
         * InputActions.yaml must remain loose: OloRuntime loads it before the
         * start scene and RuntimeInputRebindMenu writes the player's changes
         * back to the same file so bindings survive a process restart.
         */
        static bool StageProjectRuntimeFiles(
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief StageSceneReferencedContent over every scene the build ships
         *
         * Logs each unresolved reference as a warning; they do not fail the build.
         */
        static bool StageProjectSceneReferences(
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Write the game manifest file with runtime configuration
         */
        static bool WriteGameManifest(
            const GameBuildSettings& settings,
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Embed a custom icon into the game executable using Windows resource APIs
         *
         * Replaces the default icon resource (ID 1) in the copied executable with
         * the user-selected .ico file. Non-fatal — returns false but lets the build continue.
         */
        static bool EmbedCustomIcon(
            const std::filesystem::path& exePath,
            const std::filesystem::path& iconPath,
            std::string& errorMessage);

        /**
         * @brief Write a Linux .desktop launcher entry next to the game executable
         *
         * The Linux arm of icon handling — stages a portable launcher and an
         * optional PNG/SVG/XPM icon. Non-fatal — returns false but lets the build continue.
         */
        static bool WriteLinuxDesktopEntry(
            const std::filesystem::path& exePath,
            const std::filesystem::path& iconPath,
            const std::string& gameName,
            std::string& errorMessage);

        /**
         * @brief Calculate total size of the output directory
         */
        static sizet CalculateDirectorySize(const std::filesystem::path& directory);
    };

} // namespace OloEngine
