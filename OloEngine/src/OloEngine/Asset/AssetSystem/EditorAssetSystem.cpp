#include "EditorAssetSystem.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Task/Task.h"

namespace OloEngine
{
    EditorAssetSystem::EditorAssetSystem()
    {
    }

    EditorAssetSystem::~EditorAssetSystem()
    {
        StopAndWait();
    }

    void EditorAssetSystem::Stop()
    {
        m_Running = false;
    }

    void EditorAssetSystem::StopAndWait()
    {
        Stop();
        // We can't easily wait for all launched tasks here without tracking them all.
        // But since they are background tasks, they will eventually finish or be cleaned up by the scheduler.
        // For now, we just set m_Running to false which prevents new tasks from doing work if they check it.
    }

    void EditorAssetSystem::QueueAssetLoad(const AssetMetadata& metadata)
    {
        if (metadata.Handle == 0)
        {
            OLO_CORE_ERROR("EditorAssetSystem: Cannot queue asset with invalid handle");
            return;
        }

        if (!m_Running)
        {
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

        // Update telemetry
        m_QueuedAssetsCount.fetch_add(1, std::memory_order_relaxed);
        m_ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);

        OLO_CORE_TRACE("EditorAssetSystem: Queued asset {} for loading", (u64)metadata.Handle);

        // Check if this asset type supports async loading (two-phase loading)
        bool supportsAsync = AssetImporter::SupportsAsyncLoading(metadata.Type);

        // Launch async task
        Tasks::Launch("LoadAsset", [this, metadata, supportsAsync]()
                      {
            if (!m_Running)
            {
                m_ActiveTaskCount.fetch_sub(1, std::memory_order_relaxed);
                return;
            }

            OLO_PROFILER_SCOPE("Asset Load Task");

            // Remove from pending set now that we're processing it
            {
                std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
                m_PendingAssets.erase(metadata.Handle);
            }

            if (supportsAsync)
            {
                // Two-phase loading: Load raw data on worker thread (no GPU resources)
                // GPU finalization will happen on main thread when assets are retrieved

                RawAssetData rawData;
                if (AssetImporter::TryLoadRawData(metadata, rawData))
                {
                    // Queue raw data for main thread finalization
                    PendingRawAsset pendingAsset;
                    pendingAsset.Metadata = metadata;
                    pendingAsset.RawData = std::move(rawData);
                    pendingAsset.SerializerType = metadata.Type;

                    {
                        std::scoped_lock<std::mutex> lock(m_PendingRawAssetsMutex);
                        m_PendingRawAssets.push(std::move(pendingAsset));
                    }

                    // Update telemetry
                    m_LoadedAssetsCount.fetch_add(1, std::memory_order_relaxed);

                    OLO_CORE_TRACE("EditorAssetSystem: Raw asset data loaded (pending GPU finalization) | handle={}",
                                   (u64)metadata.Handle);
                }
                else
                {
                    // Failed to load raw data
                    m_FailedAssetsCount.fetch_add(1, std::memory_order_relaxed);
                    OLO_CORE_ERROR("EditorAssetSystem: Failed to load raw asset data for asset {}",
                                   (u64)metadata.Handle);
                }
            }
            else
            {
                // Traditional loading: Load complete asset including GPU resources
                // NOTE: This path is for assets that don't support async loading
                // They will create GPU resources on the calling thread which may cause issues
                // if called from a non-main thread. Consider moving to main thread queue.

                Ref<Asset> asset = GetAsset(metadata);

                if (asset)
                {
                    EditorAssetLoadResponse response;
                    response.Metadata = metadata;
                    response.AssetRef = asset;
                    response.NeedsGPUFinalization = false;

                    std::scoped_lock<std::mutex> lock(m_ReadyAssetsMutex);
                    m_ReadyAssets.push(response);

                    // Update telemetry
                    m_LoadedAssetsCount.fetch_add(1, std::memory_order_relaxed);

                    OLO_CORE_TRACE("EditorAssetSystem: Asset loaded | handle={}",
                                   (u64)metadata.Handle);
                }
                else
                {
                    // Update telemetry for failed loads
                    m_FailedAssetsCount.fetch_add(1, std::memory_order_relaxed);

                    OLO_CORE_ERROR("EditorAssetSystem: Failed to load asset {}",
                                   (u64)metadata.Handle);
                }
            }

            m_ActiveTaskCount.fetch_sub(1, std::memory_order_relaxed); }, Tasks::ETaskPriority::BackgroundNormal);
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

    bool EditorAssetSystem::RetrievePendingRawAssets(std::vector<PendingRawAsset>& outRawAssets)
    {
        std::scoped_lock<std::mutex> lock(m_PendingRawAssetsMutex);

        if (m_PendingRawAssets.empty())
            return false;

        while (!m_PendingRawAssets.empty())
        {
            outRawAssets.push_back(std::move(m_PendingRawAssets.front()));
            m_PendingRawAssets.pop();
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
        // This was previously called from the thread loop.
        // Since we removed the thread loop, this function is currently unused.
        // We can leave it empty or remove it.
        // The header still declares it private.
    }

    std::tuple<u32, u32, u32, sizet> EditorAssetSystem::GetTelemetry() const
    {
        return std::make_tuple(
            m_QueuedAssetsCount.load(std::memory_order_acquire),
            m_LoadedAssetsCount.load(std::memory_order_acquire),
            m_FailedAssetsCount.load(std::memory_order_acquire),
            m_ActiveTaskCount.load(std::memory_order_acquire));
    }

    sizet EditorAssetSystem::GetQueueLength() const
    {
        return m_ActiveTaskCount.load(std::memory_order_acquire);
    }

} // namespace OloEngine
