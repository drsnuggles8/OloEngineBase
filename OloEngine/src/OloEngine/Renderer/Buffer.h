#include <utility>

#pragma once

namespace OloEngine
{

	enum class ShaderDataType
	{
		None = 0, Float, Float2, Float3, Float4, Mat3, Mat4, Int, Int2, Int3, Int4, Bool
	};

	[[nodiscard("Store this!")]] static u32 ShaderDataTypeSize(ShaderDataType const type)
	{
		switch (type)
		{
			using enum OloEngine::ShaderDataType;
			case Float:    return 4;
			case Float2:   return 4 * 2;
			case Float3:   return 4 * 3;
			case Float4:   return 4 * 4;
			case Mat3:     return 4 * 3 * 3;
			case Mat4:     return 4 * 4 * 4;
			case Int:      return 4;
			case Int2:     return 4 * 2;
			case Int3:     return 4 * 3;
			case Int4:     return 4 * 4;
			case Bool:     return 1;
			case None:     break;
		}

		OLO_CORE_ASSERT(false, "Unknown ShaderDataType!");
		return 0;
	}

	struct BufferElement
	{
		std::string name;
		ShaderDataType dataType{};
		u32 size{};
		sizet offset{};
		bool normalized{};

		BufferElement() = default;

		BufferElement(ShaderDataType const type, const std::string& name, const bool normalized = false)
			: name(name), dataType(type), size(ShaderDataTypeSize(type)), offset(0), normalized(normalized)
		{
		}

		[[nodiscard("Store this!")]] u32 GetComponentCount() const
		{
			switch (dataType)
			{
				using enum OloEngine::ShaderDataType;
				case Float:   return 1;
				case Float2:  return 2;
				case Float3:  return 3;
				case Float4:  return 4;
				case Mat3:    return 3; // 3* float3
				case Mat4:    return 4; // 4* float4
				case Int:     return 1;
				case Int2:    return 2;
				case Int3:    return 3;
				case Int4:    return 4;
				case Bool:    return 1;
				case None:    break;
			}

			OLO_CORE_ASSERT(false, "Unknown ShaderDataType!");
			return 0;
		}
	};

	struct VertexData
	{
		const void* data;
		u32 size;
	};

	struct UniformData
	{
		const void* data;
		u32 size;
		u32 offset;
	};

	class BufferLayout
	{
	public:
		BufferLayout() = default;

		BufferLayout(const std::initializer_list<BufferElement>& elements)
			: m_Elements(elements)
		{
			CalculateOffsetsAndStride();
		}

		[[nodiscard("Store this!")]] u32 GetStride() const { return m_Stride; }
		[[nodiscard("Store this!")]] std::vector<BufferElement> GetElements() const { return m_Elements; }

		std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
		std::vector<BufferElement>::iterator end() { return m_Elements.end(); }
		[[nodiscard("Store this!")]] std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
		[[nodiscard("Store this!")]] std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }
	private:
		void CalculateOffsetsAndStride()
		{
			sizet offset = 0;
			m_Stride = 0;
			for (auto& element : m_Elements)
			{
				element.offset = offset;
				offset += element.size;
				m_Stride += element.size;
			}
		}
	private:
		std::vector<BufferElement> m_Elements;
		u32 m_Stride = 0;
	};

	// TODO(olbu): Add Create() functions for the new constructors of OpenGLVertexBuffer, OpenGLIndexBuffer, OpenGLUniformBuffer
	class VertexBuffer
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
	};

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

	class UniformBuffer
	{
	public:
		virtual ~UniformBuffer() = default;
		virtual void SetData(const UniformData& data) = 0;

		static Ref<UniformBuffer> Create(u32 size, u32 binding);
	};
}
