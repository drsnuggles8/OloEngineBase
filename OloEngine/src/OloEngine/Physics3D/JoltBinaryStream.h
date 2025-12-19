#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Buffer.h"

#include <vector>
#include <cstring>
#include <span>
#include <limits>

// Forward declarations for types we need but don't want to include full headers
namespace JPH {
	class Shape;
	enum class EShapeType : u8;
	template<typename T> class Ref;
	class StreamIn;
	class StreamOut;
}

namespace OloEngine {

	// @brief Binary stream reader for Jolt Physics serialization
	// 
	// Reads binary data from OloEngine Buffer objects for deserializing physics shapes.
	class JoltBinaryStreamReader
	{
	public:
		// @brief Constructs a reader from a Buffer.
		// @warning The reader does NOT take ownership of the memory. The caller must ensure 
		//          the provided buffer outlives this reader instance to avoid dangling pointers.
		// @note For safe usage: pass an owning Buffer, use std::shared_ptr/unique_ptr for dynamic data,
		//       or ensure stack/temporary data remains valid during reader lifetime.
		// @param buffer The buffer to read from (must remain valid during reader lifetime)
		explicit JoltBinaryStreamReader(const Buffer& buffer)
			: m_Data(buffer.Data), m_Size(buffer.Size), m_ReadBytes(0), m_Failed(false)
		{
			OLO_CORE_ASSERT(buffer.Data != nullptr && buffer.Size > 0, "Invalid buffer provided to JoltBinaryStreamReader");
		}

		// @brief Constructs a reader from raw pointer and size.
		// @warning The reader does NOT take ownership of the memory. The caller must ensure 
		//          the provided data outlives this reader instance to avoid dangling pointers.
		// @note For safe usage: use with stack arrays, string literals, or ensure dynamic memory
		//       remains valid. For untrusted input, consider copying into an owned buffer first.
		// @param data Pointer to data to read from (must remain valid during reader lifetime)
		// @param size Size of the data in bytes
		explicit JoltBinaryStreamReader(const u8* data, u64 size)
			: m_Data(data), m_Size(size), m_ReadBytes(0), m_Failed(false)
		{
			OLO_CORE_ASSERT(data != nullptr && size > 0, "Invalid data provided to JoltBinaryStreamReader");
		}

		~JoltBinaryStreamReader() = default;

		// Make non-copyable but movable
		JoltBinaryStreamReader(const JoltBinaryStreamReader&) = delete;
		JoltBinaryStreamReader& operator=(const JoltBinaryStreamReader&) = delete;
		JoltBinaryStreamReader(JoltBinaryStreamReader&&) = default;
		JoltBinaryStreamReader& operator=(JoltBinaryStreamReader&&) = default;

		// Stream reading interface
		void ReadBytes(void* outData, sizet inNumBytes)
		{
			// Special case: reading zero bytes is always a no-op
			if (inNumBytes == 0)
			{
				return;
			}

			// Validate output pointer (only when we actually need to write data)
			if (!outData)
			{
				OLO_CORE_ERROR("JoltBinaryStreamReader: outData pointer is null");
				m_Failed = true;
				return;
			}

			if (IsEOF() || IsFailed())
			{
				OLO_CORE_ERROR("JoltBinaryStreamReader: Attempted to read past end of stream or from failed stream");
				return;
			}

			const u8* sourceData = GetSourceData();
			if (!sourceData)
			{
				OLO_CORE_ERROR("JoltBinaryStreamReader: Source data is null");
				m_Failed = true;
				return;
			}

			// Bounds checking before memcpy
			u64 totalSize = GetSourceSize();
			
			// Guard against underflow - check if m_ReadBytes is corrupt/greater than totalSize
			if (m_ReadBytes > totalSize)
			{
				OLO_CORE_ERROR("JoltBinaryStreamReader: Read position ({}) exceeds total size ({}), clamping and failing", 
					m_ReadBytes, totalSize);
				m_Failed = true;
				return;
			}
			
			u64 remaining = totalSize - m_ReadBytes;
			if (inNumBytes > remaining)
			{
				OLO_CORE_ERROR("JoltBinaryStreamReader: Requested {} bytes but only {} remaining (total size: {}, already read: {})", 
					inNumBytes, remaining, totalSize, m_ReadBytes);
				// Mark stream as failed but leave read position unchanged
				m_Failed = true;
				return;
			}

			::memcpy(outData, sourceData + m_ReadBytes, inNumBytes);
			m_ReadBytes += inNumBytes;
		}

		[[nodiscard]] bool IsEOF() const
		{ 
			return GetSourceSize() == 0 || m_ReadBytes >= GetSourceSize();
		}

		[[nodiscard]] bool IsFailed() const
		{
			// Check explicit failure flag first
			if (m_Failed)
				return true;
				
			// Check data validity
			return m_Data == nullptr || m_Size == 0;
		}

		// Additional utility methods
		[[nodiscard]] u64 GetBytesRead() const { return m_ReadBytes; }
		[[nodiscard]] u64 GetRemainingBytes() const 
		{ 
			u64 size = GetSourceSize();
			return m_ReadBytes < size ? size - m_ReadBytes : 0;
		}

		void Reset() { m_ReadBytes = 0; m_Failed = false; }

		// Seek to position (useful for debugging/validation)
		[[nodiscard]] bool Seek(u64 position)
		{
			if (position <= GetSourceSize())
			{
				m_ReadBytes = position;
				return true;
			}
			return false;
		}

