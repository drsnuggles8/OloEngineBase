#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/ArrayResource.h"

namespace OloEngine
{
    /**
     * @brief Manages resources across multiple frames in flight to prevent GPU/CPU synchronization stalls
     * 
     * This class implements frame-in-flight resource management to avoid blocking CPU/GPU synchronization.
     * It maintains separate resource instances for each frame in flight, allowing the CPU to write to
     * the next frame's resources while the GPU is still processing the current frame.
     * 
     * Key benefits:
     * - Eliminates GPU/CPU synchronization stalls
     * - Improves rendering performance through pipelining
     * - Prevents data races between CPU writes and GPU reads
     * - Supports both individual resources and array resources
     */
    class FrameInFlightManager
    {
    private:
        static constexpr u32 DEFAULT_FRAMES_IN_FLIGHT = 3;
        
        u32 m_FramesInFlight;
        u32 m_CurrentFrameIndex = 0;
        u32 m_FrameNumber = 0;
        
        // Resource storage for each frame
        struct FrameResources
        {
            std::unordered_map<std::string, std::vector<Ref<UniformBuffer>>> m_UniformBuffers;
            std::unordered_map<std::string, std::vector<Ref<StorageBuffer>>> m_StorageBuffers;
            std::unordered_map<std::string, std::vector<Ref<Texture2D>>> m_Textures2D;
            std::unordered_map<std::string, std::vector<Ref<TextureCubemap>>> m_TexturesCube;
            
            // Array resources
            std::unordered_map<std::string, std::vector<Ref<UniformBufferArray>>> m_UniformBufferArrays;
            std::unordered_map<std::string, std::vector<Ref<StorageBufferArray>>> m_StorageBufferArrays;
            std::unordered_map<std::string, std::vector<Ref<Texture2DArray>>> m_Texture2DArrays;
            std::unordered_map<std::string, std::vector<Ref<TextureCubemapArray>>> m_TextureCubeArrays;
        };
        
        std::vector<FrameResources> m_FrameResources;
        
        // Resource metadata for creation
        struct ResourceMetadata
        {
            u32 size = 0;
            BufferUsage usage = BufferUsage::Dynamic;
            u32 arraySize = 0; // For array resources
            bool isArray = false;
        };
        
        std::unordered_map<std::string, ResourceMetadata> m_ResourceMetadata;

    public:
        /**
         * @brief Constructor
         * @param framesInFlight Number of frames to keep in flight (default: 3)
         */
        explicit FrameInFlightManager(u32 framesInFlight = DEFAULT_FRAMES_IN_FLIGHT)
            : m_FramesInFlight(framesInFlight)
        {
            m_FrameResources.resize(framesInFlight);
            OLO_CORE_INFO("FrameInFlightManager initialized with {0} frames in flight", framesInFlight);
        }

        /**
         * @brief Register a uniform buffer for frame-in-flight management
         * @param name Resource name
         * @param size Buffer size in bytes
         * @param binding Buffer binding point
         * @param usage Buffer usage pattern
         * @param initialData Optional initial data
         */
        void RegisterUniformBuffer(const std::string& name, u32 size, u32 binding, BufferUsage usage = BufferUsage::Dynamic, const void* initialData = nullptr)
        {
            m_ResourceMetadata[name] = { size, usage, binding, false };
            
            for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
            {
                auto buffer = UniformBuffer::Create(size, binding);
                if (initialData)
                {
                    buffer->SetData(initialData, size);
                }
                m_FrameResources[frame].m_UniformBuffers[name].push_back(buffer);
            }
            
            OLO_CORE_TRACE("Registered UniformBuffer '{0}' for {1} frames (size: {2} bytes, binding: {3})", name, m_FramesInFlight, size, binding);
        }

        /**
         * @brief Register a storage buffer for frame-in-flight management
         * @param name Resource name
         * @param size Buffer size in bytes
         * @param usage Buffer usage pattern
         * @param initialData Optional initial data
         */
        void RegisterStorageBuffer(const std::string& name, u32 size, BufferUsage usage = BufferUsage::Dynamic, const void* initialData = nullptr)
        {
            m_ResourceMetadata[name] = { size, usage, 0, false };
            
            for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
            {
                auto buffer = StorageBuffer::Create(size, initialData, usage);
                m_FrameResources[frame].m_StorageBuffers[name].push_back(buffer);
            }
            
            OLO_CORE_TRACE("Registered StorageBuffer '{0}' for {1} frames (size: {2} bytes)", name, m_FramesInFlight, size);
        }

