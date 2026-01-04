#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"

#include <functional>
#include <queue>
#include <mutex>
#include <variant>
#include <vector>
#include <memory>

namespace OloEngine
{
    // Forward declarations
    class Texture2D;
    class Shader;
    class Mesh;

    /**
     * @brief Types of GPU resource creation commands
     *
     * This is designed to be graphics-API agnostic. Currently OpenGL,
     * but the pattern works for Vulkan/D3D12/Metal as well.
     */
    enum class GPUResourceCommandType : u8
    {
        CreateTexture2D,
        CreateCubemap,
        CreateShader,
        CreateMesh,
        CreateBuffer,
        DeleteTexture,
        DeleteShader,
        DeleteBuffer,
        Custom
    };

    /**
     * @brief Base class for GPU resource creation commands
     *
     * Commands are created on worker threads and executed on the main thread.
     * All GPU API calls happen during Execute().
     */
    class GPUResourceCommand
    {
      public:
        virtual ~GPUResourceCommand() = default;

        // Execute the command on the main thread (creates GPU resources)
        virtual void Execute() = 0;

        // Get the command type for debugging/profiling
        virtual GPUResourceCommandType GetType() const = 0;

        // Optional: Asset handle this command is associated with
        AssetHandle AssociatedAsset = 0;
    };

    // ========================================================================
    // Raw Asset Data Structures (Thread-Safe, No GPU Resources)
    // ========================================================================

    /**
     * @brief Raw texture data loaded from disk - no GPU resources
     *
     * This intermediate structure holds decoded pixel data that can be
     * safely created on any thread. GPU texture creation happens later
     * on the main thread.
     */
    struct RawTextureData
    {
        std::vector<u8> PixelData;   ///< Decoded pixel data (RGBA, RGB, etc.)
        u32 Width = 0;
        u32 Height = 0;
        u32 Channels = 0;            ///< 1=R, 2=RG, 3=RGB, 4=RGBA
        bool GenerateMipmaps = true;
        bool SRGB = false;           ///< True for diffuse/albedo textures
        std::string DebugName;       ///< For GPU debugging tools
        AssetHandle Handle = 0;      ///< Associated asset handle

        [[nodiscard]] bool IsValid() const
        {
            return !PixelData.empty() && Width > 0 && Height > 0 && Channels > 0;
        }

        [[nodiscard]] sizet GetDataSize() const
        {
            return static_cast<sizet>(Width) * Height * Channels;
        }
    };

    /**
     * @brief Raw shader source - no GPU resources
     */
    struct RawShaderData
    {
        std::string VertexSource;
        std::string FragmentSource;
        std::string GeometrySource;   // Optional
        std::string ComputeSource;    // Optional (for compute shaders)
        std::string Name;
        AssetHandle Handle = 0;

        [[nodiscard]] bool IsValid() const
        {
            return !VertexSource.empty() || !ComputeSource.empty();
        }
    };

    // ========================================================================
    // GPU Resource Creation Commands
    // ========================================================================

    /**
     * @brief Command to create a Texture2D from loaded image data
     */
    class CreateTexture2DCommand : public GPUResourceCommand
    {
      public:
        CreateTexture2DCommand(RawTextureData&& data, std::function<void(Ref<Texture2D>)> callback)
            : m_Data(std::move(data)), m_Callback(std::move(callback))
        {
            AssociatedAsset = m_Data.Handle;
        }

        void Execute() override;
        GPUResourceCommandType GetType() const override { return GPUResourceCommandType::CreateTexture2D; }

        const RawTextureData& GetData() const { return m_Data; }

      private:
        RawTextureData m_Data;
        std::function<void(Ref<Texture2D>)> m_Callback;
    };

    /**
     * @brief Command to compile/link a shader from source
     */
    class CreateShaderCommand : public GPUResourceCommand
    {
      public:
        CreateShaderCommand(RawShaderData&& data, std::function<void(Ref<Shader>)> callback)
            : m_Data(std::move(data)), m_Callback(std::move(callback))
        {
            AssociatedAsset = m_Data.Handle;
        }

        void Execute() override;
        GPUResourceCommandType GetType() const override { return GPUResourceCommandType::CreateShader; }

        const RawShaderData& GetData() const { return m_Data; }

      private:
        RawShaderData m_Data;
        std::function<void(Ref<Shader>)> m_Callback;
    };

    /**
     * @brief Custom command with arbitrary callback
     *
     * Use for one-off GPU operations that don't fit the predefined types.
     */
    class CustomGPUCommand : public GPUResourceCommand
    {
      public:
        explicit CustomGPUCommand(std::function<void()> callback, std::string debugName = "Custom")
            : m_Callback(std::move(callback)), m_DebugName(std::move(debugName))
        {
        }

