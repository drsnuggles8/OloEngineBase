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
     * 5. **Copy Engine Resources** — Copy shaders and fonts
     * 6. **Copy Mono Runtime** — Copy mono/lib and mono/etc for C# scripting
     *    (skipped when IsScriptingAvailableOnPlatform is false for the target)
     * 7. **Copy ScriptCore** — Copy the C# ScriptCore assembly (same skip)
     * 8. **Copy Scenes** — Copy .olo scene files from the project
     *    plus loose Lua scripts and writable project runtime configuration
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
     * │   └── AssetPack.olopack   (textures, meshes, etc.)
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
