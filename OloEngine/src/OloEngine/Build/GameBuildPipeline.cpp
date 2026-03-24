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

        // Validate and sanitize GameName before using it to build paths
        {
            const auto& gameName = settings.GameName;
            if (gameName.empty())
            {
                result.ErrorMessage = "GameName cannot be empty";
                return result;
            }
            std::filesystem::path nameAsPath(gameName);
            if (nameAsPath.is_absolute() || gameName.find("..") != std::string::npos || gameName.find('/') != std::string::npos || gameName.find('\\') != std::string::npos || nameAsPath.filename().string() != gameName)
            {
                result.ErrorMessage = "GameName contains invalid characters or path separators: " + gameName;
                return result;
            }
        }

        // Step 1: Validate project (5%)
        OLO_CORE_INFO("[GameBuild] Step 1/9: Validating project...");
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

        // Create output directory structure (clean staging)
        const std::filesystem::path outputDir = settings.OutputDirectory / settings.GameName;
        std::error_code ec;
        if (std::filesystem::exists(outputDir, ec))
        {
            std::filesystem::remove_all(outputDir, ec);
            if (ec)
            {
                result.ErrorMessage = "Failed to clean existing output directory: " + ec.message();
                return result;
            }
        }
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
        OLO_CORE_INFO("[GameBuild] Step 2/9: Building asset pack...");
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

        // Step 3: Copy runtime executable (55% -> 60%)
        OLO_CORE_INFO("[GameBuild] Step 3/9: Copying runtime executable...");
        if (!CopyRuntimeExecutable(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.60f;

        // Step 3b: Embed custom icon if specified (non-fatal)
        if (!settings.IconPath.empty())
        {
            OLO_CORE_INFO("[GameBuild] Embedding custom icon...");
            std::string iconError;
            const std::filesystem::path destExe = outputDir / (settings.GameName + ".exe");
            if (!EmbedCustomIcon(destExe, settings.IconPath, iconError))
            {
                OLO_CORE_WARN("[GameBuild] Custom icon embedding failed (non-fatal): {}", iconError);
            }
        }
        progress = 0.62f;

        if (cancelToken && cancelToken->load(std::memory_order_acquire))
        {
            result.ErrorMessage = "Build cancelled by user";
            return result;
        }

        // Step 4: Copy dependency DLLs (62% -> 68%)
        OLO_CORE_INFO("[GameBuild] Step 4/9: Copying dependency DLLs...");
        if (!CopyDependencyDLLs(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.68f;

        // Step 5: Copy engine resources — shaders, fonts (68% -> 80%)
        OLO_CORE_INFO("[GameBuild] Step 5/9: Copying engine resources...");
        if (!CopyEngineResources(outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.80f;

        // Step 6: Copy Mono runtime (80% -> 88%)
        OLO_CORE_INFO("[GameBuild] Step 6/9: Copying Mono runtime...");
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
        OLO_PROFILE_FUNCTION();

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
        OLO_PROFILE_FUNCTION();

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
        OLO_PROFILE_FUNCTION();

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

        // Warn if OloRuntime.exe appears to be stale relative to OloEditor.exe.
        // The CMake build ensures they stay in sync, but this catches manual or
        // partial builds where only one target was rebuilt.
        {
            std::filesystem::path editorExe = engineRoot / "bin" / settings.BuildConfiguration / "OloEditor" / "OloEditor.exe";
            if (std::filesystem::exists(editorExe))
            {
                std::error_code tsEc;
                auto runtimeTime = std::filesystem::last_write_time(runtimeExe, tsEc);
                auto editorTime = std::filesystem::last_write_time(editorExe, tsEc);
                if (!tsEc && runtimeTime < editorTime)
                {
                    OLO_CORE_WARN("[GameBuild] OloRuntime.exe is older than OloEditor.exe — "
                                  "it may be missing recent engine changes. "
                                  "Rebuild the OloRuntime target to ensure the game binary is up-to-date.");
                }
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
        OLO_PROFILE_FUNCTION();

        // Resolve the runtime binary directory using the same logic as CopyRuntimeExecutable
        const auto& startupDir = Application::GetStartupWorkingDirectory();
        std::filesystem::path engineRoot = startupDir.parent_path();

        std::filesystem::path runtimeBinDir = engineRoot / "bin" / settings.BuildConfiguration / "OloRuntime";
        if (!std::filesystem::exists(runtimeBinDir))
        {
            // Fallback: editor runs from OloEditor/, engine root is one level up
            runtimeBinDir = engineRoot / ".." / "bin" / settings.BuildConfiguration / "OloRuntime";
        }

        std::vector<std::string> requiredDlls = {
            "libpng16.dll",
            "libpng16d.dll", // Debug variant
            "zlib.dll",
            "zlibd.dll", // Debug variant
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
        OLO_PROFILE_FUNCTION();

        // Engine resources are located relative to the editor working directory.
        // The build pipeline runs from OloEditor/ cwd.
        const auto copyOpts = std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive;
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
        OLO_PROFILE_FUNCTION();

        // Mono runtime is expected relative to the working directory
        // In development: OloEditor/mono/
        const std::filesystem::path monoSrcDir = "mono";

        if (!std::filesystem::exists(monoSrcDir))
        {
            OLO_CORE_WARN("[GameBuild] Mono runtime directory not found at: {}", monoSrcDir.string());
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
        OLO_PROFILE_FUNCTION();

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
        OLO_PROFILE_FUNCTION();

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
            auto destPath = sceneOutputDir / relativePath;

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
        OLO_PROFILE_FUNCTION();

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
        OLO_PROFILE_FUNCTION();

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

#ifdef _WIN32
    bool GameBuildPipeline::EmbedCustomIcon(
        const std::filesystem::path& exePath,
        const std::filesystem::path& iconPath,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        if (!std::filesystem::exists(iconPath))
        {
            errorMessage = "Icon file not found: " + iconPath.string();
            return false;
        }

        // Read the entire .ico file into memory
        std::ifstream icoFile(iconPath, std::ios::binary | std::ios::ate);
        if (!icoFile.is_open())
        {
            errorMessage = "Failed to open icon file: " + iconPath.string();
            return false;
        }

        const auto fileSize = icoFile.tellg();
        if (fileSize < 6) // Minimum ICO header size
        {
            errorMessage = "Icon file is too small or corrupt";
            return false;
        }
        icoFile.seekg(0);

        std::vector<u8> icoData(static_cast<sizet>(fileSize));
        icoFile.read(reinterpret_cast<char*>(icoData.data()), fileSize);
        icoFile.close();

        // Parse ICO header: 6 bytes header + 16 bytes per entry
        // ICONDIR: Reserved(2) + Type(2) + Count(2)
        if (icoData.size() < 6)
        {
            errorMessage = "Invalid ICO file format";
            return false;
        }

        u16 imageCount = *reinterpret_cast<const u16*>(&icoData[4]);
        if (imageCount == 0 || icoData.size() < static_cast<sizet>(6 + imageCount * 16))
        {
            errorMessage = "Invalid ICO file: no images or truncated directory";
            return false;
        }

        // Open the executable for resource updates
        HANDLE hUpdate = ::BeginUpdateResourceW(exePath.wstring().c_str(), FALSE);
        if (!hUpdate)
        {
            errorMessage = "BeginUpdateResource failed (error " + std::to_string(::GetLastError()) + ")";
            return false;
        }

        // Build the RT_GROUP_ICON directory that references individual RT_ICON entries.
        // GRPICONDIR: Reserved(2) + Type(2) + Count(2) + GRPICONDIRENTRY[Count]
        // Each GRPICONDIRENTRY is 14 bytes (same as ICONDIRENTRY but with nID instead of dwImageOffset)
        const sizet grpSize = 6 + imageCount * 14;
        std::vector<u8> grpData(grpSize);
        std::memcpy(grpData.data(), icoData.data(), 6); // Copy header

        bool anyFailed = false;
        for (u16 i = 0; i < imageCount; ++i)
        {
            const sizet entryOffset = 6 + static_cast<sizet>(i) * 16;
            const u8* entry = &icoData[entryOffset];

            // ICONDIRENTRY: Width(1) Height(1) ColorCount(1) Reserved(1)
            //               Planes(2) BitCount(2) BytesInRes(4) ImageOffset(4)
            u32 bytesInRes = *reinterpret_cast<const u32*>(&entry[8]);
            u32 imageOffset = *reinterpret_cast<const u32*>(&entry[12]);

            if (static_cast<sizet>(imageOffset) + bytesInRes > icoData.size())
            {
                anyFailed = true;
                continue;
            }

            // Write individual RT_ICON resource (1-indexed ID)
            u16 iconId = static_cast<u16>(i + 1);
            if (!::UpdateResourceW(hUpdate, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(iconId),
                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                                   const_cast<u8*>(&icoData[imageOffset]), bytesInRes))
            {
                anyFailed = true;
            }

            // Build GRPICONDIRENTRY: copy first 12 bytes from ICONDIRENTRY, then nID(2)
            const sizet grpEntryOffset = 6 + static_cast<sizet>(i) * 14;
            std::memcpy(&grpData[grpEntryOffset], entry, 12);
            *reinterpret_cast<u16*>(&grpData[grpEntryOffset + 12]) = iconId;
        }

        // Write RT_GROUP_ICON resource (ID 1 — matches the .rc resource ID)
        if (!::UpdateResourceW(hUpdate, MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1),
                               MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                               grpData.data(), static_cast<DWORD>(grpData.size())))
        {
            anyFailed = true;
        }

        if (!::EndUpdateResourceW(hUpdate, FALSE))
        {
            errorMessage = "EndUpdateResource failed (error " + std::to_string(::GetLastError()) + ")";
            return false;
        }

        if (anyFailed)
        {
            OLO_CORE_WARN("[GameBuild] Some icon entries could not be embedded");
        }

        OLO_CORE_INFO("[GameBuild] Custom icon embedded: {} ({} image(s))", iconPath.filename().string(), imageCount);
        return true;
    }
#else
    bool GameBuildPipeline::EmbedCustomIcon(
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::string& errorMessage)
    {
        errorMessage = "Icon embedding is only supported on Windows";
        return false;
    }
#endif

} // namespace OloEngine
