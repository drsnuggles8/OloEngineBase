#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Buffer.h"

namespace OloEngine
{
    /**
     * @brief Storage Buffer (SSBO) for large, structured data access in shaders
     * 
     * Storage buffers provide read/write access to large amounts of structured data
     * from shaders. Unlike uniform buffers, they can be much larger and support
     * dynamic indexing and atomic operations.
     */
    class StorageBuffer
    {
    protected:
        // CPU-side cache of the buffer data to allow reading
        void* m_LocalData = nullptr;
        u32 m_Size = 0;
        u32 m_BindingPoint = 0;
        bool m_IsReadWrite = true; // Whether the buffer supports read/write or is read-only

    public:
        virtual ~StorageBuffer() 
        {
            if (m_LocalData)
                delete[] static_cast<u8*>(m_LocalData);
        }
        
        /**
         * @brief Set data in the storage buffer
         * @param data Pointer to the data
         * @param size Size of the data in bytes
         * @param offset Offset in bytes from the start of the buffer
         */
        virtual void SetData(const void* data, u32 size, u32 offset = 0) = 0;
        
        /**
         * @brief Get data from the storage buffer (if supported)
         * @param data Pointer to write the data to
         * @param size Size of the data to read in bytes
         * @param offset Offset in bytes from the start of the buffer
         */
        virtual void GetData(void* data, u32 size, u32 offset = 0) = 0;
        
        /**
         * @brief Template method to set typed data
         */
        template<typename T>
        void SetData(const T& data, u32 offset = 0)
        {
            SetData(&data, sizeof(T), offset);
        }
        
        /**
         * @brief Template method to set array data
         */
        template<typename T>
        void SetDataArray(const std::vector<T>& data, u32 offset = 0)
        {
            SetData(data.data(), static_cast<u32>(data.size() * sizeof(T)), offset);
        }
        
        /**
         * @brief Template method to get typed data
         */
        template<typename T>
        T GetData(u32 offset = 0)
        {
            T result;
            GetData(&result, sizeof(T), offset);
            return result;
        }
        
        /**
         * @brief Template method to get array data
         */
        template<typename T>
        std::vector<T> GetDataArray(u32 count, u32 offset = 0)
        {
            std::vector<T> result(count);
            GetData(result.data(), static_cast<u32>(count * sizeof(T)), offset);
            return result;
        }
        
        /**
         * @brief Bind the storage buffer to a binding point
         * @param bindingPoint The binding point to bind to
         */
        virtual void Bind(u32 bindingPoint) = 0;
        
        /**
         * @brief Unbind the storage buffer
         */
        virtual void Unbind() = 0;
        
        /**
         * @brief Get the size of the buffer in bytes
         */
        u32 GetSize() const { return m_Size; }
        
        /**
         * @brief Phase 6.1: Get renderer ID for handle caching
         */
        virtual u32 GetRendererID() const = 0;
        
        /**
         * @brief Get the current binding point
         */
        u32 GetBindingPoint() const { return m_BindingPoint; }
        
        /**
         * @brief Check if the buffer supports read/write operations
         */
        bool IsReadWrite() const { return m_IsReadWrite; }
        
        /**
         * @brief Create a storage buffer
         * @param size Size of the buffer in bytes
         * @param data Initial data (optional)
         * @param usage Buffer usage pattern
         */
        static Ref<StorageBuffer> Create(u32 size, const void* data = nullptr, BufferUsage usage = BufferUsage::Dynamic);
        
        /**
         * @brief Create a storage buffer with typed data
         */
        template<typename T>
        static Ref<StorageBuffer> Create(const T& data, BufferUsage usage = BufferUsage::Dynamic)
        {
            return Create(sizeof(T), &data, usage);
        }
        
        /**
         * @brief Create a storage buffer with array data
         */
        template<typename T>
        static Ref<StorageBuffer> Create(const std::vector<T>& data, BufferUsage usage = BufferUsage::Dynamic)
        {
            return Create(static_cast<u32>(data.size() * sizeof(T)), data.data(), usage);
        }

    protected:
        /**
         * @brief Update the CPU-side cache
         */
        void UpdateLocalData(const void* data, u32 size, u32 offset = 0)
        {
            // Allocate local buffer if needed
            if (!m_LocalData && size > 0 && offset == 0)
            {
                m_LocalData = new u8[size];
                m_Size = size;
            }
            
            // Resize if needed
            if (m_LocalData && offset + size > m_Size)
            {
                u8* newData = new u8[offset + size];
                if (m_Size > 0)
                    std::memcpy(newData, m_LocalData, m_Size);
                delete[] static_cast<u8*>(m_LocalData);
                m_LocalData = newData;
                m_Size = offset + size;
            }

            if (m_LocalData && offset + size <= m_Size)
            {
                std::memcpy(static_cast<u8*>(m_LocalData) + offset, data, size);
            }
        }
    };
}
