#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine
{
    /**
     * @brief Array container for shader resources supporting batched binding operations
     * 
     * Provides efficient management of arrays of shader resources (buffers, textures)
     * with support for contiguous binding, partial updates, and automatic resizing.
     * This enables advanced rendering techniques like GPU-driven rendering and
     * multi-material batching.
     * 
     * @tparam ResourceType The type of resources stored in the array (e.g., StorageBuffer, Texture2D)
     */
    template<typename ResourceType>
    class ArrayResource
    {
    private:
        std::vector<Ref<ResourceType>> m_Resources;
        u32 m_BaseBindingPoint = 0;
        u32 m_MaxSize = 0;
        std::string m_Name;
        bool m_IsBound = false;

    public:
        /**
         * @brief Constructor for array resource
         * @param name Resource array name
         * @param baseBindingPoint Starting binding point for the array
         * @param maxSize Maximum number of resources in the array
         */
        ArrayResource(const std::string& name, u32 baseBindingPoint, u32 maxSize = 32)
            : m_Name(name), m_BaseBindingPoint(baseBindingPoint), m_MaxSize(maxSize)
        {
            m_Resources.reserve(maxSize);
        }

        /**
         * @brief Add a resource to the array
         * @param resource Resource to add
         * @return Index of the added resource, or UINT32_MAX if array is full
         */
        u32 AddResource(Ref<ResourceType> resource)
        {
            if (m_Resources.size() >= m_MaxSize)
            {
                OLO_CORE_ERROR("ArrayResource '{0}' is full (max size: {1})", m_Name, m_MaxSize);
                return UINT32_MAX;
            }

            m_Resources.push_back(resource);
            return static_cast<u32>(m_Resources.size() - 1);
        }

        /**
         * @brief Set a resource at a specific index
         * @param index Index to set the resource at
         * @param resource Resource to set
         * @return true if successful, false if index is out of bounds
         */
        bool SetResource(u32 index, Ref<ResourceType> resource)
        {
            if (index >= m_MaxSize)
            {
                OLO_CORE_ERROR("Index {0} out of bounds for ArrayResource '{1}' (max size: {2})", 
                              index, m_Name, m_MaxSize);
                return false;
            }

            // Resize vector if necessary
            if (index >= m_Resources.size())
            {
                m_Resources.resize(index + 1);
            }

            m_Resources[index] = resource;
            return true;
        }

        /**
         * @brief Get a resource at a specific index
         * @param index Index of the resource
         * @return Resource at the index, or nullptr if not found
         */
        Ref<ResourceType> GetResource(u32 index) const
        {
            if (index >= m_Resources.size())
                return nullptr;
            return m_Resources[index];
        }

        /**
         * @brief Remove a resource at a specific index
         * @param index Index to remove
         * @return true if successful, false if index is out of bounds
         */
        bool RemoveResource(u32 index)
        {
            if (index >= m_Resources.size())
                return false;

            m_Resources[index] = nullptr;
            return true;
        }

        /**
         * @brief Bind all resources in the array to consecutive binding points
         */
        void BindArray()
        {
            for (u32 i = 0; i < m_Resources.size(); ++i)
            {
                auto& resource = m_Resources[i];
                if (resource)
                {
                    u32 bindingPoint = m_BaseBindingPoint + i;
                    
                    // Call appropriate bind method based on resource type
                    if constexpr (std::is_same_v<ResourceType, StorageBuffer>)
                    {
                        resource->Bind(bindingPoint);
                    }
                    else if constexpr (std::is_same_v<ResourceType, Texture2D> || 
                                     std::is_same_v<ResourceType, TextureCubemap>)
                    {
                        resource->Bind(bindingPoint);
                    }
                    // Add other resource types as needed
                }
            }
            
            m_IsBound = true;
            OLO_CORE_TRACE("ArrayResource '{0}' bound to binding points {1}-{2}", 
                          m_Name, m_BaseBindingPoint, m_BaseBindingPoint + m_Resources.size() - 1);
        }

        /**
         * @brief Bind a specific range of resources in the array
         * @param startIndex Starting index in the array
         * @param count Number of resources to bind
         */
        void BindRange(u32 startIndex, u32 count)
        {
            u32 endIndex = std::min(startIndex + count, static_cast<u32>(m_Resources.size()));
            
            for (u32 i = startIndex; i < endIndex; ++i)
            {
                auto& resource = m_Resources[i];
                if (resource)
                {
                    u32 bindingPoint = m_BaseBindingPoint + i;
                    
                    if constexpr (std::is_same_v<ResourceType, StorageBuffer>)
                    {
                        resource->Bind(bindingPoint);
                    }
                    else if constexpr (std::is_same_v<ResourceType, Texture2D> || 
                                     std::is_same_v<ResourceType, TextureCubemap>)
                    {
                        resource->Bind(bindingPoint);
                    }
                }
            }
            
            OLO_CORE_TRACE("ArrayResource '{0}' range bound: indices {1}-{2} to binding points {3}-{4}", 
                          m_Name, startIndex, endIndex - 1, 
                          m_BaseBindingPoint + startIndex, m_BaseBindingPoint + endIndex - 1);
        }

        /**
         * @brief Unbind all resources in the array
         */
        void UnbindArray()
        {
            for (auto& resource : m_Resources)
            {
                if (resource)
                {
                    if constexpr (std::is_same_v<ResourceType, StorageBuffer>)
                    {
                        resource->Unbind();
                    }
                    // Textures don't have explicit unbind in our current implementation
                }
            }
            
            m_IsBound = false;
            OLO_CORE_TRACE("ArrayResource '{0}' unbound", m_Name);
        }

        /**
         * @brief Get the number of resources currently in the array
         */
        u32 GetResourceCount() const { return static_cast<u32>(m_Resources.size()); }

        /**
         * @brief Get the maximum capacity of the array
         */
        u32 GetMaxSize() const { return m_MaxSize; }

        /**
         * @brief Get the base binding point
         */
        u32 GetBaseBindingPoint() const { return m_BaseBindingPoint; }

        /**
         * @brief Get the name of the array resource
         */
        const std::string& GetName() const { return m_Name; }

        /**
         * @brief Check if the array is currently bound
         */
        bool IsBound() const { return m_IsBound; }

        /**
         * @brief Clear all resources from the array
         */
        void Clear()
        {
            if (m_IsBound)
            {
                UnbindArray();
            }
            m_Resources.clear();
        }

        /**
         * @brief Resize the array to a new capacity
         * @param newMaxSize New maximum size
         */
        void Resize(u32 newMaxSize)
        {
            if (newMaxSize < m_Resources.size())
            {
                OLO_CORE_WARN("Resizing ArrayResource '{0}' to {1} will truncate {2} existing resources", 
                             m_Name, newMaxSize, m_Resources.size() - newMaxSize);
                m_Resources.resize(newMaxSize);
            }
            
            m_MaxSize = newMaxSize;
            m_Resources.reserve(newMaxSize);
        }

        /**
         * @brief Get iterator to the beginning of the resource array
         */
        auto begin() { return m_Resources.begin(); }
        auto begin() const { return m_Resources.begin(); }

        /**
         * @brief Get iterator to the end of the resource array
         */
        auto end() { return m_Resources.end(); }
        auto end() const { return m_Resources.end(); }
    };

    // Type aliases for common array resource types
    using StorageBufferArray = ArrayResource<StorageBuffer>;
    using UniformBufferArray = ArrayResource<UniformBuffer>;
    using Texture2DArray = ArrayResource<Texture2D>;
    using TextureCubemapArray = ArrayResource<TextureCubemap>;
}