        /**
         * @brief Register a uniform buffer array for frame-in-flight management
         * @param name Resource name
         * @param baseBindingPoint Starting binding point
         * @param arraySize Maximum number of buffers in the array
         * @param elementSize Size of each buffer element
         * @param usage Buffer usage pattern
         */
        void RegisterUniformBufferArray(const std::string& name, u32 baseBindingPoint, u32 arraySize, u32 elementSize, BufferUsage usage = BufferUsage::Dynamic)
        {
            m_ResourceMetadata[name] = { elementSize, usage, arraySize, true };
            
            for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
            {
                auto bufferArray = CreateRef<UniformBufferArray>(name + "_frame_" + std::to_string(frame), baseBindingPoint, arraySize);
                m_FrameResources[frame].m_UniformBufferArrays[name].push_back(bufferArray);
            }
            
            OLO_CORE_TRACE("Registered UniformBufferArray '{0}' for {1} frames (array size: {2}, element size: {3} bytes)", 
                          name, m_FramesInFlight, arraySize, elementSize);
        }

        /**
         * @brief Register a storage buffer array for frame-in-flight management
         * @param name Resource name
         * @param baseBindingPoint Starting binding point
         * @param arraySize Maximum number of buffers in the array
         * @param elementSize Size of each buffer element
         * @param usage Buffer usage pattern
         */
        void RegisterStorageBufferArray(const std::string& name, u32 baseBindingPoint, u32 arraySize, u32 elementSize, BufferUsage usage = BufferUsage::Dynamic)
        {
            m_ResourceMetadata[name] = { elementSize, usage, arraySize, true };
            
            for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
            {
                auto bufferArray = CreateRef<StorageBufferArray>(name + "_frame_" + std::to_string(frame), baseBindingPoint, arraySize);
                m_FrameResources[frame].m_StorageBufferArrays[name].push_back(bufferArray);
            }
            
            OLO_CORE_TRACE("Registered StorageBufferArray '{0}' for {1} frames (array size: {2}, element size: {3} bytes)", 
                          name, m_FramesInFlight, arraySize, elementSize);
        }

        /**
         * @brief Get the current frame's uniform buffer
         * @param name Resource name
         * @param index Array index (for multiple buffers with same name)
         * @return Current frame's uniform buffer
         */
        Ref<UniformBuffer> GetCurrentUniformBuffer(const std::string& name, u32 index = 0) const
        {
            const auto& frameResources = m_FrameResources[m_CurrentFrameIndex];
            auto it = frameResources.m_UniformBuffers.find(name);
            
            if (it != frameResources.m_UniformBuffers.end() && index < it->second.size())
            {
                return it->second[index];
            }
            
            OLO_CORE_WARN("UniformBuffer '{0}' not found for current frame {1}", name, m_CurrentFrameIndex);
            return nullptr;
        }

        /**
         * @brief Get the current frame's storage buffer
         * @param name Resource name
         * @param index Array index (for multiple buffers with same name)
         * @return Current frame's storage buffer
         */
        Ref<StorageBuffer> GetCurrentStorageBuffer(const std::string& name, u32 index = 0) const
        {
            const auto& frameResources = m_FrameResources[m_CurrentFrameIndex];
            auto it = frameResources.m_StorageBuffers.find(name);
            
            if (it != frameResources.m_StorageBuffers.end() && index < it->second.size())
            {
                return it->second[index];
            }
            
            OLO_CORE_WARN("StorageBuffer '{0}' not found for current frame {1}", name, m_CurrentFrameIndex);
            return nullptr;
        }

        /**
         * @brief Get the current frame's uniform buffer array
         * @param name Resource name
         * @param index Array index (for multiple arrays with same name)
         * @return Current frame's uniform buffer array
         */
        Ref<UniformBufferArray> GetCurrentUniformBufferArray(const std::string& name, u32 index = 0) const
        {
            const auto& frameResources = m_FrameResources[m_CurrentFrameIndex];
            auto it = frameResources.m_UniformBufferArrays.find(name);
            
            if (it != frameResources.m_UniformBufferArrays.end() && index < it->second.size())
            {
                return it->second[index];
            }
            
            OLO_CORE_WARN("UniformBufferArray '{0}' not found for current frame {1}", name, m_CurrentFrameIndex);
            return nullptr;
        }

