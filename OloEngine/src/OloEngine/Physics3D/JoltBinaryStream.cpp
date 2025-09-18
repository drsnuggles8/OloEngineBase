#include "OloEnginePCH.h"
#include "JoltBinaryStream.h"

#include "OloEngine/Core/Log.h"
#include "JoltUtils.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace OloEngine {

	namespace JoltBinaryStreamUtils {

		/**
		 * @brief Serialize a Jolt shape to binary data
		 * 
		 * @param shape The Jolt shape to serialize
		 * @param outWriter The binary stream writer to write to
		 * @return true if serialization was successful
		 */
		bool SerializeShape(const JPH::Shape* shape, JoltBinaryStreamWriter& outWriter)
		{
			if (!shape)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShape: Cannot serialize null shape");
				return false;
			}

			try
			{
				// For now, use a simple approach - write basic shape information
				// This is a placeholder until we can properly implement Jolt serialization
				
				// Write shape type
				JPH::EShapeType shapeType = shape->GetType();
				outWriter.WriteBytes(&shapeType, sizeof(shapeType));

				// Write a placeholder for data size
				sizet dataSize = sizeof(shapeType); // Just the type for now
				outWriter.WriteBytes(&dataSize, sizeof(dataSize));

				// TODO: Implement proper shape serialization
				// For now, we're just writing the shape type
				
				return !outWriter.IsFailed();
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShape: Exception during serialization: {}", e.what());
				return false;
			}
		}

		/**
		 * @brief Deserialize a Jolt shape from binary data
		 * 
		 * @param inReader The binary stream reader to read from
		 * @return The deserialized shape, or nullptr if deserialization failed
		 */
		JPH::Ref<JPH::Shape> DeserializeShape(JoltBinaryStreamReader& inReader)
		{
			if (inReader.IsFailed() || inReader.IsEOF())
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Cannot deserialize from failed or empty stream");
				return nullptr;
			}

			try
			{
				// Read shape type
				JPH::EShapeType shapeType;
				inReader.ReadBytes(&shapeType, sizeof(shapeType));

				// Read data size
				sizet dataSize;
				inReader.ReadBytes(&dataSize, sizeof(dataSize));

				// TODO: Implement proper shape deserialization based on shape type
				// For now, we're just reading the basic data but not reconstructing the shape
				
				OLO_CORE_WARN("JoltBinaryStreamUtils::DeserializeShape: Shape deserialization not fully implemented yet");
				return nullptr;
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Exception during deserialization: {}", e.what());
				return nullptr;
			}
		}

		/**
		 * @brief Serialize a shape to a Buffer
		 * 
		 * @param shape The shape to serialize
		 * @return Buffer containing the serialized data, or empty buffer on failure
		 */
		Buffer SerializeShapeToBuffer(const JPH::Shape* shape)
		{
			if (!shape)
				return Buffer();

			JoltBinaryStreamWriter writer;
			if (SerializeShape(shape, writer))
			{
				return writer.CreateBuffer();
			}

			return Buffer();
		}

		/**
		 * @brief Deserialize a shape from a Buffer
		 * 
		 * @param buffer The buffer containing serialized shape data
		 * @return The deserialized shape, or nullptr on failure
		 */
		JPH::Ref<JPH::Shape> DeserializeShapeFromBuffer(const Buffer& buffer)
		{
			if (!buffer.Data || buffer.Size == 0)
				return nullptr;

			JoltBinaryStreamReader reader(buffer);
			return DeserializeShape(reader);
		}

		/**
		 * @brief Validate serialized shape data
		 * 
		 * @param buffer The buffer to validate
		 * @return true if the buffer contains valid shape data
		 */
		bool ValidateShapeData(const Buffer& buffer)
		{
			if (!buffer.Data || buffer.Size == 0)
				return false;

			// Try to deserialize and check if it succeeds
			JPH::Ref<JPH::Shape> shape = DeserializeShapeFromBuffer(buffer);
			return shape != nullptr;
		}

		/**
		 * @brief Get information about serialized shape data
		 * 
		 * @param buffer The buffer containing serialized shape data
		 * @param outShapeType Will contain the shape type if successful
		 * @param outDataSize Will contain the data size
		 * @return true if information was successfully extracted
		 */
		bool GetShapeInfo(const Buffer& buffer, JPH::EShapeType& outShapeType, sizet& outDataSize)
		{
			outDataSize = buffer.Size;

			if (!buffer.Data || buffer.Size < sizeof(JPH::EShapeType) + sizeof(sizet))
				return false;

			try
			{
				// Read shape type directly from buffer header
				const u8* data = static_cast<const u8*>(buffer.Data);
				::memcpy(&outShapeType, data, sizeof(JPH::EShapeType));
				
				// Read the stored data size
				sizet storedDataSize;
				::memcpy(&storedDataSize, data + sizeof(JPH::EShapeType), sizeof(sizet));
				outDataSize = storedDataSize;
				
				return true;
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::GetShapeInfo: Exception: {}", e.what());
				return false;
			}
		}

		/**
		 * @brief Calculate approximate memory usage of serialized shape
		 * 
		 * @param buffer The buffer containing serialized shape data
		 * @return Approximate memory usage in bytes
		 */
		sizet CalculateShapeMemoryUsage(const Buffer& buffer)
		{
			// For now, return the serialized size as an approximation
			// In the future, this could be more sophisticated
			return buffer.Size;
		}

		/**
		 * @brief Compress shape data using simple RLE or other compression
		 * 
		 * @param inputBuffer The uncompressed shape data
		 * @return Compressed buffer, or original buffer if compression fails/isn't beneficial
		 */
		Buffer CompressShapeData(const Buffer& inputBuffer)
		{
			// For now, return the original buffer
			// TODO: Implement compression algorithm if needed
			// This could use zlib, lz4, or other compression libraries
			return Buffer::Copy(inputBuffer);
		}

		/**
		 * @brief Decompress shape data
		 * 
		 * @param compressedBuffer The compressed shape data
		 * @return Decompressed buffer, or original buffer if not compressed
		 */
		Buffer DecompressShapeData(const Buffer& compressedBuffer)
		{
			// For now, return the original buffer
			// TODO: Implement decompression to match CompressShapeData
			return Buffer::Copy(compressedBuffer);
		}

	} // namespace JoltBinaryStreamUtils

} // namespace OloEngine