#include "EditorAssetSystem.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    EditorAssetSystem::EditorAssetSystem()
        : m_Thread("Asset Thread")
    {
        m_Thread.Dispatch([this]() { AssetThreadFunc(); });
    }

    EditorAssetSystem::~EditorAssetSystem()
    {
        StopAndWait();
    }

    void EditorAssetSystem::Stop()
    {
        m_Running = false;
        m_AssetLoadingQueueCV.notify_one();
    }

    void EditorAssetSystem::StopAndWait()
    {
        Stop();
        m_Thread.Join();
    }

    void EditorAssetSystem::AssetMonitorUpdate()
    {
        Timer timer;
        EnsureAllLoadedCurrent();
        m_AssetUpdatePerf = timer.ElapsedMillis();
    }

    void EditorAssetSystem::AssetThreadFunc()
    {
        OLO_PROFILER_THREAD("Asset Thread");

        while (m_Running)
        {
            OLO_PROFILER_SCOPE("Asset Thread Queue");

            AssetMonitorUpdate();

            bool queueEmptyOrStop = false;
            while (!queueEmptyOrStop)
            {
                AssetMetadata metadata;
                {
                    std::scoped_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
                    if (m_AssetLoadingQueue.empty() || !m_Running)
                    {
                        queueEmptyOrStop = true;
                    }
                    else
                    {
                        metadata = m_AssetLoadingQueue.front();
                        m_AssetLoadingQueue.pop();
                    }
                }

                // If queueEmptyOrStop then metadata will be invalid (Handle == 0)
                // We check metadata here (instead of just breaking straight away on queueEmptyOrStop)
                // to deal with the edge case that other thread might queue requests for invalid assets.
                // This way, we just pop those requests and ignore them.
                if (metadata.Handle == 0)
                    continue;

                OLO_PROFILER_SCOPE("Asset Load");

                // Remove from pending set now that we're processing it
                {
                    std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
                    m_PendingAssets.erase(metadata.Handle);
                }

                Ref<Asset> asset = GetAsset(metadata);
                if (asset)
                {
                    EditorAssetLoadResponse response;
                    response.Metadata = metadata;
                    response.AssetRef = asset;

                    std::scoped_lock<std::mutex> lock(m_ReadyAssetsMutex);
                    m_ReadyAssets.push(response);
                    
                    // Update telemetry
                    m_LoadedAssetsCount++;
                    
                    OLO_CORE_TRACE("EditorAssetSystem: Asset {} loaded and ready (loaded: {}, failed: {})", 
                                   (u64)metadata.Handle, m_LoadedAssetsCount.load(), m_FailedAssetsCount.load());
                }
                else
                {
                    // Update telemetry for failed loads
                    m_FailedAssetsCount++;
                    
                    OLO_CORE_ERROR("EditorAssetSystem: Failed to load asset {} (loaded: {}, failed: {})", 
                                   (u64)metadata.Handle, m_LoadedAssetsCount.load(), m_FailedAssetsCount.load());
                }
            }

            if (m_Running)
            {
                // Wait for new assets to load or stop signal
                std::unique_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
                m_AssetLoadingQueueCV.wait_for(lock, std::chrono::milliseconds(100),
                    [this] { return !m_AssetLoadingQueue.empty() || !m_Running; });
            }
        }
    }

    void EditorAssetSystem::QueueAssetLoad(const AssetMetadata& metadata)
    {
        if (metadata.Handle == 0)
        {
            OLO_CORE_ERROR("EditorAssetSystem: Cannot queue asset with invalid handle");
            return;
        }

        // Check if already pending to prevent duplicate loading
        {
            std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
            if (m_PendingAssets.contains(metadata.Handle))
            {
                OLO_CORE_TRACE("EditorAssetSystem: Asset {} already queued for loading", (u64)metadata.Handle);
                return;
            }
            m_PendingAssets.insert(metadata.Handle);
        }

        {
            std::scoped_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
            m_AssetLoadingQueue.push(metadata);
        }
        m_AssetLoadingQueueCV.notify_one();
        
        // Update telemetry
        m_QueuedAssetsCount++;
        
        OLO_CORE_TRACE("EditorAssetSystem: Queued asset {} for loading (queue size: {})", 
                       (u64)metadata.Handle, GetQueueLength());
    }

    Ref<Asset> EditorAssetSystem::GetAsset(const AssetMetadata& metadata)
    {
        OLO_PROFILER_SCOPE("EditorAssetSystem::GetAsset");

        if (!metadata.IsValid())
        {
            OLO_CORE_ERROR("EditorAssetSystem: Invalid asset metadata");
            return nullptr;
        }

        try
        {
            // Load the asset using the asset importer
            Ref<Asset> asset;
            if (AssetImporter::TryLoadData(metadata, asset))
            {
                asset->m_Handle = metadata.Handle;
                OLO_CORE_TRACE("EditorAssetSystem: Successfully loaded asset: {} ({})",
                              metadata.FilePath.string(), static_cast<u32>(metadata.Type));
            }
            else
            {
                OLO_CORE_ERROR("EditorAssetSystem: Failed to load asset: {}", metadata.FilePath.string());
                return nullptr;
            }
            return asset;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("EditorAssetSystem: Exception while loading asset {}: {}",
                           metadata.FilePath.string(), e.what());
            return nullptr;
        }
    }

    bool EditorAssetSystem::RetrieveReadyAssets(std::vector<EditorAssetLoadResponse>& outAssetList)
    {
        std::scoped_lock<std::mutex> lock(m_ReadyAssetsMutex);
        
        if (m_ReadyAssets.empty())
            return false;

        while (!m_ReadyAssets.empty())
        {
            outAssetList.push_back(m_ReadyAssets.front());
            m_ReadyAssets.pop();
        }

        return true;
    }

    void EditorAssetSystem::UpdateLoadedAssetList(const std::unordered_map<AssetHandle, Ref<Asset>>& loadedAssets)
    {
        std::scoped_lock<std::mutex> lock(m_LoadedAssetsMutex);
        m_LoadedAssets = loadedAssets;
    }

    void EditorAssetSystem::EnsureAllLoadedCurrent()
    {
        OLO_PROFILER_SCOPE("EditorAssetSystem::EnsureAllLoadedCurrent");

        std::scoped_lock<std::mutex> lock(m_LoadedAssetsMutex);
        
        for (auto& [handle, asset] : m_LoadedAssets)
        {
            if (!asset)
                continue;

            // Check if asset file has been modified
            // This is a simplified check - in a full implementation,
            // this would check file modification times and trigger reloads
            
            // TODO: Implement file watching and modification detection
            // For now, we'll just verify the asset is still valid
        }
    }

    std::tuple<u32, u32, u32, sizet> EditorAssetSystem::GetTelemetry() const
    {
        std::scoped_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
        return std::make_tuple(
            m_QueuedAssetsCount.load(),
            m_LoadedAssetsCount.load(), 
            m_FailedAssetsCount.load(),
            m_AssetLoadingQueue.size()
        );
    }

    sizet EditorAssetSystem::GetQueueLength() const
    {
        std::scoped_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
        return m_AssetLoadingQueue.size();
    }

} // namespace OloEngine