        /**
         * @brief Get the current frame's storage buffer array
         * @param name Resource name
         * @param index Array index (for multiple arrays with same name)
         * @return Current frame's storage buffer array
         */
        Ref<StorageBufferArray> GetCurrentStorageBufferArray(const std::string& name, u32 index = 0) const
        {
            const auto& frameResources = m_FrameResources[m_CurrentFrameIndex];
            auto it = frameResources.m_StorageBufferArrays.find(name);
            
            if (it != frameResources.m_StorageBufferArrays.end() && index < it->second.size())
            {
                return it->second[index];
            }
            
            OLO_CORE_WARN("StorageBufferArray '{0}' not found for current frame {1}", name, m_CurrentFrameIndex);
            return nullptr;
        }

        /**
         * @brief Advance to the next frame in the sequence
         * Call this at the beginning of each frame
         */
        void NextFrame()
        {
            m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % m_FramesInFlight;
            m_FrameNumber++;
            
            OLO_CORE_TRACE("Advanced to frame {0} (index {1})", m_FrameNumber, m_CurrentFrameIndex);
        }

        /**
         * @brief Get the current frame index
         */
        u32 GetCurrentFrameIndex() const { return m_CurrentFrameIndex; }

        /**
         * @brief Get the total frame number
         */
        u32 GetFrameNumber() const { return m_FrameNumber; }

        /**
         * @brief Get the number of frames in flight
         */
        u32 GetFramesInFlight() const { return m_FramesInFlight; }

        /**
         * @brief Check if a resource is registered
         * @param name Resource name
         * @return true if resource is registered
         */
        bool IsResourceRegistered(const std::string& name) const
        {
            return m_ResourceMetadata.find(name) != m_ResourceMetadata.end();
        }

        /**
         * @brief Get resource metadata
         * @param name Resource name
         * @return Resource metadata if found
         */
        const ResourceMetadata* GetResourceMetadata(const std::string& name) const
        {
            auto it = m_ResourceMetadata.find(name);
            return (it != m_ResourceMetadata.end()) ? &it->second : nullptr;
        }

        /**
         * @brief Clear all resources and reset the manager
         */
        void Clear()
        {
            for (auto& frameRes : m_FrameResources)
            {
                frameRes.m_UniformBuffers.clear();
                frameRes.m_StorageBuffers.clear();
                frameRes.m_Textures2D.clear();
                frameRes.m_TexturesCube.clear();
                frameRes.m_UniformBufferArrays.clear();
                frameRes.m_StorageBufferArrays.clear();
                frameRes.m_Texture2DArrays.clear();
                frameRes.m_TextureCubeArrays.clear();
            }
            
            m_ResourceMetadata.clear();
            m_CurrentFrameIndex = 0;
            m_FrameNumber = 0;
            
            OLO_CORE_INFO("FrameInFlightManager cleared");
        }

        /**
         * @brief Get statistics about frame-in-flight resources
         */
        struct Statistics
        {
            u32 TotalUniformBuffers = 0;
            u32 TotalStorageBuffers = 0;
            u32 TotalTextures = 0;
            u32 TotalArrayResources = 0;
            sizet TotalMemoryUsage = 0;
        };

        Statistics GetStatistics() const
        {
            Statistics stats;
            
            for (const auto& frameRes : m_FrameResources)
            {
                stats.TotalUniformBuffers += static_cast<u32>(frameRes.m_UniformBuffers.size());
                stats.TotalStorageBuffers += static_cast<u32>(frameRes.m_StorageBuffers.size());
                stats.TotalTextures += static_cast<u32>(frameRes.m_Textures2D.size() + frameRes.m_TexturesCube.size());
                stats.TotalArrayResources += static_cast<u32>(frameRes.m_UniformBufferArrays.size() + 
                                           frameRes.m_StorageBufferArrays.size() + 
                                           frameRes.m_Texture2DArrays.size() + 
                                           frameRes.m_TextureCubeArrays.size());
            }
            
            // Estimate memory usage
            for (const auto& [name, metadata] : m_ResourceMetadata)
            {
                sizet resourceMemory = metadata.size;
                if (metadata.isArray)
                    resourceMemory *= metadata.arraySize;
                stats.TotalMemoryUsage += resourceMemory * m_FramesInFlight;
            }
            
            return stats;
        }
    };
}
