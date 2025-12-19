#include "OloEnginePCH.h"

#include <OloEngine/Asset/AssetPackBuilder.h>
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <unordered_set>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

namespace OloEngine
{
    AssetPackBuilder::BuildResult AssetPackBuilder::BuildFromActiveProject(const BuildSettings& settings, std::atomic<f32>& progress, const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        auto project = Project::GetActive();
        if (!project)
        {
            OLO_CORE_ERROR("AssetPackBuilder::BuildFromActiveProject - No active project");
            return { false, "No active project loaded", 0, 0, {} };
        }

        auto assetManager = Project::GetAssetManager();
        if (!assetManager)
        {
            OLO_CORE_ERROR("AssetPackBuilder::BuildFromActiveProject - No asset manager available");
            return { false, "No asset manager available", 0, 0, {} };
        }

        OLO_CORE_INFO("AssetPackBuilder: Starting asset pack build from active project");
        return BuildImpl(assetManager, settings, progress, cancelToken);
    }

    AssetPackBuilder::BuildResult AssetPackBuilder::BuildFromRegistry(const AssetRegistry& assetRegistry, const BuildSettings& settings, std::atomic<f32>& progress, const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_INFO("AssetPackBuilder: Starting asset pack build from provided registry");

        BuildResult result;
        result.m_OutputPath = settings.m_OutputPath;

        try
        {
            // Check for cancellation before starting
            if (cancelToken && cancelToken->load(std::memory_order_acquire))
            {
                OLO_CORE_INFO("AssetPackBuilder: Build cancelled before starting");
                return { false, "Build cancelled by user", 0, 0, {} };
            }

            // Create a temporary EditorAssetManager to handle the assets from the registry
            auto tempAssetManager = Ref<EditorAssetManager>::Create();
            tempAssetManager->Initialize();

            // Load all assets from the provided registry
            auto allAssets = assetRegistry.GetAllAssets();
            OLO_CORE_INFO("AssetPackBuilder: Loading {} assets from registry", allAssets.size());

            // Set up progress tracking for asset loading
            float loadProgress = 0.0f;
            float progressPerAsset = 0.3f / static_cast<float>(allAssets.size()); // Reserve 30% for loading

            sizet loadedCount = 0;
            sizet failedCount = 0;

            // Load each asset from the registry into the temporary manager
            for (const auto& metadata : allAssets)
            {
                // Check for cancellation during loading
                if (cancelToken && cancelToken->load(std::memory_order_acquire))
                {
                    OLO_CORE_INFO("AssetPackBuilder: Build cancelled during asset loading");
                    tempAssetManager->Shutdown();
                    return { false, "Build cancelled by user", 0, 0, {} };
                }

                try
                {
                    // Add the metadata to the temporary manager's registry
                    tempAssetManager->SetMetadata(metadata.Handle, metadata);

                    // Load the asset data
                    auto asset = tempAssetManager->GetAsset(metadata.Handle);
                    if (asset)
                    {
                        loadedCount++;
                        OLO_CORE_TRACE("AssetPackBuilder: Loaded asset {} ({})", metadata.Handle, metadata.FilePath.string());
                    }
                    else
                    {
                        failedCount++;
                        OLO_CORE_WARN("AssetPackBuilder: Failed to load asset {} ({})", metadata.Handle, metadata.FilePath.string());
                    }
                }
                catch (const std::exception& e)
                {
                    failedCount++;
                    OLO_CORE_ERROR("AssetPackBuilder: Exception loading asset {} ({}): {}", metadata.Handle, metadata.FilePath.string(), e.what());
                }

                // Update progress
                loadProgress += progressPerAsset;
                progress = loadProgress;
            }

            OLO_CORE_INFO("AssetPackBuilder: Loaded {}/{} assets successfully ({} failed)", loadedCount, allAssets.size(), failedCount);

            // Check if we have any loaded assets to pack
            if (loadedCount == 0)
            {
                tempAssetManager->Shutdown();
                return { false, "No assets could be loaded from the registry", 0, 0, {} };
            }

            // Check for cancellation before starting the build
            if (cancelToken && cancelToken->load(std::memory_order_acquire))
            {
                OLO_CORE_INFO("AssetPackBuilder: Build cancelled before packing");
                tempAssetManager->Shutdown();
                return { false, "Build cancelled by user", 0, 0, {} };
            }

            // Use the existing BuildImpl with the temporary asset manager
            // The progress will start from 0.3 (30% for loading) and go to 1.0

            // Create a wrapper to update our main progress from BuildImpl's progress
            std::atomic<f32> internalProgress = 0.0f;
            std::atomic<bool> progressUpdateActive = true;
            
            // Start a simple progress forwarding
            auto updateProgress = [&]() {
                while (progressUpdateActive.load())
                {
                    float internal = internalProgress.load();
                    progress = 0.3f + (internal * 0.7f);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            };
            std::thread progressThread(updateProgress);

            // Call the existing implementation with the populated temporary manager
            result = BuildImpl(tempAssetManager, settings, internalProgress, cancelToken);

            // Stop progress thread and finalize
            progressUpdateActive = false;
            if (progressThread.joinable())
                progressThread.join();

            // Clean up the temporary asset manager
            tempAssetManager->Shutdown();

            // Update final progress
            progress = result.m_Success ? 1.0f : internalProgress.load();

            return result;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetPackBuilder::BuildFromRegistry - Exception: {}", e.what());
            return { false, "Exception during build: " + std::string(e.what()), 0, 0, {} };
        }
        catch (...)
        {
            OLO_CORE_ERROR("AssetPackBuilder::BuildFromRegistry - Unknown exception");
            return { false, "Unknown exception during build", 0, 0, {} };
        }
    }

    AssetPackBuilder::BuildResult AssetPackBuilder::BuildImpl(Ref<AssetManagerBase> assetManager, const BuildSettings& settings, std::atomic<f32>& progress, const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        progress = 0.0f;

        BuildResult result;
        result.m_OutputPath = settings.m_OutputPath;

        try
        {
            // Check for cancellation before starting
            if (cancelToken && cancelToken->load(std::memory_order_acquire))
            {
                OLO_CORE_INFO("AssetPackBuilder: Build cancelled before starting");
                return { false, "Build cancelled by user", 0, 0, {} };
            }

            // Validate assets if requested
            if (settings.m_ValidateAssets)
            {
                OLO_CORE_INFO("AssetPackBuilder: Validating assets...");
                if (!ValidateAssets(assetManager))
                {
                    result.m_ErrorMessage = "Asset validation failed";
                    return result;
                }
                progress = 0.1f;
                
                // Check for cancellation after validation
                if (cancelToken && cancelToken->load(std::memory_order_acquire))
                {
                    OLO_CORE_INFO("AssetPackBuilder: Build cancelled after validation");
                    return { false, "Build cancelled by user", 0, 0, {} };
                }
            }

            // Create output directory if it doesn't exist
            std::filesystem::create_directories(settings.m_OutputPath.parent_path());

            // Initialize asset pack file structure
            AssetPackFile assetPackFile;
            
            // Serialize all assets
            OLO_CORE_INFO("AssetPackBuilder: Serializing assets...");
            if (!SerializeAllAssets(assetManager, assetPackFile, progress, cancelToken))
            {
                result.m_ErrorMessage = "Failed to serialize assets or build was cancelled";
                return result;
            }

            progress = 0.8f;

            // Check for cancellation after serialization
            if (cancelToken && cancelToken->load(std::memory_order_acquire))
            {
                OLO_CORE_INFO("AssetPackBuilder: Build cancelled after asset serialization");
                return { false, "Build cancelled by user", 0, 0, {} };
            }

            // Get script module binary if requested
            Buffer scriptModuleBinary;
            if (settings.m_IncludeScriptModule)
            {
                scriptModuleBinary = GetScriptModuleBinary();
                OLO_CORE_INFO("AssetPackBuilder: Script module binary size: {} bytes", scriptModuleBinary.Size);
            }

            progress = 0.9f;

            // Serialize the pack to file
            OLO_CORE_INFO("AssetPackBuilder: Writing asset pack to: {}", settings.m_OutputPath.string());
            
            FileStreamWriter writer(settings.m_OutputPath);
            if (!writer.IsStreamGood())
            {
                result.m_ErrorMessage = "Failed to create output file: " + settings.m_OutputPath.string();
                return result;
            }

            // Write header
            writer.WriteRaw(assetPackFile.Header.MagicNumber);
            writer.WriteRaw(assetPackFile.Header.Version);
            writer.WriteRaw(assetPackFile.Header.BuildVersion);
            writer.WriteRaw(assetPackFile.Header.IndexOffset);

            // Write index
            writer.WriteRaw(assetPackFile.Index.AssetCount);
            writer.WriteRaw(assetPackFile.Index.SceneCount);
            writer.WriteRaw(assetPackFile.Index.PackedAppBinaryOffset);
            writer.WriteRaw(assetPackFile.Index.PackedAppBinarySize);

            // Write asset infos
            for (const auto& assetInfo : assetPackFile.AssetInfos)
            {
                writer.WriteRaw(assetInfo.Handle);
                writer.WriteRaw(assetInfo.Type);
                writer.WriteRaw(assetInfo.PackedOffset);
                writer.WriteRaw(assetInfo.PackedSize);
                writer.WriteRaw(assetInfo.Flags);
            }

            // Write scene infos
            for (const auto& sceneInfo : assetPackFile.SceneInfos)
            {
                writer.WriteRaw(sceneInfo.Handle);
                writer.WriteRaw(sceneInfo.PackedOffset);
                writer.WriteRaw(sceneInfo.PackedSize);
                writer.WriteRaw(sceneInfo.Flags);
                
                // Write scene assets map
                u32 sceneAssetCount = static_cast<u32>(sceneInfo.Assets.size());
                writer.WriteRaw(sceneAssetCount);
                for (const auto& [handle, assetInfo] : sceneInfo.Assets)
                {
                    writer.WriteRaw(handle);
                    writer.WriteRaw(assetInfo.Handle);
                    writer.WriteRaw(assetInfo.PackedOffset);
                    writer.WriteRaw(assetInfo.PackedSize);
                    writer.WriteRaw(assetInfo.Type);
                    writer.WriteRaw(assetInfo.Flags);
                }
            }

            // Write script module binary
            writer.WriteRaw(static_cast<u32>(scriptModuleBinary.Size));
            if (scriptModuleBinary.Size > 0)
            {
                writer.WriteData(reinterpret_cast<const char*>(scriptModuleBinary.Data), scriptModuleBinary.Size);
            }

            // Write actual asset data from temporary files
            for (const auto& [handle, tempFilePath] : assetPackFile.TempAssetFiles)
            {
                if (std::filesystem::exists(tempFilePath))
                {
                    // Read the temporary file and write its contents to the main pack file
                    std::ifstream tempFile(tempFilePath, std::ios::binary);
                    if (tempFile.is_open())
                    {
                        // Get file size
                        tempFile.seekg(0, std::ios::end);
                        sizet fileSize = tempFile.tellg();
                        tempFile.seekg(0, std::ios::beg);
                        
                        // Read and write data in chunks
                        constexpr sizet bufferSize = 8192;
                        char buffer[bufferSize];
                        sizet remainingBytes = fileSize;
                        
                        while (remainingBytes > 0)
                        {
                            sizet bytesToRead = std::min(bufferSize, remainingBytes);
                            tempFile.read(buffer, bytesToRead);
                            writer.WriteData(buffer, bytesToRead);
                            remainingBytes -= bytesToRead;
                        }
                        
                        tempFile.close();
                    }
                    else
                    {
                        OLO_CORE_ERROR("AssetPackBuilder: Failed to read temporary file: {}", tempFilePath.string());
                    }
                    
                    // Clean up temporary file
                    std::filesystem::remove(tempFilePath);
                }
                else
                {
                    OLO_CORE_ERROR("AssetPackBuilder: Temporary file not found: {}", tempFilePath.string());
                }
            }

            // Clear temporary file list
            assetPackFile.TempAssetFiles.clear();

            if (!writer.IsStreamGood())
            {
                result.m_ErrorMessage = "Failed to write asset pack file";
                return result;
            }

            progress = 1.0f;

            // Success!
            result.m_Success = true;
            result.m_AssetCount = assetPackFile.Index.AssetCount;
            result.m_SceneCount = assetPackFile.Index.SceneCount;

            OLO_CORE_INFO("AssetPackBuilder: Successfully built asset pack with {} assets, {} scenes", 
                         result.m_AssetCount, result.m_SceneCount);

            return result;
        }
        catch (const std::exception& e)
        {
            result.m_ErrorMessage = "Exception during build: " + std::string(e.what());
            OLO_CORE_ERROR("AssetPackBuilder: Exception during build: {}", e.what());
            return result;
        }
    }

    [[nodiscard]] bool AssetPackBuilder::SerializeAllAssets(Ref<AssetManagerBase> assetManager, AssetPackFile& assetPackFile, std::atomic<f32>& progress, const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        // Get all loaded assets from the asset manager
        auto loadedAssets = assetManager->GetLoadedAssets();
        
        assetPackFile.Index.AssetCount = static_cast<u32>(loadedAssets.size());
        assetPackFile.Index.SceneCount = 0; // Will be calculated as we process

        f32 assetProgress = 0.1f; // Start from validation progress
        f32 progressPerAsset = 0.7f / static_cast<f32>(loadedAssets.size()); // Reserve 0.7 for asset processing

        std::unordered_set<AssetHandle> processedAssets;

        // First pass: Create asset info structures and count assets
        for (const auto& [handle, asset] : loadedAssets)
        {
            // Check for cancellation at the start of each asset processing
            if (cancelToken && cancelToken->load(std::memory_order_acquire))
            {
                OLO_CORE_INFO("AssetPackBuilder: SerializeAllAssets cancelled during first pass");
                return false;
            }

            if (processedAssets.find(handle) != processedAssets.end())
                continue;

            processedAssets.insert(handle);

            // Create asset info
            AssetPackFile::AssetInfo assetInfo;
            assetInfo.Handle = handle;
            assetInfo.Type = static_cast<AssetType>(asset->GetAssetType());
            assetInfo.PackedOffset = 0; // Will be calculated during serialization pass
            assetInfo.PackedSize = 0;   // Will be calculated during serialization pass
            assetInfo.Flags = 0;

            // Count scenes separately
            if (assetInfo.Type == AssetType::Scene)
            {
                assetPackFile.Index.SceneCount++;

                // Create scene info
                AssetPackFile::SceneInfo sceneInfo;
                sceneInfo.Handle = handle;
                sceneInfo.PackedOffset = 0; // Will be calculated during serialization pass
                sceneInfo.PackedSize = 0;   // Will be calculated during serialization pass
                assetPackFile.SceneInfos.push_back(sceneInfo);
            }

            assetPackFile.AssetInfos.push_back(assetInfo);

            assetProgress += progressPerAsset * 0.5f; // Use half progress for first pass
            progress = assetProgress;
        }

        // Second pass: Actually serialize assets and calculate offsets/sizes
        // We calculate the data layout first, then write everything in order
        u64 headerSize = sizeof(AssetPackFile::FileHeader);
        u64 indexSize = sizeof(AssetPackFile::IndexTable);
        u64 assetInfosSize = assetPackFile.AssetInfos.size() * sizeof(AssetPackFile::AssetInfo);
        u64 sceneInfosSize = 0;
        for (const auto& sceneInfo : assetPackFile.SceneInfos)
        {
            sceneInfosSize += sizeof(AssetHandle) + sizeof(u64) + sizeof(u64) + sizeof(u16); // Basic scene info
            sceneInfosSize += sizeof(u32); // Scene asset count
            sceneInfosSize += sceneInfo.Assets.size() * sizeof(AssetPackFile::AssetInfo); // Scene assets
        }
        
        Buffer scriptModuleBinary = GetScriptModuleBinary();
        u64 scriptBinarySize = sizeof(u32) + scriptModuleBinary.Size; // Size field + data

        u64 assetDataStartOffset = headerSize + indexSize + assetInfosSize + sceneInfosSize + scriptBinarySize;
        u64 currentOffset = assetDataStartOffset;

        // Create temporary files for each asset and calculate sizes
        std::vector<std::pair<AssetHandle, std::filesystem::path>> tempAssetFiles;
        
        // Process regular assets
        for (auto& assetInfo : assetPackFile.AssetInfos)
        {
            // Check for cancellation at the start of each asset serialization
            if (cancelToken && cancelToken->load(std::memory_order_acquire))
            {
                OLO_CORE_INFO("AssetPackBuilder: SerializeAllAssets cancelled during second pass");
                // Clean up any temporary files created so far
                for (const auto& [handle, tempPath] : tempAssetFiles)
                {
                    std::error_code ec;
                    std::filesystem::remove(tempPath, ec);
                }
                return false;
            }

            if (assetInfo.Type == AssetType::Scene)
                continue; // Scenes are handled separately
                
            // Create a temporary file for this asset
            std::filesystem::path tempPath = std::filesystem::temp_directory_path() / ("olo_asset_" + std::to_string(assetInfo.Handle) + ".tmp");
            FileStreamWriter tempWriter(tempPath);
            
            // Record the starting position
            assetInfo.PackedOffset = currentOffset;
            
            // Serialize the asset
            AssetSerializationInfo serializationInfo;
            if (AssetImporter::SerializeToAssetPack(assetInfo.Handle, tempWriter, serializationInfo))
            {
                assetInfo.PackedSize = serializationInfo.Size;
                tempAssetFiles.emplace_back(assetInfo.Handle, tempPath);
                currentOffset += assetInfo.PackedSize;
            }
            else
            {
                OLO_CORE_ERROR("AssetPackBuilder: Failed to serialize asset with handle: {}", assetInfo.Handle);
                assetInfo.PackedSize = 0;
            }

            assetProgress += progressPerAsset * 0.5f; // Use remaining progress for second pass
            progress = assetProgress;
        }

        // Process scene assets
        for (auto& sceneInfo : assetPackFile.SceneInfos)
        {
            // Check for cancellation at the start of each scene processing
            if (cancelToken && cancelToken->load(std::memory_order_acquire))
            {
                OLO_CORE_INFO("AssetPackBuilder: SerializeAllAssets cancelled during scene processing");
                // Clean up any temporary files created so far
                for (const auto& [handle, tempPath] : tempAssetFiles)
                {
                    std::error_code ec;
                    std::filesystem::remove(tempPath, ec);
                }
                return false;
            }

            // Create a temporary file for this scene
            std::filesystem::path tempPath = std::filesystem::temp_directory_path() / ("olo_scene_" + std::to_string(sceneInfo.Handle) + ".tmp");
            FileStreamWriter tempWriter(tempPath);
            
            // Record the starting position
            sceneInfo.PackedOffset = currentOffset;
            
            // Serialize the scene asset
            AssetSerializationInfo serializationInfo;
            if (AssetImporter::SerializeToAssetPack(sceneInfo.Handle, tempWriter, serializationInfo))
            {
                sceneInfo.PackedSize = serializationInfo.Size;
                tempAssetFiles.emplace_back(sceneInfo.Handle, tempPath);
                currentOffset += sceneInfo.PackedSize;
            }
            else
            {
                OLO_CORE_ERROR("AssetPackBuilder: Failed to serialize scene with handle: {}", sceneInfo.Handle);
                sceneInfo.PackedSize = 0;
            }
        }

        // Store the temporary files for writing later
        assetPackFile.TempAssetFiles = std::move(tempAssetFiles);

        OLO_CORE_INFO("AssetPackBuilder: Serialized {} assets ({} scenes), total size: {} bytes", 
                     assetPackFile.Index.AssetCount, assetPackFile.Index.SceneCount, currentOffset - assetDataStartOffset);

        return true;
    }

    [[nodiscard]] bool AssetPackBuilder::ValidateAssets(Ref<AssetManagerBase> assetManager)
    {
        OLO_PROFILE_FUNCTION();

        auto loadedAssets = assetManager->GetLoadedAssets();

        for (const auto& [handle, asset] : loadedAssets)
        {
            if (!asset)
            {
                OLO_CORE_ERROR("AssetPackBuilder: Null asset found with handle: {}", handle);
                return false;
            }

            // Check if asset type is valid
            auto assetType = asset->GetAssetType();
            if (assetType == AssetType::None)
            {
                OLO_CORE_ERROR("AssetPackBuilder: Asset with handle {} has invalid type", handle);
                return false;
            }

            // Additional validation could be added here
            // For example, checking if required asset dependencies are available
        }

        OLO_CORE_INFO("AssetPackBuilder: Asset validation passed for {} assets", loadedAssets.size());
        return true;
    }

    Buffer AssetPackBuilder::GetScriptModuleBinary()
    {
        OLO_PROFILE_FUNCTION();

        if (auto project = Project::GetActive())
        {
            auto scriptModulePath = project->GetConfig().ScriptModulePath;
            if (!scriptModulePath.empty() && std::filesystem::exists(scriptModulePath))
            {
                OLO_CORE_INFO("AssetPackBuilder: Loading script module from: {}", scriptModulePath.string());
                return FileSystem::ReadFileBinary(scriptModulePath);
            }
        }

        OLO_CORE_INFO("AssetPackBuilder: No script module found");
        return {};
    }
}