        void Execute() override
        {
            if (m_Callback)
                m_Callback();
        }

        GPUResourceCommandType GetType() const override { return GPUResourceCommandType::Custom; }

        const std::string& GetDebugName() const { return m_DebugName; }

      private:
        std::function<void()> m_Callback;
        std::string m_DebugName;
    };

    // ========================================================================
    // GPU Resource Queue
    // ========================================================================

    /**
     * @brief Thread-safe queue for deferred GPU resource creation
     *
     * This is a core component of the engine's threading model:
     * - Worker threads load asset data (decode images, parse meshes, read shaders)
     * - Worker threads enqueue GPU resource creation requests
     * - Main thread processes queue at frame start, creating actual GPU resources
     *
     * This pattern is used by:
     * - Unreal Engine (FRHICommandList)
     * - Unity (AsyncGPUReadback, upload buffer)
     * - Godot (RenderingServer)
     * - bgfx (submit queue)
     *
     * Usage Pattern:
     * @code
     * // Worker thread (e.g., asset loader)
     * RawTextureData rawData = LoadAndDecodeImage(path);
     * GPUResourceQueue::Enqueue<CreateTexture2DCommand>(
     *     std::move(rawData),
     *     [handle](Ref<Texture2D> texture) {
     *         // Called on main thread after GPU resource is created
     *         AssetManager::FinalizeAsset(handle, texture);
     *     }
     * );
     *
     * // Main thread (BeginFrame or similar)
     * GPUResourceQueue::ProcessAll();  // Creates GPU resources
     * @endcode
     *
     * Thread Safety:
     * - Enqueue() is thread-safe and can be called from any thread
     * - ProcessAll()/ProcessBatch() must only be called from the main thread
     */
    class GPUResourceQueue
    {
      public:
        /**
         * @brief Enqueue a resource creation command
         *
         * @tparam T Command type (must derive from GPUResourceCommand)
         * @tparam Args Constructor argument types
         * @param args Arguments forwarded to the command constructor
         */
        template<typename T, typename... Args>
        static void Enqueue(Args&&... args)
        {
            static_assert(std::is_base_of_v<GPUResourceCommand, T>,
                          "T must derive from GPUResourceCommand");

            auto cmd = std::make_unique<T>(std::forward<Args>(args)...);

            std::scoped_lock<std::mutex> lock(s_QueueMutex);
            s_CommandQueue.push(std::move(cmd));
            s_QueuedCount++;
        }

        /**
         * @brief Enqueue a custom callback to run on main thread
         */
        static void EnqueueCustom(std::function<void()> callback, std::string debugName = "Custom")
        {
            Enqueue<CustomGPUCommand>(std::move(callback), std::move(debugName));
        }

        /**
         * @brief Process all queued commands on the main thread
         *
         * Call this at frame start (e.g., in BeginFrame or BeginScene).
         * @return Number of commands processed
         */
        static u32 ProcessAll();

        /**
         * @brief Process up to maxCommands from the queue
         *
         * Use this for spreading work across multiple frames if needed.
         * @param maxCommands Maximum number of commands to process
         * @return Number of commands actually processed
         */
        static u32 ProcessBatch(u32 maxCommands);

        /**
         * @brief Check if there are pending commands
         */
        static bool HasPending()
        {
            std::scoped_lock<std::mutex> lock(s_QueueMutex);
            return !s_CommandQueue.empty();
        }

        /**
         * @brief Get the number of pending commands
         */
        static u32 GetPendingCount()
        {
            std::scoped_lock<std::mutex> lock(s_QueueMutex);
            return static_cast<u32>(s_CommandQueue.size());
        }

        /**
         * @brief Get statistics
         */
        struct Statistics
        {
            u64 TotalQueued = 0;
            u64 TotalProcessed = 0;
            u64 TotalFailed = 0;
            u32 CurrentPending = 0;
        };

        static Statistics GetStatistics()
        {
            std::scoped_lock<std::mutex> lock(s_QueueMutex);
            Statistics stats;
            stats.TotalQueued = s_QueuedCount;
            stats.TotalProcessed = s_ProcessedCount;
            stats.TotalFailed = s_FailedCount;
            stats.CurrentPending = static_cast<u32>(s_CommandQueue.size());
            return stats;
        }

        /**
         * @brief Clear all pending commands (use during shutdown)
         */
        static void Clear()
        {
            std::scoped_lock<std::mutex> lock(s_QueueMutex);
            while (!s_CommandQueue.empty())
                s_CommandQueue.pop();
        }

      private:
        static std::queue<std::unique_ptr<GPUResourceCommand>> s_CommandQueue;
        static std::mutex s_QueueMutex;
        static u64 s_QueuedCount;
        static u64 s_ProcessedCount;
        static u64 s_FailedCount;
    };

} // namespace OloEngine
