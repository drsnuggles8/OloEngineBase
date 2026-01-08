#include "OloEnginePCH.h"
#include "GPUResourceQueue.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    // Static member definitions
    std::queue<std::unique_ptr<GPUResourceCommand>> GPUResourceQueue::s_CommandQueue;
    FMutex GPUResourceQueue::s_QueueMutex;
    std::atomic<u64> GPUResourceQueue::s_QueuedCount{ 0 };
    std::atomic<u64> GPUResourceQueue::s_ProcessedCount{ 0 };
    std::atomic<u64> GPUResourceQueue::s_FailedCount{ 0 };

    u32 GPUResourceQueue::ProcessAll()
    {
        OLO_PROFILE_FUNCTION();

        std::queue<std::unique_ptr<GPUResourceCommand>> localQueue;

        // Move all commands to local queue under lock
        {
            TUniqueLock<FMutex> lock(s_QueueMutex);
            std::swap(localQueue, s_CommandQueue);
        }

        u32 processed = 0;
        while (!localQueue.empty())
        {
            auto& cmd = localQueue.front();
            try
            {
                OLO_PROFILE_SCOPE("GPUResourceCommand::Execute");
                cmd->Execute();
                s_ProcessedCount.fetch_add(1, std::memory_order_relaxed);
                processed++;
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("GPUResourceQueue: Command execution failed: {}", e.what());
                s_FailedCount.fetch_add(1, std::memory_order_relaxed);
            }
            catch (...)
            {
                OLO_CORE_ERROR("GPUResourceQueue: Command execution failed with unknown error");
                s_FailedCount.fetch_add(1, std::memory_order_relaxed);
            }
            localQueue.pop();
        }

        if (processed > 0)
        {
            OLO_CORE_TRACE("GPUResourceQueue: Processed {} commands", processed);
        }

        return processed;
    }

    u32 GPUResourceQueue::ProcessBatch(u32 maxCommands)
    {
        OLO_PROFILE_FUNCTION();

        if (maxCommands == 0)
            return 0;

        std::vector<std::unique_ptr<GPUResourceCommand>> batch;
        batch.reserve(maxCommands);

        // Extract up to maxCommands from the queue
        {
            TUniqueLock<FMutex> lock(s_QueueMutex);
            while (!s_CommandQueue.empty() && batch.size() < maxCommands)
            {
                batch.push_back(std::move(s_CommandQueue.front()));
                s_CommandQueue.pop();
            }
        }

        u32 processed = 0;
        for (auto& cmd : batch)
        {
            try
            {
                OLO_PROFILE_SCOPE("GPUResourceCommand::Execute");
                cmd->Execute();
                s_ProcessedCount.fetch_add(1, std::memory_order_relaxed);
                processed++;
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("GPUResourceQueue: Command execution failed: {}", e.what());
                s_FailedCount.fetch_add(1, std::memory_order_relaxed);
            }
            catch (...)
            {
                OLO_CORE_ERROR("GPUResourceQueue: Command execution failed with unknown error");
                s_FailedCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        return processed;
    }

    // ========================================================================
    // CreateTexture2DCommand Implementation
    // ========================================================================

    void CreateTexture2DCommand::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Data.IsValid())
        {
            OLO_CORE_ERROR("CreateTexture2DCommand: Invalid texture data");
            if (m_Callback)
                m_Callback(nullptr);
            return;
        }

        try
        {
            // Create texture specification
            TextureSpecification spec;
            spec.Width = m_Data.Width;
            spec.Height = m_Data.Height;
            spec.GenerateMips = m_Data.GenerateMipmaps;

            // Determine format based on channel count
            // Note: The engine currently only supports R8, RGB8, RGBA8 and a few other formats
            switch (m_Data.Channels)
            {
                case 1:
                    spec.Format = ImageFormat::R8;
                    break;
                case 3:
                    spec.Format = ImageFormat::RGB8;
                    break;
                case 4:
                default:
                    spec.Format = ImageFormat::RGBA8;
                    break;
            }

            // Create the texture on the main thread (this is the only place GL calls happen)
            Ref<Texture2D> texture = Texture2D::Create(spec);

            if (texture)
            {
                // Set the pixel data
                u32 dataSize = static_cast<u32>(m_Data.PixelData.size());
                texture->SetData(const_cast<u8*>(m_Data.PixelData.data()), dataSize);

                OLO_CORE_TRACE("CreateTexture2DCommand: Created texture '{}' ({}x{}, {} channels)",
                               m_Data.DebugName, m_Data.Width, m_Data.Height, m_Data.Channels);
            }
            else
            {
                OLO_CORE_ERROR("CreateTexture2DCommand: Failed to create texture '{}'", m_Data.DebugName);
            }

            // Invoke callback with the created texture (or nullptr on failure)
            if (m_Callback)
                m_Callback(texture);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("CreateTexture2DCommand: Exception during texture creation: {}", e.what());
            if (m_Callback)
                m_Callback(nullptr);
        }
    }

    // ========================================================================
    // CreateShaderCommand Implementation
    // ========================================================================

    void CreateShaderCommand::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Data.IsValid())
        {
            OLO_CORE_ERROR("CreateShaderCommand: No shader source provided for '{}'", m_Data.Name);
            if (m_Callback)
                m_Callback(nullptr);
            return;
        }

        try
        {
            Ref<Shader> shader = nullptr;

            if (!m_Data.ComputeSource.empty())
            {
                // Compute shader
                // Note: If Shader::CreateCompute exists, use it; otherwise this is a placeholder
                OLO_CORE_WARN("CreateShaderCommand: Compute shader creation not yet implemented for '{}'", m_Data.Name);
                // shader = Shader::CreateCompute(m_Data.Name, m_Data.ComputeSource);
            }
            else
            {
                // Traditional vertex/fragment shader
                shader = Shader::Create(m_Data.Name, m_Data.VertexSource, m_Data.FragmentSource);
            }

            if (shader)
            {
                OLO_CORE_TRACE("CreateShaderCommand: Created shader '{}'", m_Data.Name);
            }
            else
            {
                OLO_CORE_ERROR("CreateShaderCommand: Failed to create shader '{}'", m_Data.Name);
            }

            if (m_Callback)
                m_Callback(shader);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("CreateShaderCommand: Exception during shader creation for '{}': {}",
                           m_Data.Name, e.what());
            if (m_Callback)
                m_Callback(nullptr);
        }
    }

} // namespace OloEngine
