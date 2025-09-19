#pragma once

#include <stdint.h>
#include <cstring>
#include <span>

namespace OloEngine
{
	// Non-owning raw buffer class
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

		static Buffer Copy(std::span<const u8> data)
		{
			Buffer result(data.size());
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
