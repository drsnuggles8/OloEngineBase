#include "RuntimeAssetSystem.h"

#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Task/Task.h"

#include <chrono>
#include <optional>

namespace OloEngine
{
    RuntimeAssetSystem::RuntimeAssetSystem()
    {
    }

    RuntimeAssetSystem::~RuntimeAssetSystem()
    {
        StopAndWait();
    }

    void RuntimeAssetSystem::Stop()
    {
        m_Running.store(false, std::memory_order_release);
    }

    void RuntimeAssetSystem::StopAndWait()
    {
        Stop();
        // Tasks will finish naturally or be cancelled by scheduler shutdown
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

        m_ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);

        Tasks::Launch("RuntimeAssetLoad", [this, request]()
        {
            if (!m_Running.load(std::memory_order_acquire))
            {
                m_ActiveTaskCount.fetch_sub(1, std::memory_order_relaxed);
                return;
            }

            OLO_PROFILER_SCOPE("Runtime Asset Load Task");

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

                m_CompletedAssets.push({ request.Handle, asset });
            }

            // Remove from pending set
            {
                std::scoped_lock<std::mutex> lock(m_PendingAssetsMutex);
                m_PendingAssets.erase(request.Handle);
            }

            m_ActiveTaskCount.fetch_sub(1, std::memory_order_relaxed);
        }, Tasks::ETaskPriority::BackgroundNormal);
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
