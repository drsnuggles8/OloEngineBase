#pragma once

#include "OloEngine/Renderer/UniformBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
	class OpenGLUniformBuffer : public UniformBuffer
	{
	public:
		OpenGLUniformBuffer(const u32 size, const u32 binding);
		OpenGLUniformBuffer(const u32 size, const u32 binding, const GLenum usage);
		~OpenGLUniformBuffer() override;

		void SetData(const UniformData& data) override;
	private:
		u32 m_RendererID = 0;
	};
}
