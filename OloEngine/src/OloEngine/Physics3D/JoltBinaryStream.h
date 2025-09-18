#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Buffer.h"

#include <vector>

// Forward declarations for types we need but don't want to include full headers
namespace JPH {
	class Shape;
	enum class EShapeType : u8;
	template<typename T> class Ref;
	class StreamIn;
	class StreamOut;
}

namespace OloEngine {

	/**
	 * @brief Binary stream reader for Jolt Physics serialization
	 * 
	 * Reads binary data from OloEngine Buffer objects for deserializing physics shapes.
	 */
	class JoltBinaryStreamReader
	{
	public:
		explicit JoltBinaryStreamReader(const Buffer& buffer)
			: m_Buffer(&buffer), m_ReadBytes(0)
		{
			OLO_CORE_ASSERT(buffer.Data != nullptr && buffer.Size > 0, "Invalid buffer provided to JoltBinaryStreamReader");
		}

		explicit JoltBinaryStreamReader(const u8* data, u64 size)
			: m_Data(data), m_Size(size), m_ReadBytes(0)
		{
			OLO_CORE_ASSERT(data != nullptr && size > 0, "Invalid data provided to JoltBinaryStreamReader");
		}

		~JoltBinaryStreamReader()
		{
			m_Buffer = nullptr;
			m_Data = nullptr;
			m_ReadBytes = 0;
			m_Size = 0;
		}

		// Stream reading interface
		void ReadBytes(void* outData, sizet inNumBytes)
		{
			if (IsEOF() || IsFailed())
			{
				OLO_CORE_ERROR("JoltBinaryStreamReader: Attempted to read past end of stream or from failed stream");
				return;
			}

			const u8* sourceData = GetSourceData();
			if (sourceData)
			{
				::memcpy(outData, sourceData + m_ReadBytes, inNumBytes);
				m_ReadBytes += inNumBytes;
			}
		}

		bool IsEOF() const
		{ 
			return GetSourceSize() == 0 || m_ReadBytes >= GetSourceSize();
		}

		bool IsFailed() const
		{
			if (m_Buffer)
				return m_Buffer->Data == nullptr || m_Buffer->Size == 0;
			else
				return m_Data == nullptr || m_Size == 0;
		}

		// Additional utility methods
		u64 GetBytesRead() const { return m_ReadBytes; }
		u64 GetRemainingBytes() const 
		{ 
			u64 size = GetSourceSize();
			return m_ReadBytes < size ? size - m_ReadBytes : 0;
		}

		void Reset() { m_ReadBytes = 0; }

		// Seek to position (useful for debugging/validation)
		bool Seek(u64 position)
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
			return m_Buffer ? m_Buffer->Data : m_Data;
		}

		u64 GetSourceSize() const
		{
			return m_Buffer ? m_Buffer->Size : m_Size;
		}

	private:
		const Buffer* m_Buffer = nullptr;
		const u8* m_Data = nullptr;
		u64 m_Size = 0;
		u64 m_ReadBytes = 0;
	};

	/**
	 * @brief Binary stream writer for Jolt Physics serialization
	 * 
	 * Writes binary data to expandable buffer for serializing physics shapes.
	 */
	class JoltBinaryStreamWriter
	{
	public:
		JoltBinaryStreamWriter()
		{
			m_TempBuffer.reserve(1024); // Reserve some initial capacity
		}

		explicit JoltBinaryStreamWriter(sizet initialCapacity)
		{
			m_TempBuffer.reserve(initialCapacity);
		}

		~JoltBinaryStreamWriter() = default;

		// Stream writing interface
		void WriteBytes(const void* inData, sizet inNumBytes)
		{
			if (inData == nullptr || inNumBytes == 0)
				return;

			sizet currentOffset = m_TempBuffer.size();
			m_TempBuffer.resize(currentOffset + inNumBytes);
			::memcpy(m_TempBuffer.data() + currentOffset, inData, inNumBytes);
		}

		bool IsFailed() const { return false; }

		// Data access methods
		const std::vector<u8>& GetData() const { return m_TempBuffer; }
		const u8* GetDataPtr() const { return m_TempBuffer.data(); }
		sizet GetSize() const { return m_TempBuffer.size(); }
		bool IsEmpty() const { return m_TempBuffer.empty(); }

		// Create OloEngine Buffer from written data
		Buffer CreateBuffer() const
		{
			if (m_TempBuffer.empty())
				return Buffer();

			Buffer buffer(m_TempBuffer.size());
			::memcpy(buffer.Data, m_TempBuffer.data(), m_TempBuffer.size());
			return buffer;
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
		}

		void Reserve(sizet capacity) 
		{ 
			m_TempBuffer.reserve(capacity); 
		}

		// Get statistics for debugging
		sizet GetCapacity() const { return m_TempBuffer.capacity(); }
		void ShrinkToFit() { m_TempBuffer.shrink_to_fit(); }

	private:
		std::vector<u8> m_TempBuffer;
	};

	/**
	 * @brief Utility functions for Jolt shape serialization using binary streams
	 */
	namespace JoltBinaryStreamUtils {

		// Shape serialization functions
		bool SerializeShape(const JPH::Shape* shape, JoltBinaryStreamWriter& outWriter);
		JPH::Ref<JPH::Shape> DeserializeShape(JoltBinaryStreamReader& inReader);

		// Convenience functions for Buffer operations
		Buffer SerializeShapeToBuffer(const JPH::Shape* shape);
		JPH::Ref<JPH::Shape> DeserializeShapeFromBuffer(const Buffer& buffer);

		// Validation and information
		bool ValidateShapeData(const Buffer& buffer);
		bool GetShapeInfo(const Buffer& buffer, JPH::EShapeType& outShapeType, sizet& outDataSize);

		// Memory and performance utilities
		sizet CalculateShapeMemoryUsage(const Buffer& buffer);
		
		// Optional compression support (for future use)
		Buffer CompressShapeData(const Buffer& inputBuffer);
		Buffer DecompressShapeData(const Buffer& compressedBuffer);

	} // namespace JoltBinaryStreamUtils

}