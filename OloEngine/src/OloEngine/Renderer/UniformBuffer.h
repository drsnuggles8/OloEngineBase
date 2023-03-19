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
		virtual void SetData(const UniformData& data) = 0;

		static Ref<UniformBuffer> Create(u32 size, u32 binding);
	};
}
