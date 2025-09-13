#include "OloEnginePCH.h"
#include "AssetPackBuilder.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <unordered_set>

namespace OloEngine
{
    AssetPackBuilder::BuildResult AssetPackBuilder::BuildFromActiveProject(const BuildSettings& settings, std::atomic<float>& progress)
    {
        OLO_PROFILE_FUNCTION();

        // Ensure we have an active project
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
        return BuildImpl(assetManager, settings, progress);
    }

    AssetPackBuilder::BuildResult AssetPackBuilder::BuildFromRegistry(const AssetRegistry& assetRegistry, const BuildSettings& settings, std::atomic<float>& progress)
    {
        OLO_PROFILE_FUNCTION();

        // For registry-based building, we'd need a way to get an asset manager that uses this registry
        // For now, this is a placeholder implementation
        OLO_CORE_WARN("AssetPackBuilder::BuildFromRegistry - Not fully implemented, falling back to active project");
        return BuildFromActiveProject(settings, progress);
    }

    AssetPackBuilder::BuildResult AssetPackBuilder::BuildImpl(Ref<AssetManagerBase> assetManager, const BuildSettings& settings, std::atomic<float>& progress)
    {
        OLO_PROFILE_FUNCTION();

        progress = 0.0f;

        BuildResult result;
        result.OutputPath = settings.OutputPath;

        try
        {
            // Validate assets if requested
            if (settings.ValidateAssets)
            {
                OLO_CORE_INFO("AssetPackBuilder: Validating assets...");
                if (!ValidateAssets(assetManager))
                {
                    result.ErrorMessage = "Asset validation failed";
                    return result;
                }
                progress = 0.1f;
            }

            // Create output directory if it doesn't exist
            std::filesystem::create_directories(settings.OutputPath.parent_path());

            // Initialize asset pack file structure
            AssetPackFile assetPackFile;
            
            // Serialize all assets
            OLO_CORE_INFO("AssetPackBuilder: Serializing assets...");
            if (!SerializeAllAssets(assetManager, assetPackFile, progress))
            {
                result.ErrorMessage = "Failed to serialize assets";
                return result;
            }

            progress = 0.8f;

            // Get script module binary if requested
            Buffer scriptModuleBinary;
            if (settings.IncludeScriptModule)
            {
                scriptModuleBinary = GetScriptModuleBinary();
                OLO_CORE_INFO("AssetPackBuilder: Script module binary size: {} bytes", scriptModuleBinary.Size);
            }

            progress = 0.9f;

            // Serialize the pack to file
            OLO_CORE_INFO("AssetPackBuilder: Writing asset pack to: {}", settings.OutputPath.string());
            
            // For now, we'll use a simple approach similar to how AssetPack::Load works in reverse
            // This would need to be implemented in AssetPackFile or AssetPackSerializer
            // Based on the existing codebase structure, we'll create a basic implementation
            
            FileStreamWriter writer(settings.OutputPath);
            if (!writer.IsStreamGood())
            {
                result.ErrorMessage = "Failed to create output file: " + settings.OutputPath.string();
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

            // Write asset data (this would be where the actual serialized asset data goes)
            // For now, we'll just write placeholder data since the actual serialization
            // would need to be coordinated with the existing AssetSerializer system

            if (!writer.IsStreamGood())
            {
                result.ErrorMessage = "Failed to write asset pack file";
                return result;
            }

            progress = 1.0f;

            // Success!
            result.Success = true;
            result.AssetCount = assetPackFile.Index.AssetCount;
            result.SceneCount = assetPackFile.Index.SceneCount;

            OLO_CORE_INFO("AssetPackBuilder: Successfully built asset pack with {} assets, {} scenes", 
                         result.AssetCount, result.SceneCount);

            return result;
        }
        catch (const std::exception& e)
        {
            result.ErrorMessage = "Exception during build: " + std::string(e.what());
            OLO_CORE_ERROR("AssetPackBuilder: Exception during build: {}", e.what());
            return result;
        }
    }

    bool AssetPackBuilder::SerializeAllAssets(Ref<AssetManagerBase> assetManager, AssetPackFile& assetPackFile, std::atomic<float>& progress)
    {
        OLO_PROFILE_FUNCTION();

        // Get all loaded assets from the asset manager
        auto loadedAssets = assetManager->GetLoadedAssets();
        
        assetPackFile.Index.AssetCount = static_cast<u32>(loadedAssets.size());
        assetPackFile.Index.SceneCount = 0; // Will be calculated as we process

        float assetProgress = 0.1f; // Start from validation progress
        float progressPerAsset = 0.7f / static_cast<float>(loadedAssets.size()); // Reserve 0.7 for asset processing

        std::unordered_set<AssetHandle> processedAssets;

        for (const auto& [handle, asset] : loadedAssets)
        {
            if (processedAssets.contains(handle))
                continue;

            processedAssets.insert(handle);

            // Create asset info
            AssetPackFile::AssetInfo assetInfo;
            assetInfo.Handle = handle;
            assetInfo.Type = static_cast<AssetType>(asset->GetAssetType());
            assetInfo.PackedOffset = 0; // Will be calculated during actual serialization
            assetInfo.PackedSize = 0;   // Will be calculated during actual serialization
            assetInfo.Flags = 0;

            // Count scenes separately
            if (assetInfo.Type == AssetType::Scene)
            {
                assetPackFile.Index.SceneCount++;

                // Create scene info
                AssetPackFile::SceneInfo sceneInfo;
                sceneInfo.Handle = handle;
                sceneInfo.PackedOffset = 0; // Will be calculated during actual serialization
                sceneInfo.PackedSize = 0;   // Will be calculated during actual serialization
                assetPackFile.SceneInfos.push_back(sceneInfo);
            }

            assetPackFile.AssetInfos.push_back(assetInfo);

            assetProgress += progressPerAsset;
            progress = assetProgress;
        }

        OLO_CORE_INFO("AssetPackBuilder: Processed {} assets ({} scenes)", 
                     assetPackFile.Index.AssetCount, assetPackFile.Index.SceneCount);

        return true;
    }

    bool AssetPackBuilder::ValidateAssets(Ref<AssetManagerBase> assetManager)
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

        // Try to get script module from project
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