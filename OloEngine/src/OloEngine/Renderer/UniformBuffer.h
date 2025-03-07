#include <utility>
#include "OloEngine/Renderer/Buffer.h"

#pragma once

namespace OloEngine
{
	// TODO(olbu): Add Create() functions for the new constructors of OpenGLUniformBuffer
	class UniformBuffer
	{
	public:
		virtual ~UniformBuffer() = default;
		
		// Original method using UniformData struct
		virtual void SetData(const UniformData& data) = 0;
		
		// New convenience method to set data directly
		virtual void SetData(const void* data, u32 size, u32 offset = 0)
		{
			UniformData uniformData;
			uniformData.data = data;
			uniformData.size = size;
			uniformData.offset = offset;
			SetData(uniformData);
		}

		static Ref<UniformBuffer> Create(u32 size, u32 binding);
	};
}
