#pragma once

#include <utility>
#include "OloEngine/Renderer/Buffer.h"

namespace OloEngine
{
	// TODO(olbu): Add Create() functions for the new constructors of OpenGLUniformBuffer
	class UniformBuffer
	{
	protected:
		// CPU-side cache of the buffer data to allow reading
		void* m_LocalData = nullptr;
		u32 m_Size = 0;

	public:
		virtual ~UniformBuffer() 
		{
			if (m_LocalData)
				delete[] m_LocalData;
		}
		
		// Original method using UniformData struct
		virtual void SetData(const UniformData& data) = 0;
		
		// Phase 6.1: Resource handle caching support
		virtual u32 GetRendererID() const = 0;
		virtual u32 GetSize() const { return m_Size; }
		
		// New convenience method to set data directly
		virtual void SetData(const void* data, u32 size, u32 offset = 0)
		{
			// Store the data in our CPU-side buffer for later reading
			if (!m_LocalData && size > 0 && offset == 0)
			{
				m_LocalData = new u8[size];
				m_Size = size;
			}

			if (m_LocalData && offset + size <= m_Size)
			{
				// Update our local copy
				std::memcpy(static_cast<u8*>(m_LocalData) + offset, data, size);
			}
			
			// Send to GPU
			UniformData uniformData;
			uniformData.data = data;
			uniformData.size = size;
			uniformData.offset = offset;
			SetData(uniformData);
		}

		// New method to read data back from the CPU-side cache
		template<typename T>
		T GetData() const
		{
			OLO_CORE_ASSERT(m_LocalData != nullptr, "Cannot read from uninitialized UBO data!");
			OLO_CORE_ASSERT(sizeof(T) <= m_Size, "Type size exceeds UBO size!");
			
			T result;
			std::memcpy(&result, m_LocalData, sizeof(T));
			return result;
		}

		static Ref<UniformBuffer> Create(u32 size, u32 binding);
	};
}
