#include <utility>

#pragma once

namespace OloEngine
{
	// Currently OloEngine only supports 32-bit index buffers
	class IndexBuffer
	{
	public:
		virtual ~IndexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		[[nodiscard("Store this!")]] virtual u32 GetCount() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetBufferHandle() const = 0;

		static Ref<IndexBuffer> Create(u32* indices, u32 size);
	};
}