	private:
		const u8* GetSourceData() const
		{
			return m_Data;
		}

		u64 GetSourceSize() const
		{
			return m_Size;
		}

	private:
		const u8* m_Data = nullptr;
		u64 m_Size = 0;
		u64 m_ReadBytes = 0;
		bool m_Failed = false;
	};

	// @brief Binary stream writer for Jolt Physics serialization
	// 
	// Writes binary data to expandable buffer for serializing physics shapes.
	class JoltBinaryStreamWriter
	{
	public:
		JoltBinaryStreamWriter()
			: m_Failed(false)
		{
			m_TempBuffer.reserve(1024); // Reserve some initial capacity
		}

		explicit JoltBinaryStreamWriter(sizet initialCapacity)
			: m_Failed(false)
		{
			m_TempBuffer.reserve(initialCapacity);
		}

		~JoltBinaryStreamWriter() = default;

		// Stream writing interface
		void WriteBytes(const void* inData, sizet inNumBytes)
		{
			if (m_Failed)
			{
				OLO_CORE_ERROR("JoltBinaryStreamWriter: Attempted to write to failed stream");
				return;
			}
			
			if (inData == nullptr || inNumBytes == 0)
			{
				if (inNumBytes == 0)
					return; // Valid case: zero bytes to write
				
				// Invalid case: null data with non-zero size
				OLO_CORE_ERROR("JoltBinaryStreamWriter: Null data pointer with non-zero size ({} bytes)", inNumBytes);
				m_Failed = true;
				return;
			}

			try 
			{
				sizet currentOffset = m_TempBuffer.size();
				
				// Check for size_t overflow before resizing (ensure type alignment)
				if (static_cast<size_t>(inNumBytes) > std::numeric_limits<size_t>::max() - static_cast<size_t>(currentOffset))
				{
					OLO_CORE_ERROR("JoltBinaryStreamWriter: Buffer size overflow - requested {} bytes would exceed maximum size", inNumBytes);
					m_Failed = true;
					return;
				}
				
				m_TempBuffer.resize(currentOffset + inNumBytes);
				::memcpy(m_TempBuffer.data() + currentOffset, inData, inNumBytes);
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("JoltBinaryStreamWriter: Failed to write {} bytes - {}", inNumBytes, e.what());
				m_Failed = true;
			}
		}

		[[nodiscard]] bool IsFailed() const { return m_Failed; }

		// Data access methods
		[[nodiscard]] const std::vector<u8>& GetData() const { return m_TempBuffer; }
		[[nodiscard]] const u8* GetDataPtr() const { return m_TempBuffer.data(); }
		[[nodiscard]] sizet GetSize() const { return m_TempBuffer.size(); }
		[[nodiscard]] bool IsEmpty() const { return m_TempBuffer.empty(); }

		// Create OloEngine Buffer from written data
		Buffer CreateBuffer() const
		{
			if (m_TempBuffer.empty())
				return Buffer();

			return Buffer::Copy(std::span{m_TempBuffer.data(), m_TempBuffer.size()});
		}

		// Create ScopedBuffer from written data
		ScopedBuffer CreateScopedBuffer() const
		{
			return ScopedBuffer(CreateBuffer());
		}

		// Utility methods
		void Clear() 
		{ 
			m_TempBuffer.clear(); 
			m_Failed = false;
		}

		void Reserve(sizet capacity) 
		{ 
			m_TempBuffer.reserve(capacity); 
		}

		// Get statistics for debugging
		[[nodiscard]] sizet GetCapacity() const { return m_TempBuffer.capacity(); }
		void ShrinkToFit() { m_TempBuffer.shrink_to_fit(); }

	private:
		std::vector<u8> m_TempBuffer;
		bool m_Failed = false;
	};

	// @brief Utility functions for Jolt shape serialization using binary streams
	namespace JoltBinaryStreamUtils {

		// Shape serialization functions
		bool SerializeShape(const JPH::Shape* shape, JoltBinaryStreamWriter& outWriter);
		JPH::Ref<JPH::Shape> DeserializeShape(JoltBinaryStreamReader& inReader);

		// Convenience functions for Buffer operations
		Buffer SerializeShapeToBuffer(const JPH::Shape* shape);
		JPH::Ref<JPH::Shape> DeserializeShapeFromBuffer(const Buffer& buffer);

		// Validation and information
		bool ValidateShapeData(const Buffer& buffer, bool deepValidation = false);
		bool GetShapeInfo(const Buffer& buffer, JPH::EShapeType& outShapeType, sizet& outDataSize);

		// Memory and performance utilities
		sizet CalculateShapeMemoryUsage(const Buffer& buffer);
		
		// Optional compression support (for future use)
		// @brief Compress shape data using RLE compression
		// 
		// @param inputBuffer Input buffer to compress
		// @return Always returns an owning Buffer - either compressed data or a copy of input buffer
		// 
		// @note MEMORY SAFETY: Always returns an owning Buffer to prevent UAF issues.
		//       When compression is not beneficial, returns a copy of inputBuffer.
		Buffer CompressShapeData(const Buffer& inputBuffer, bool forceCompression = false);
		Buffer DecompressShapeData(const Buffer& compressedBuffer);

	} // namespace JoltBinaryStreamUtils

}
