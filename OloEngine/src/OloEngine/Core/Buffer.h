#pragma once

#include "OloEngine/Core/Base.h"

#include <stdint.h>
#include <cstring>
#include <span>
#include <limits>
#include <stdexcept>

namespace OloEngine
{
	/// Non-owning raw buffer class for managing memory blocks
	/// @note Buffer operations may throw exceptions on critical failures like memory overflow
	struct Buffer
	{
		u8* Data = nullptr;
		u64 Size = 0;

		Buffer() = default;

		Buffer(u64 size)
		{
			Allocate(size);
		}

		Buffer(const Buffer&) = default;

		static Buffer Copy(Buffer other)
		{
			Buffer result(other.Size);
			::memcpy(result.Data, other.Data, other.Size);
			return result;
		}

		/// Creates a buffer by copying data from a span
		/// @param data The span of bytes to copy
		/// @return A new Buffer containing a copy of the span data
		/// @throws std::length_error if span size exceeds u64 limits (only on platforms where size_t > u64)
		/// @note Empty spans result in an empty buffer (not an error condition)
		[[nodiscard]] static Buffer Copy(std::span<const u8> data)
		{
			// Early return for empty spans to avoid calling memcpy with zero length
			if (data.size() == 0)
				return Buffer{};
				
			// Validate that span size fits in u64 to avoid truncation (only when size_t > u64)
			if constexpr (sizeof(sizet) > sizeof(u64))
			{
				if (data.size() > std::numeric_limits<u64>::max())
				{
					throw std::length_error("Buffer::Copy: span size exceeds u64 maximum - cannot copy data of this size");
				}
			}
				
			Buffer result(static_cast<u64>(data.size()));
			::memcpy(result.Data, data.data(), data.size());
			return result;
		}

		void Allocate(u64 size)
		{
			Release();

			Data = new u8[size];
			Size = size;
		}

		void Release()
		{
			delete[] Data;
			Data = nullptr;
			Size = 0;
		}

		template<typename T>
		T* As()
		{
			return (T*)Data;
		}

		operator bool() const
		{
			return (bool)Data;
		}

	};

	struct ScopedBuffer
	{
		ScopedBuffer(Buffer buffer)
			: m_Buffer(buffer)
		{
		}

		ScopedBuffer(u64 size)
			: m_Buffer(size)
		{
		}

		~ScopedBuffer()
		{
			m_Buffer.Release();
		}

		[[nodiscard("Store this!")]] u8 * Data() const { return m_Buffer.Data; }
		[[nodiscard("Store this!")]] u64 Size() const { return m_Buffer.Size; }

		template<typename T>
		T* As()
		{
			return m_Buffer.As<T>();
		}

		operator bool() const { return m_Buffer; }
	private:
		Buffer m_Buffer;
	};


}
