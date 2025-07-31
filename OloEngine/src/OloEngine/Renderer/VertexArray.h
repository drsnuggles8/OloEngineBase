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

		virtual void AddVertexBuffer(const AssetRef<VertexBuffer>& vertexBuffer) = 0;
		virtual void SetIndexBuffer(const AssetRef<IndexBuffer>& indexBuffer) = 0;

		[[nodiscard("Store this!")]] virtual const std::vector<AssetRef<VertexBuffer>>& GetVertexBuffers() const = 0;
		[[nodiscard("Store this!")]] virtual const AssetRef<IndexBuffer>& GetIndexBuffer() const = 0;

		static AssetRef<VertexArray> Create();

		[[nodiscard]] virtual u32 GetRendererID() const = 0;
	};
}
