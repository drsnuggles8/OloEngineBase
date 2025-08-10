#pragma once

#include <cstddef>
#include <utility>
#include <type_traits>
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
	// TODO(olbu): Add Create() functions for the new constructors of OpenGLVertexBuffer
	class VertexBuffer : public RefCounted
	{
	public:
		virtual ~VertexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual void SetData(const VertexData& data) = 0;

		[[nodiscard("Store this!")]] virtual const BufferLayout& GetLayout() const = 0;
		virtual void SetLayout(const BufferLayout& layout) = 0;

		[[nodiscard("Store this!")]] virtual u32 GetBufferHandle() const = 0;

		static Ref<VertexBuffer> Create(u32 size);
		static Ref<VertexBuffer> Create(f32* vertices, u32 size);
		static Ref<VertexBuffer> Create(const f32* vertices, u32 size);
		static Ref<VertexBuffer> Create(const void* data, u32 size);
		
		template<typename T, typename = std::enable_if_t<
			(std::is_integral_v<T> || std::is_enum_v<T>) && !std::is_same_v<T, u32>>>
		static Ref<VertexBuffer> Create(T, u32) = delete;
	};
}
