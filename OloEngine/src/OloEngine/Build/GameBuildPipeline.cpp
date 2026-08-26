#include "OloEnginePCH.h"
#include "GameBuildPipeline.h"

#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Build/BuildPipelinePlatform.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/BackendSelection.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    bool StageRuntimeDependencyLibraries(
        BuildTargetPlatform targetPlatform,
        const std::filesystem::path& runtimeBinDir,
        const std::filesystem::path& outputDir,
        sizet& copiedCount,
        std::string& errorMessage)
    {
        copiedCount = 0;
        if (targetPlatform != BuildTargetPlatform::Windows)
        {
            return true;
        }

        std::error_code ec;
        std::vector<std::filesystem::path> dependencies;
        for (std::filesystem::directory_iterator it(runtimeBinDir, ec), end; it != end && !ec; it.increment(ec))
        {
            const bool isRegularFile = it->is_regular_file(ec);
            if (ec)
            {
                break;
            }
            if (!isRegularFile)
            {
                continue;
            }

            auto extension = it->path().extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            if (extension == ".dll")
            {
                dependencies.push_back(it->path());
            }
        }
        if (ec)
        {
            errorMessage = "Failed to enumerate runtime dependencies in " + runtimeBinDir.string() + ": " + ec.message();
            return false;
        }

        std::ranges::sort(dependencies);
        for (const auto& srcDll : dependencies)
        {
            std::filesystem::copy_file(srcDll, outputDir / srcDll.filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errorMessage = "Failed to copy runtime dependency " + srcDll.filename().string() + ": " + ec.message();
                return false;
            }
            ++copiedCount;
        }

        return true;
    }

    bool StageLooseRuntimeTextures(
        const std::filesystem::path& projectAssetsDir,
        const std::filesystem::path& outputAssetsDir,
        sizet& copiedCount,
        std::string& errorMessage)
    {
        copiedCount = 0;

        static const std::unordered_set<std::string> textureExtensions = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp"
        };

        std::error_code ec;
        std::vector<std::filesystem::path> textures;
        for (std::filesystem::recursive_directory_iterator it(projectAssetsDir, ec), end;
             it != end && !ec; it.increment(ec))
        {
            const bool isRegularFile = it->is_regular_file(ec);
            if (ec)
            {
                break;
            }
            if (!isRegularFile)
            {
                continue;
            }

            auto extension = it->path().extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            if (textureExtensions.contains(extension))
            {
                textures.push_back(it->path());
            }
        }
        if (ec)
        {
            errorMessage = "Failed to enumerate loose project textures in " + projectAssetsDir.string() + ": " + ec.message();
            return false;
        }

        std::ranges::sort(textures);
        for (const auto& texture : textures)
        {
            const auto relative = std::filesystem::relative(texture, projectAssetsDir, ec);
            if (ec || relative.empty() || relative.generic_string().starts_with(".."))
            {
                errorMessage = "Failed to make project texture path relative: " + texture.string();
                return false;
            }

            const auto destination = outputAssetsDir / relative;
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (ec)
            {
                errorMessage = "Failed to create runtime texture directory: " + ec.message();
                return false;
            }

            std::filesystem::copy_file(texture, destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errorMessage = "Failed to copy loose runtime texture " + relative.generic_string() + ": " + ec.message();
                return false;
            }
            ++copiedCount;
        }

        return true;
    }

    GameBuildResult GameBuildPipeline::Build(
        const GameBuildSettings& settings,
        std::atomic<f32>& progress,
        const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        auto startTime = std::chrono::high_resolution_clock::now();

        GameBuildResult result;
        progress = 0.0f;

        // Fail loudly, before touching the filesystem, when asked for a target
        // this host cannot produce (#891). OloEngine has no cross-compilation
        // toolchain — every step below copies the HOST's own OloRuntime binary,
        // Mono runtime and script assemblies, so packaging for another platform
        // would produce a folder that looks complete but cannot run.
        if (!IsBuildTargetSupportedOnThisHost(settings.TargetPlatform))
        {
            result.ErrorMessage = "Cannot build for target platform '" + std::string(ToString(settings.TargetPlatform)) +
                                  "' on this host (host platform: " + ToString(GetHostBuildPlatform()) +
                                  "). OloEngine has no cross-compilation toolchain — build on a " +
                                  ToString(settings.TargetPlatform) + " host to produce a " +
                                  ToString(settings.TargetPlatform) + " distribution.";
            return result;
        }

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
        if (IsScriptingAvailableOnPlatform(settings.TargetPlatform))
        {
            std::filesystem::create_directories(outputDir / "mono" / "lib", ec);
            std::filesystem::create_directories(outputDir / "mono" / "etc", ec);
            std::filesystem::create_directories(outputDir / "Resources" / "Scripts", ec);
        }

        result.OutputPath = outputDir;

        // Ship the renderer-backend config next to the exe (#691): the
        // engine's backend selection reads `config/renderer.yaml` (cwd first,
        // then the executable's directory), so this is what makes a packaged
        // game start on the developer's chosen default without a command-line
        // flag. Written through the same helper the editor dropdown uses — one
        // schema owner, no drift.
        {
            const bool wantVulkan = [&settings]
            {
                std::string lowered = settings.DefaultRendererBackend;
                std::ranges::transform(lowered, lowered.begin(), [](unsigned char c)
                                       { return static_cast<char>(std::tolower(c)); });
                return lowered == "vulkan";
            }();
            const auto rendererConfig = outputDir / "config" / "renderer.yaml";
            if (!WriteRendererConfig(rendererConfig,
                                     wantVulkan ? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL))
            {
                result.ErrorMessage = "Failed to write " + rendererConfig.string();
                return result;
            }
        }

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

        // Step 3b: Platform-specific executable finishing touches (#891) —
        // icon embedding on Windows, a .desktop launcher entry on Linux.
        // Non-fatal either way.
        {
            const std::filesystem::path destExe = outputDir / GetHostExecutableFileName(settings.GameName, settings.TargetPlatform);
            if (settings.TargetPlatform == BuildTargetPlatform::Windows)
            {
                if (!settings.IconPath.empty())
                {
                    OLO_CORE_INFO("[GameBuild] Embedding custom icon...");
                    std::string iconError;
                    if (!EmbedCustomIcon(destExe, settings.IconPath, iconError))
                    {
                        OLO_CORE_WARN("[GameBuild] Custom icon embedding failed (non-fatal): {}", iconError);
                    }
                }
            }
            else
            {
                OLO_CORE_INFO("[GameBuild] Writing Linux desktop entry...");
                std::string desktopError;
                if (!WriteLinuxDesktopEntry(destExe, settings.IconPath, settings.GameName, desktopError))
                {
                    OLO_CORE_WARN("[GameBuild] Desktop entry generation failed (non-fatal): {}", desktopError);
                }
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
        if (!CopyMonoRuntime(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.88f;

        // Step 7: Copy ScriptCore assembly (88% -> 90%)
        OLO_CORE_INFO("[GameBuild] Step 7/9: Copying ScriptCore assembly...");
        if (!CopyScriptCoreAssembly(settings, outputDir, result.ErrorMessage))
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

        // Step 8b: Copy loose Lua script files. Non-fatal — a game with no Lua
        // scripts is perfectly normal, and a failure here shouldn't sink an
        // otherwise-complete build.
        {
            std::string scriptError;
            if (!CopyScriptFiles(outputDir, scriptError))
            {
                OLO_CORE_WARN("[GameBuild] Lua script copy failed (non-fatal): {}", scriptError);
            }
        }

        // Step 8c: Copy writable project runtime configuration. Input actions
        // stay loose (rather than inside the immutable asset pack) because the
        // in-game rebind panel persists back to this same path.
        if (!StageProjectRuntimeFiles(outputDir, result.ErrorMessage))
        {
            return result;
        }

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

        if (auto project = Project::GetActive(); !project)
        {
            errorMessage = "No active project. Open a project in the editor first.";
            return false;
        }

        if (auto assetManager = Project::GetAssetManager(); !assetManager)
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

        // Locate the OloRuntime executable based on build configuration and
        // target platform. The binary is at: bin/{Config}/OloRuntime/<name>
        // — <name> carries the host's native extension (.exe on Windows,
        // none on Linux); Build() already refused any target that isn't this
        // host's own platform, so that's the only convention that applies.
        const auto& startupDir = Application::GetStartupWorkingDirectory();
        std::filesystem::path engineRoot = startupDir.parent_path();

        const std::string runtimeExeName = GetHostExecutableFileName("OloRuntime", settings.TargetPlatform);
        std::filesystem::path runtimeExe = engineRoot / "bin" / settings.BuildConfiguration / "OloRuntime" / runtimeExeName;

        if (!std::filesystem::exists(runtimeExe))
        {
            // Try relative to the workspace root (common in development)
            // The editor typically runs from OloEditor/, so engine root is one level up
            runtimeExe = engineRoot / ".." / "bin" / settings.BuildConfiguration / "OloRuntime" / runtimeExeName;

            if (!std::filesystem::exists(runtimeExe))
            {
                errorMessage = runtimeExeName + " not found. Build OloRuntime in " + settings.BuildConfiguration +
                               " configuration first. Expected at: " + runtimeExe.string();
                return false;
            }
        }

        // Warn if the runtime binary appears to be stale relative to the
        // editor binary. The CMake build ensures they stay in sync, but this
        // catches manual or partial builds where only one target was rebuilt.
        {
            const std::string editorExeName = GetHostExecutableFileName("OloEditor", settings.TargetPlatform);
            std::filesystem::path editorExe = engineRoot / "bin" / settings.BuildConfiguration / "OloEditor" / editorExeName;
            if (std::filesystem::exists(editorExe))
            {
                std::error_code tsEc;
                auto runtimeTime = std::filesystem::last_write_time(runtimeExe, tsEc);
                auto editorTime = std::filesystem::last_write_time(editorExe, tsEc);
                if (!tsEc && runtimeTime < editorTime)
                {
                    OLO_CORE_WARN("[GameBuild] {} is older than {} — "
                                  "it may be missing recent engine changes. "
                                  "Rebuild the OloRuntime target to ensure the game binary is up-to-date.",
                                  runtimeExeName, editorExeName);
                }
            }
        }

        // Copy and rename to the game name
        const std::filesystem::path destExe = outputDir / GetHostExecutableFileName(settings.GameName, settings.TargetPlatform);
        std::error_code ec;
        std::filesystem::copy_file(runtimeExe, destExe,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to copy runtime executable: " + ec.message();
            return false;
        }

        // Linux does not use a file extension to mark a file runnable —
        // copy_file does not reliably preserve the executable bit across
        // filesystems, so set it explicitly. Unlike the DLL/Mono/icon steps,
        // this one is fatal: an unrunnable binary is exactly the "folder that
        // looks fine and does not run" acceptance criterion #4 exists to catch.
        if (settings.TargetPlatform == BuildTargetPlatform::Linux)
        {
            std::filesystem::permissions(destExe,
                                         std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                         std::filesystem::perm_options::add, ec);
            if (ec)
            {
                errorMessage = "Failed to mark " + destExe.string() + " executable: " + ec.message();
                return false;
            }

            constexpr std::filesystem::perms execBits =
                std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
            const auto status = std::filesystem::status(destExe, ec);
            if (ec || (status.permissions() & execBits) == std::filesystem::perms::none)
            {
                errorMessage = "Copied runtime executable " + destExe.string() + " is not runnable — "
                                                                                 "no execute permission bit is set after copy.";
                return false;
            }
        }

        OLO_CORE_INFO("[GameBuild] Runtime executable copied: {}", destExe.string());
        return true;
    }

    bool GameBuildPipeline::CopyDependencyDLLs(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        [[maybe_unused]] std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // Every dependency the Linux runtime needs today is either statically
        // linked or resolved via the system's shared-library search path —
        // this step is Windows-shaped by name and function (#891). Kept as an
        // explicit early-return rather than falling through an empty Windows
        // DLL list so a future Linux runtime dependency has an obvious home.
        if (settings.TargetPlatform != BuildTargetPlatform::Windows)
        {
            OLO_CORE_INFO("[GameBuild] No dependency libraries to stage for {}", ToString(settings.TargetPlatform));
            return true;
        }

        // Resolve the runtime binary directory using the same logic as CopyRuntimeExecutable
        const auto& startupDir = Application::GetStartupWorkingDirectory();
        std::filesystem::path engineRoot = startupDir.parent_path();

        std::filesystem::path runtimeBinDir = engineRoot / "bin" / settings.BuildConfiguration / "OloRuntime";
        if (!std::filesystem::exists(runtimeBinDir))
        {
            // Fallback: editor runs from OloEditor/, engine root is one level up
            runtimeBinDir = engineRoot / ".." / "bin" / settings.BuildConfiguration / "OloRuntime";
        }

        sizet copiedCount = 0;
        if (!StageRuntimeDependencyLibraries(settings.TargetPlatform, runtimeBinDir, outputDir, copiedCount, errorMessage))
        {
            return false;
        }

        OLO_CORE_INFO("[GameBuild] Copied {} dependency DLLs", copiedCount);
        return true;
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
                ++shaderCount;
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
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // C# scripting is Windows-only — OloEngine-ScriptCore only builds
        // under the Visual Studio generator (#891) — so a non-Windows target
        // has no Mono runtime to ship. This is the honest answer, not a
        // silently incomplete build: Lua scripting is unaffected.
        if (!IsScriptingAvailableOnPlatform(settings.TargetPlatform))
        {
            OLO_CORE_INFO("[GameBuild] Skipping Mono runtime copy — C# scripting is not available on {}",
                          ToString(settings.TargetPlatform));
            return true;
        }

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
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // Same reasoning as CopyMonoRuntime: no ScriptCore assembly exists to
        // copy on a platform C# scripting doesn't run on (#891).
        if (!IsScriptingAvailableOnPlatform(settings.TargetPlatform))
        {
            OLO_CORE_INFO("[GameBuild] Skipping ScriptCore assembly copy — C# scripting is not available on {}",
                          ToString(settings.TargetPlatform));
            return true;
        }

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
        if (const auto& projectConfig = Project::GetActive()->GetConfig(); !projectConfig.ScriptModulePath.empty())
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

            ++copiedCount;
        }

        if (copiedCount == 0)
        {
            errorMessage = "No scene files (.olo) found in project asset directory";
            return false;
        }

        // Write the start scene path into the manifest
        // The project config may specify a start scene; otherwise fall back to the first found
        if (const auto& startScene = project->GetConfig().StartScene; !startScene.empty())
        {
            OLO_CORE_INFO("[GameBuild] Start scene from project config: {}", startScene.string());
        }

        OLO_CORE_INFO("[GameBuild] Copied {} scene file(s) to output", copiedCount);
        return true;
    }

    bool GameBuildPipeline::CopyScriptFiles(
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

        const auto assetDir = Project::GetAssetDirectory();

        // Scripts land under <game>/Assets/<asset-relative path> — NOT a
        // Scripts/ sibling like the scene copy uses. The runtime resolves a
        // LuaScriptComponent's project-relative ScriptFile through
        // Project::GetAssetFileSystemPath, so the relative layout under the
        // asset root has to survive the build byte for byte.
        const auto scriptOutputRoot = outputDir / "Assets";
        std::error_code ec;
        std::filesystem::create_directories(scriptOutputRoot, ec);

        // The traversal is wrapped because recursive_directory_iterator's
        // operator++ (and directory_entry::is_regular_file) throw
        // filesystem_error on an unreadable entry — an asset tree with one
        // permission-denied subfolder would otherwise throw straight out of
        // here. Build() runs on a detached FThread with no handler above it, so
        // that would std::terminate the whole editor rather than fail a build
        // step this caller deliberately treats as non-fatal.
        u32 copiedCount = 0;
        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(assetDir, ec))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".lua")
                {
                    continue;
                }

                auto relativePath = std::filesystem::relative(entry.path(), assetDir, ec);
                auto destPath = scriptOutputRoot / relativePath;

                std::filesystem::create_directories(destPath.parent_path(), ec);
                std::filesystem::copy_file(entry.path(), destPath,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    OLO_CORE_WARN("[GameBuild] Failed to copy script file {}: {}", relativePath.string(), ec.message());
                    ec.clear();
                    continue;
                }

                ++copiedCount;
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            // Loud, not silent: the shipped game is missing scripts it should
            // have had, and "the level does nothing" is a miserable way to find
            // that out later.
            OLO_CORE_ERROR("[GameBuild] Lua script scan of '{}' aborted after {} file(s): {}",
                           assetDir.string(), copiedCount, e.what());
            return true;
        }

        if (copiedCount == 0)
        {
            OLO_CORE_INFO("[GameBuild] No Lua script files found in project asset directory");
        }
        else
        {
            OLO_CORE_INFO("[GameBuild] Copied {} Lua script file(s) to output", copiedCount);
        }
        return true;
    }

    bool GameBuildPipeline::StageProjectRuntimeFiles(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        sizet copiedTextureCount = 0;
        if (!StageLooseRuntimeTextures(Project::GetAssetDirectory(), outputDir / "Assets",
                                       copiedTextureCount, errorMessage))
        {
            return false;
        }
        OLO_CORE_INFO("[GameBuild] Copied {} loose runtime texture file(s)", copiedTextureCount);

        const std::filesystem::path inputActionsSrc = Project::GetInputActionMapPath();
        std::error_code existsEc;
        const bool hasInputActions = std::filesystem::exists(inputActionsSrc, existsEc);
        if (existsEc)
        {
            errorMessage = "Failed to query project input-action config: " + existsEc.message();
            return false;
        }
        if (!hasInputActions)
        {
            OLO_CORE_INFO("[GameBuild] No project input-action config to copy");
            return true;
        }

        const std::filesystem::path inputActionsDst = outputDir / "Config" / "InputActions.yaml";
        std::error_code ec;
        std::filesystem::create_directories(inputActionsDst.parent_path(), ec);
        if (ec)
        {
            errorMessage = "Failed to create runtime Config directory: " + ec.message();
            return false;
        }

        std::filesystem::copy_file(inputActionsSrc, inputActionsDst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to copy project input actions: " + ec.message();
            return false;
        }

        OLO_CORE_INFO("[GameBuild] Copied writable input actions to {}", inputActionsDst.string());
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

        // Record the target explicitly, including whether C# scripting is
        // available on it (#891) — the honest answer, rather than a runtime
        // that silently discovers scripting is missing.
        out << YAML::Key << "Platform" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Target" << YAML::Value << ToString(settings.TargetPlatform);
        out << YAML::Key << "CSharpScriptingAvailable" << YAML::Value << IsScriptingAvailableOnPlatform(settings.TargetPlatform);
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
    bool GameBuildPipeline::EmbedCustomIcon(
        const std::filesystem::path& exePath,
        const std::filesystem::path& iconPath,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();
        return BuildPipelinePlatform::EmbedCustomIcon(exePath, iconPath, errorMessage);
    }

    bool GameBuildPipeline::WriteLinuxDesktopEntry(
        const std::filesystem::path& exePath,
        const std::filesystem::path& iconPath,
        const std::string& gameName,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();
        return BuildPipelinePlatform::WriteDesktopEntry(exePath, iconPath, gameName, errorMessage);
    }

} // namespace OloEngine
