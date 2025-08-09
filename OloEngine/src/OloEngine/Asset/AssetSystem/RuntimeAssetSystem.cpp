#include "RuntimeAssetSystem.h"

#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    RuntimeAssetSystem::RuntimeAssetSystem()
        : m_Thread("Runtime Asset Thread")
    {
        m_Thread.Dispatch([this]() { AssetThreadFunc(); });
    }

    RuntimeAssetSystem::~RuntimeAssetSystem()
    {
        StopAndWait();
    }

    void RuntimeAssetSystem::Stop()
    {
        m_Running = false;
        m_AssetLoadingQueueCV.notify_one();
    }

    void RuntimeAssetSystem::StopAndWait()
    {
        Stop();
        m_Thread.Join();
    }

    void RuntimeAssetSystem::AssetThreadFunc()
    {
        OLO_PROFILER_THREAD("Runtime Asset Thread");

        while (m_Running)
        {
            OLO_PROFILER_SCOPE("Runtime Asset Thread Queue");

            bool queueEmptyOrStop = false;
            while (!queueEmptyOrStop)
            {
                RuntimeAssetLoadRequest request;
                {
                    std::scoped_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
                    if (m_AssetLoadingQueue.empty() || !m_Running)
                    {
                        queueEmptyOrStop = true;
                    }
                    else
                    {
                        request = m_AssetLoadingQueue.front();
                        m_AssetLoadingQueue.pop();
                    }
                }

                if (request.Handle == 0)
                    continue;

                OLO_PROFILER_SCOPE("Runtime Asset Load");

                // Load asset from pack
                Ref<Asset> asset = LoadAssetFromPack(request.Handle);
                
                // Move asset to completed queue
                {
                    std::scoped_lock<std::mutex> lock(m_CompletedAssetsMutex);
                    m_CompletedAssets.push({request.Handle, asset});
                }

                // Remove from pending set
                {
                    std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
                    m_PendingAssets.erase(request.Handle);
                }
            }

            if (m_Running)
            {
                // Wait for new assets to load or stop signal
                std::unique_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
                m_AssetLoadingQueueCV.wait_for(lock, std::chrono::milliseconds(50),
                    [this] { return !m_AssetLoadingQueue.empty() || !m_Running; });
            }
        }
    }

    void RuntimeAssetSystem::QueueAssetLoad(const RuntimeAssetLoadRequest& request)
    {
        if (request.Handle == 0)
        {
            OLO_CORE_ERROR("RuntimeAssetSystem: Cannot queue asset with invalid handle");
            return;
        }

        // Check if already pending
        {
            std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
            if (m_PendingAssets.find(request.Handle) != m_PendingAssets.end())
            {
                // Already pending, don't queue again
                return;
            }
            m_PendingAssets.insert(request.Handle);
        }

        {
            std::scoped_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
            m_AssetLoadingQueue.push(request);
        }
        m_AssetLoadingQueueCV.notify_one();
    }

    void RuntimeAssetSystem::SyncWithAssetThread()
    {
        OLO_PROFILER_SCOPE("RuntimeAssetSystem::SyncWithAssetThread");

        // This method is called from the main thread to process completed assets
        // In a full implementation, this would notify the RuntimeAssetManager
        // of newly loaded assets. For now, we just clear the completed queue.
        
        std::scoped_lock<std::mutex> lock(m_CompletedAssetsMutex);
        while (!m_CompletedAssets.empty())
        {
            auto [handle, asset] = m_CompletedAssets.front();
            m_CompletedAssets.pop();
            
            if (asset)
            {
                OLO_CORE_TRACE("RuntimeAssetSystem: Asset loaded and ready: {}", static_cast<uint64_t>(handle));
                // TODO: Notify RuntimeAssetManager of loaded asset
            }
            else
            {
                OLO_CORE_ERROR("RuntimeAssetSystem: Failed to load asset: {}", static_cast<uint64_t>(handle));
            }
        }
    }

    bool RuntimeAssetSystem::IsAssetPending(AssetHandle handle) const
    {
        std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
        return m_PendingAssets.find(handle) != m_PendingAssets.end();
    }

    size_t RuntimeAssetSystem::GetPendingAssetCount() const
    {
        std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
        return m_PendingAssets.size();
    }

    Ref<Asset> RuntimeAssetSystem::LoadAssetFromPack(AssetHandle handle)
    {
        OLO_PROFILER_SCOPE("RuntimeAssetSystem::LoadAssetFromPack");

        // TODO: Implement actual asset pack loading
        // This would use the AssetPack system to load assets from binary packs
        OLO_CORE_WARN("RuntimeAssetSystem: Asset pack loading not fully implemented yet");
        
        // For now, return nullptr to indicate loading failure
        // In a full implementation, this would:
        // 1. Find the asset in the active asset pack
        // 2. Deserialize the asset from binary data
        // 3. Return the loaded asset
        
        return nullptr;
    }

} // namespace OloEngine
