#include "RuntimeAssetSystem.h"

#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

#include <chrono>
#include <optional>

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
        m_Running.store(false, std::memory_order_release);
        m_AssetLoadingQueueCV.notify_all();
    }

    void RuntimeAssetSystem::StopAndWait()
    {
        Stop();
        m_Thread.Join();
    }

    void RuntimeAssetSystem::AssetThreadFunc()
    {
        OLO_PROFILER_THREAD("Runtime Asset Thread");

        while (m_Running.load(std::memory_order_acquire))
        {
            OLO_PROFILER_SCOPE("Runtime Asset Thread Queue");

            // Process all available requests in the queue
            while (m_Running.load(std::memory_order_acquire))
            {
                // Try to get next request from queue
                std::optional<RuntimeAssetLoadRequest> requestOpt;
                {
                    std::scoped_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
                    if (m_AssetLoadingQueue.empty())
                    {
                        break; // No more work, exit inner loop
                    }
                    
                    requestOpt = m_AssetLoadingQueue.front();
                    m_AssetLoadingQueue.pop();
                }

                // Process the request
                const auto& request = *requestOpt;
                if (request.Handle == 0)
                    continue;

                OLO_PROFILER_SCOPE("Runtime Asset Load");

                // Load asset from pack with exception handling
                Ref<Asset> asset = nullptr;
                try
                {
                    asset = LoadAssetFromPack(request.Handle);
                }
                catch (const std::exception& e)
                {
                    OLO_CORE_ERROR("RuntimeAssetSystem: Exception during asset loading for handle {}: {}", request.Handle, e.what());
                }
                catch (...)
                {
                    OLO_CORE_ERROR("RuntimeAssetSystem: Unknown exception during asset loading for handle {}", request.Handle);
                }
                
                // Move asset to completed queue with capacity check
                {
                    std::scoped_lock<std::mutex> lock(m_CompletedAssetsMutex);
                    
                    // Implement back-pressure: limit completed queue size
                    constexpr sizet MAX_COMPLETED_QUEUE_SIZE = 1000;
                    if (m_CompletedAssets.size() >= MAX_COMPLETED_QUEUE_SIZE)
                    {
                        OLO_CORE_WARN("RuntimeAssetSystem: Completed assets queue is full ({} items), dropping oldest asset", m_CompletedAssets.size());
                        m_CompletedAssets.pop(); // Remove oldest asset
                    }
                    
                    m_CompletedAssets.push({request.Handle, asset});
                }

                // Remove from pending set
                {
                    std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
                    m_PendingAssets.erase(request.Handle);
                }
            }

            if (m_Running.load(std::memory_order_acquire))
            {
                // Wait for new assets to load or stop signal
                std::unique_lock<std::mutex> lock(m_AssetLoadingQueueMutex);
                m_AssetLoadingQueueCV.wait(lock,
                    [this] { return !m_AssetLoadingQueue.empty() || !m_Running.load(std::memory_order_acquire); });
            }
        }
    }

    void RuntimeAssetSystem::QueueAssetLoad(RuntimeAssetLoadRequest request)
    {
        if (request.Handle == 0)
        {
            OLO_CORE_ERROR("RuntimeAssetSystem: Cannot queue asset with invalid handle");
            return;
        }

        // Check if system is still running
        if (!m_Running.load(std::memory_order_acquire))
        {
            OLO_CORE_WARN("RuntimeAssetSystem: Cannot queue asset load - system is stopped");
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
            m_AssetLoadingQueue.push(std::move(request));
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
                OLO_CORE_TRACE("RuntimeAssetSystem: Asset loaded and ready: {}", static_cast<u64>(handle));
                // TODO: Notify RuntimeAssetManager of loaded asset
            }
            else
            {
                OLO_CORE_ERROR("RuntimeAssetSystem: Failed to load asset: {}", static_cast<u64>(handle));
            }
        }
    }

    bool RuntimeAssetSystem::IsAssetPending(AssetHandle handle) const
    {
        std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
        return m_PendingAssets.find(handle) != m_PendingAssets.end();
    }

    sizet RuntimeAssetSystem::GetPendingAssetCount() const noexcept
    {
        std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
        return m_PendingAssets.size();
    }

    Ref<Asset> RuntimeAssetSystem::LoadAssetFromPack(AssetHandle handle)
    {
        OLO_PROFILER_SCOPE("RuntimeAssetSystem::LoadAssetFromPack");

        // TODO: Implement actual asset pack loading
        // This would use the AssetPack system to load assets from binary packs
        // In a full implementation, this would:
        // 1. Find the asset in the active asset pack
        // 2. Deserialize the asset from binary data
        // 3. Return the loaded asset
        
        OLO_CORE_ERROR("LoadAssetFromPack not implemented - asset pack loading not yet supported for handle {0}", handle);
        return nullptr;
    }

} // namespace OloEngine
