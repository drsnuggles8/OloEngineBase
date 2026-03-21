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
     * @brief Orchestrates the full game build pipeline
     *
     * The GameBuildPipeline is responsible for taking the active project
     * in the editor and producing a self-contained, distributable game folder.
     *
     * ## Build Steps
     * 1. **Validate** — Check that the project has scenes and a valid configuration
     * 2. **Pack Assets** — Use AssetPackBuilder to create the .olopack file
     * 3. **Copy Runtime** — Copy OloRuntime.exe to the output directory
     * 4. **Copy Dependencies** — Copy required DLLs (Mono, libpng, zlib, etc.)
     * 5. **Copy Engine Resources** — Copy shaders and fonts
     * 6. **Copy Mono Runtime** — Copy mono/lib and mono/etc for C# scripting
     * 7. **Copy ScriptCore** — Copy the C# ScriptCore assembly
     * 8. **Copy Scenes** — Copy .olo scene files from the project
     * 9. **Write Manifest** — Write game.manifest with game name, start scene, etc.
     *
     * ## Output Structure
     * ```
     * OutputDirectory/GameName/
     * ├── GameName.exe            (renamed OloRuntime.exe)
     * ├── game.manifest           (YAML config: game name, start scene)
     * ├── Assets/
     * │   └── AssetPack.olopack   (textures, meshes, etc.)
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
         */
        static bool CopyMonoRuntime(
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Copy the C# ScriptCore assembly
         */
        static bool CopyScriptCoreAssembly(
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
         * @brief Write the game manifest file with runtime configuration
         */
        static bool WriteGameManifest(
            const GameBuildSettings& settings,
            const std::filesystem::path& outputDir,
            std::string& errorMessage);

        /**
         * @brief Calculate total size of the output directory
         */
        static sizet CalculateDirectorySize(const std::filesystem::path& directory);
    };

} // namespace OloEngine
