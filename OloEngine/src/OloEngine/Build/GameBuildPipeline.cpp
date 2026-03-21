#include "OloEnginePCH.h"
#include "GameBuildPipeline.h"

#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Project/Project.h"

#include <chrono>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    GameBuildResult GameBuildPipeline::Build(
        const GameBuildSettings& settings,
        std::atomic<f32>& progress,
        const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        auto startTime = std::chrono::high_resolution_clock::now();

        GameBuildResult result;
        progress = 0.0f;

        // Step 1: Validate project (5%)
        OLO_CORE_INFO("[GameBuild] Step 1/8: Validating project...");
        if (!ValidateProject(result.ErrorMessage))
        {
            return result;
        }
        progress = 0.05f;

        if (cancelToken && cancelToken->load(std::memory_order_acquire))
        {
            result.ErrorMessage = "Build cancelled by user";
            return result;
        }

        // Create output directory structure
        const std::filesystem::path outputDir = settings.OutputDirectory / settings.GameName;
        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec)
        {
            result.ErrorMessage = "Failed to create output directory: " + ec.message();
            return result;
        }
        std::filesystem::create_directories(outputDir / "Assets", ec);
        std::filesystem::create_directories(outputDir / "mono" / "lib", ec);
        std::filesystem::create_directories(outputDir / "mono" / "etc", ec);
        std::filesystem::create_directories(outputDir / "Resources" / "Scripts", ec);

        result.OutputPath = outputDir;

        // Step 2: Build asset pack (5% -> 55%)
        OLO_CORE_INFO("[GameBuild] Step 2/8: Building asset pack...");
        if (!BuildAssetPack(settings, outputDir, result.AssetCount, result.SceneCount, progress, cancelToken))
        {
            if (result.ErrorMessage.empty())
            {
                result.ErrorMessage = "Asset pack build failed";
            }
            return result;
        }
        progress = 0.55f;

        if (cancelToken && cancelToken->load(std::memory_order_acquire))
        {
            result.ErrorMessage = "Build cancelled by user";
            return result;
        }

        // Step 3: Copy runtime executable (55% -> 62%)
        OLO_CORE_INFO("[GameBuild] Step 3/8: Copying runtime executable...");
        if (!CopyRuntimeExecutable(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.62f;

        if (cancelToken && cancelToken->load(std::memory_order_acquire))
        {
            result.ErrorMessage = "Build cancelled by user";
            return result;
        }

        // Step 4: Copy dependency DLLs (62% -> 68%)
        OLO_CORE_INFO("[GameBuild] Step 4/8: Copying dependency DLLs...");
        if (!CopyDependencyDLLs(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.68f;

        // Step 5: Copy engine resources — shaders, fonts (68% -> 80%)
        OLO_CORE_INFO("[GameBuild] Step 5/8: Copying engine resources...");
        if (!CopyEngineResources(outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.80f;

        // Step 6: Copy Mono runtime (80% -> 88%)
        OLO_CORE_INFO("[GameBuild] Step 6/8: Copying Mono runtime...");
        if (!CopyMonoRuntime(outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.88f;

        // Step 7: Copy ScriptCore assembly (88% -> 90%)
        OLO_CORE_INFO("[GameBuild] Step 7/9: Copying ScriptCore assembly...");
        if (!CopyScriptCoreAssembly(outputDir, result.ErrorMessage))
        {
            // Not fatal — game may not use C# scripts
            OLO_CORE_WARN("[GameBuild] ScriptCore copy failed (non-fatal): {}", result.ErrorMessage);
            result.ErrorMessage.clear();
        }
        progress = 0.90f;

        // Step 8: Copy scene files (90% -> 95%)
        OLO_CORE_INFO("[GameBuild] Step 8/9: Copying scene files...");
        if (!CopySceneFiles(outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.95f;

        // Step 9: Write game manifest (95% -> 100%)
        OLO_CORE_INFO("[GameBuild] Step 9/9: Writing game manifest...");
        if (!WriteGameManifest(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }

        // Calculate total size
        result.TotalSizeBytes = CalculateDirectorySize(outputDir);

        auto endTime = std::chrono::high_resolution_clock::now();
        result.BuildTimeSeconds = std::chrono::duration<f64>(endTime - startTime).count();
        result.Success = true;
        progress = 1.0f;

        OLO_CORE_INFO("[GameBuild] Build completed successfully in {:.1f}s", result.BuildTimeSeconds);
        OLO_CORE_INFO("[GameBuild]   Output: {}", outputDir.string());
        OLO_CORE_INFO("[GameBuild]   Assets: {}, Scenes: {}", result.AssetCount, result.SceneCount);
        OLO_CORE_INFO("[GameBuild]   Total size: {:.1f} MB", static_cast<f64>(result.TotalSizeBytes) / (1024.0 * 1024.0));

        return result;
    }

    bool GameBuildPipeline::ValidateProject(std::string& errorMessage)
    {
        auto project = Project::GetActive();
        if (!project)
        {
            errorMessage = "No active project. Open a project in the editor first.";
            return false;
        }

        auto assetManager = Project::GetAssetManager();
        if (!assetManager)
        {
            errorMessage = "No asset manager available.";
            return false;
        }

        return true;
    }

    bool GameBuildPipeline::BuildAssetPack(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        sizet& assetCount,
        sizet& sceneCount,
        std::atomic<f32>& progress,
        const std::atomic<bool>* cancelToken)
    {
        AssetPackBuilder::BuildSettings packSettings;
        packSettings.m_OutputPath = outputDir / "Assets" / "AssetPack.olopack";
        packSettings.m_CompressAssets = settings.CompressAssets;
        packSettings.m_IncludeScriptModule = settings.IncludeScriptModule;
        packSettings.m_ValidateAssets = settings.ValidateAssets;

        // The pack builder reports 0.0-1.0 progress; we map it to 0.05-0.60
        std::atomic<f32> packProgress = 0.0f;

        auto buildResult = AssetPackBuilder::BuildFromActiveProject(packSettings, packProgress, cancelToken);

        // Map final pack progress to our overall progress
        progress = 0.05f + (packProgress.load() * 0.55f);

        if (!buildResult.m_Success)
        {
            OLO_CORE_ERROR("[GameBuild] Asset pack build failed: {}", buildResult.m_ErrorMessage);
            return false;
        }

        assetCount = buildResult.m_AssetCount;
        sceneCount = buildResult.m_SceneCount;

        OLO_CORE_INFO("[GameBuild] Asset pack created: {} assets, {} scenes",
                      assetCount, sceneCount);
        return true;
    }

    bool GameBuildPipeline::CopyRuntimeExecutable(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        // Locate the OloRuntime executable based on build configuration
        // The binary is at: bin/{Config}/OloRuntime/OloRuntime.exe
        const auto& startupDir = Application::GetStartupWorkingDirectory();
        std::filesystem::path engineRoot = startupDir.parent_path();

        std::filesystem::path runtimeExe = engineRoot / "bin" / settings.BuildConfiguration / "OloRuntime" / "OloRuntime.exe";

        if (!std::filesystem::exists(runtimeExe))
        {
            // Try relative to the workspace root (common in development)
            // The editor typically runs from OloEditor/, so engine root is one level up
            runtimeExe = engineRoot / ".." / "bin" / settings.BuildConfiguration / "OloRuntime" / "OloRuntime.exe";

            if (!std::filesystem::exists(runtimeExe))
            {
                errorMessage = "OloRuntime.exe not found. Build OloRuntime in " + settings.BuildConfiguration +
                               " configuration first. Expected at: " + runtimeExe.string();
                return false;
            }
        }

        // Copy and rename to the game name
        const std::filesystem::path destExe = outputDir / (settings.GameName + ".exe");
        std::error_code ec;
        std::filesystem::copy_file(runtimeExe, destExe,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to copy runtime executable: " + ec.message();
            return false;
        }

        OLO_CORE_INFO("[GameBuild] Runtime executable copied: {}", destExe.string());
        return true;
    }

    bool GameBuildPipeline::CopyDependencyDLLs(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        // DLLs live next to OloRuntime.exe in bin/{Config}/OloRuntime/
        const auto& startupDir = Application::GetStartupWorkingDirectory();
        std::filesystem::path runtimeBinDir = startupDir.parent_path() / "bin" / settings.BuildConfiguration / "OloRuntime";

        std::vector<std::string> requiredDlls = {
            "libpng16.dll",
            "libpng16d.dll",  // Debug variant
            "zlib.dll",
            "zlibd.dll",      // Debug variant
        };

        sizet copiedCount = 0;
        for (const auto& dllName : requiredDlls)
        {
            std::filesystem::path srcDll = runtimeBinDir / dllName;
            if (!std::filesystem::exists(srcDll))
            {
                continue; // Optional — not all variants exist (debug vs release)
            }

            std::error_code ec;
            std::filesystem::copy_file(srcDll, outputDir / dllName,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                OLO_CORE_WARN("[GameBuild] Failed to copy DLL {}: {}", dllName, ec.message());
            }
            else
            {
                copiedCount++;
            }
        }

        OLO_CORE_INFO("[GameBuild] Copied {} dependency DLLs", copiedCount);
        return true; // DLL copy failures are non-fatal warnings
    }

    bool GameBuildPipeline::CopyEngineResources(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        // Engine resources are located relative to the editor working directory.
        // The build pipeline runs from OloEditor/ cwd.
        const auto copyOpts = std::filesystem::copy_options::overwrite_existing
                            | std::filesystem::copy_options::recursive;
        std::error_code ec;

        // --- Shaders (required — renderer will fail without them) ---
        const std::filesystem::path shaderSrc = "assets/shaders";
        const std::filesystem::path shaderDst = outputDir / "assets" / "shaders";
        if (!std::filesystem::exists(shaderSrc))
        {
            errorMessage = "Engine shaders not found at: " + std::filesystem::absolute(shaderSrc).string();
            return false;
        }

        std::filesystem::create_directories(shaderDst, ec);
        std::filesystem::copy(shaderSrc, shaderDst, copyOpts, ec);
        if (ec)
        {
            errorMessage = "Failed to copy engine shaders: " + ec.message();
            return false;
        }

        sizet shaderCount = 0;
        for ([[maybe_unused]] const auto& entry : std::filesystem::recursive_directory_iterator(shaderDst))
        {
            if (entry.is_regular_file())
            {
                shaderCount++;
            }
        }
        OLO_CORE_INFO("[GameBuild] Copied {} shader files", shaderCount);

        // --- Textures (skybox cubemaps, IBL, etc.) ---
        const std::filesystem::path textureSrc = "assets/textures";
        const std::filesystem::path textureDst = outputDir / "assets" / "textures";
        if (std::filesystem::exists(textureSrc))
        {
            std::filesystem::create_directories(textureDst, ec);
            std::filesystem::copy(textureSrc, textureDst, copyOpts, ec);
            if (ec)
            {
                OLO_CORE_WARN("[GameBuild] Failed to copy textures: {}", ec.message());
                ec.clear();
            }
            else
            {
                OLO_CORE_INFO("[GameBuild] Textures copied");
            }
        }

        // --- Fonts (required for text rendering) ---
        const std::filesystem::path fontSrc = "assets/fonts";
        const std::filesystem::path fontDst = outputDir / "assets" / "fonts";
        if (std::filesystem::exists(fontSrc))
        {
            std::filesystem::create_directories(fontDst, ec);
            std::filesystem::copy(fontSrc, fontDst, copyOpts, ec);
            if (ec)
            {
                OLO_CORE_WARN("[GameBuild] Failed to copy fonts: {}", ec.message());
                // Non-fatal: text rendering falls back to built-in font
            }
            else
            {
                OLO_CORE_INFO("[GameBuild] Fonts copied");
            }
        }

        return true;
    }

    bool GameBuildPipeline::CopyMonoRuntime(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        // Mono runtime is expected relative to the working directory
        // In development: OloEditor/mono/
        const std::filesystem::path monoSrcDir = "mono";

        if (!std::filesystem::exists(monoSrcDir))
        {
            OLO_CORE_WARN("[GameBuild] Mono runtime directory not found at: {}", monoSrcDir.string());
            errorMessage = "Mono runtime not found. C# scripting may not work in the built game.";
            return true; // Non-fatal — game might not use C# scripts
        }

        // Copy mono/lib/
        const std::filesystem::path monoLibSrc = monoSrcDir / "lib";
        const std::filesystem::path monoLibDst = outputDir / "mono" / "lib";
        if (std::filesystem::exists(monoLibSrc))
        {
            std::error_code ec;
            std::filesystem::copy(monoLibSrc, monoLibDst,
                                  std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive, ec);
            if (ec)
            {
                errorMessage = "Failed to copy Mono lib directory: " + ec.message();
                return false;
            }
        }

        // Copy mono/etc/
        const std::filesystem::path monoEtcSrc = monoSrcDir / "etc";
        const std::filesystem::path monoEtcDst = outputDir / "mono" / "etc";
        if (std::filesystem::exists(monoEtcSrc))
        {
            std::error_code ec;
            std::filesystem::copy(monoEtcSrc, monoEtcDst,
                                  std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive, ec);
            if (ec)
            {
                errorMessage = "Failed to copy Mono etc directory: " + ec.message();
                return false;
            }
        }

        OLO_CORE_INFO("[GameBuild] Mono runtime copied");
        return true;
    }

    bool GameBuildPipeline::CopyScriptCoreAssembly(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        // The ScriptCore DLL is at Resources/Scripts/OloEngine-ScriptCore.dll
        const std::filesystem::path scriptCoreSrc = "Resources/Scripts/OloEngine-ScriptCore.dll";

        if (!std::filesystem::exists(scriptCoreSrc))
        {
            errorMessage = "OloEngine-ScriptCore.dll not found at: " + scriptCoreSrc.string();
            return false;
        }

        const std::filesystem::path scriptCoreDst = outputDir / "Resources" / "Scripts" / "OloEngine-ScriptCore.dll";

        std::error_code ec;
        std::filesystem::create_directories(scriptCoreDst.parent_path(), ec);
        std::filesystem::copy_file(scriptCoreSrc, scriptCoreDst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to copy ScriptCore assembly: " + ec.message();
            return false;
        }

        // Also copy the app-specific script assembly if it exists
        const auto& projectConfig = Project::GetActive()->GetConfig();
        if (!projectConfig.ScriptModulePath.empty())
        {
            std::filesystem::path appScriptSrc = projectConfig.ScriptModulePath;
            if (std::filesystem::exists(appScriptSrc))
            {
                // Runtime looks for the assembly at Resources/Scripts/<filename>
                std::filesystem::path appScriptDst = outputDir / "Resources" / "Scripts" / appScriptSrc.filename();
                std::filesystem::create_directories(appScriptDst.parent_path(), ec);
                std::filesystem::copy_file(appScriptSrc, appScriptDst,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    OLO_CORE_WARN("[GameBuild] Failed to copy app script assembly: {}", ec.message());
                }
                else
                {
                    OLO_CORE_INFO("[GameBuild] App script assembly copied: {}", appScriptDst.string());
                }
            }
        }

        OLO_CORE_INFO("[GameBuild] ScriptCore assembly copied");
        return true;
    }

    bool GameBuildPipeline::CopySceneFiles(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        auto project = Project::GetActive();
        if (!project)
        {
            errorMessage = "No active project";
            return false;
        }

        // Scenes live under the project asset directory as .olo files
        const auto assetDir = Project::GetAssetDirectory();

        // Create Scenes/ directory in the output
        const auto sceneOutputDir = outputDir / "Scenes";
        std::error_code ec;
        std::filesystem::create_directories(sceneOutputDir, ec);

        // Find all .olo scene files recursively in the asset directory
        u32 copiedCount = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assetDir, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            if (entry.path().extension() != ".olo")
            {
                continue;
            }

            // Preserve relative path from asset directory
            auto relativePath = std::filesystem::relative(entry.path(), assetDir, ec);
            auto destPath = outputDir / relativePath;

            std::filesystem::create_directories(destPath.parent_path(), ec);
            std::filesystem::copy_file(entry.path(), destPath,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                OLO_CORE_WARN("[GameBuild] Failed to copy scene file {}: {}", relativePath.string(), ec.message());
                ec.clear();
                continue;
            }

            copiedCount++;
        }

        if (copiedCount == 0)
        {
            errorMessage = "No scene files (.olo) found in project asset directory";
            return false;
        }

        // Write the start scene path into the manifest
        // The project config may specify a start scene; otherwise fall back to the first found
        const auto& startScene = project->GetConfig().StartScene;
        if (!startScene.empty())
        {
            OLO_CORE_INFO("[GameBuild] Start scene from project config: {}", startScene.string());
        }

        OLO_CORE_INFO("[GameBuild] Copied {} scene file(s) to output", copiedCount);
        return true;
    }

    bool GameBuildPipeline::WriteGameManifest(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        const std::filesystem::path manifestPath = outputDir / "game.manifest";

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Game" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << settings.GameName;
        out << YAML::Key << "EngineVersion" << YAML::Value << "0.0.1";
        out << YAML::EndMap;

        out << YAML::Key << "Assets" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "PackFile" << YAML::Value << "Assets/AssetPack.olopack";
        out << YAML::Key << "SceneDirectory" << YAML::Value << "Scenes";
        out << YAML::EndMap;

        out << YAML::Key << "Rendering" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Is3DMode" << YAML::Value << settings.Is3DMode;
        out << YAML::EndMap;

        // Write start scene from build settings (asset-relative path)
        if (!settings.StartScene.empty())
        {
            // The StartScene in settings is relative to the project asset dir (e.g. "Scenes/MyScene.olo").
            // The runtime expects a path relative to the game output dir (e.g. "Scenes/MyScene.olo").
            out << YAML::Key << "StartScene" << YAML::Value << settings.StartScene.generic_string();
        }
        else
        {
            // Fallback: try project config, converting absolute to relative
            auto project = Project::GetActive();
            if (project && !project->GetConfig().StartScene.empty())
            {
                std::error_code ec;
                auto relative = std::filesystem::relative(
                    project->GetConfig().StartScene, Project::GetAssetDirectory(), ec);
                if (!ec && !relative.empty())
                {
                    out << YAML::Key << "StartScene" << YAML::Value << relative.generic_string();
                }
                else
                {
                    // Last resort: just the filename under Scenes/
                    out << YAML::Key << "StartScene" << YAML::Value
                        << ("Scenes/" + project->GetConfig().StartScene.filename().string());
                }
            }
        }

        out << YAML::EndMap;

        std::ofstream fout(manifestPath);
        if (!fout.is_open())
        {
            errorMessage = "Failed to create game manifest file: " + manifestPath.string();
            return false;
        }
        fout << out.c_str();
        fout.close();

        OLO_CORE_INFO("[GameBuild] Game manifest written: {}", manifestPath.string());
        return true;
    }

    sizet GameBuildPipeline::CalculateDirectorySize(const std::filesystem::path& directory)
    {
        sizet totalSize = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec))
        {
            if (entry.is_regular_file())
            {
                totalSize += entry.file_size(ec);
            }
        }
        return totalSize;
    }

} // namespace OloEngine
