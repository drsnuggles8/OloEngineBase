#pragma once

#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Core/Ref.h"

#include <memory>

namespace OloEngine
{
	class VertexArray : public RefCounted
	{
	public:
		virtual ~VertexArray() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) = 0;
		virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) = 0;

		[[nodiscard("Store this!")]] virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const = 0;
		[[nodiscard("Store this!")]] virtual const Ref<IndexBuffer>& GetIndexBuffer() const = 0;

		static Ref<VertexArray> Create();

		[[nodiscard]] virtual u32 GetRendererID() const = 0;
	};
}
