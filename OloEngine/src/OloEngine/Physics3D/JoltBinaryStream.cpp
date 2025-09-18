#include "OloEnginePCH.h"
#include "JoltBinaryStream.h"

#include "OloEngine/Core/Log.h"
#include "JoltUtils.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace OloEngine {

	namespace JoltBinaryStreamUtils {

		/**
		 * @brief Serialize a Jolt shape to binary data
		 * Note: This is a basic implementation that stores shape metadata and could be enhanced
		 * to use Jolt's streaming system when available.
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
				// Write shape type and basic metadata for now
				// This provides a foundation for shape serialization that can be enhanced later
				JPH::EShapeType shapeType = shape->GetType();
				JPH::EShapeSubType shapeSubType = shape->GetSubType();
				
				// Write shape type
				outWriter.WriteBytes(&shapeType, sizeof(shapeType));
				
				// Write shape subtype
				outWriter.WriteBytes(&shapeSubType, sizeof(shapeSubType));
				
				// Write shape user data
				u64 userData = shape->GetUserData();
				outWriter.WriteBytes(&userData, sizeof(userData));
				
				// Write bounds information
				JPH::AABox localBounds = shape->GetLocalBounds();
				outWriter.WriteBytes(&localBounds, sizeof(localBounds));
				
				// Write additional metadata
				f32 innerRadius = shape->GetInnerRadius();
				outWriter.WriteBytes(&innerRadius, sizeof(innerRadius));
				
				// TODO: For a complete implementation, this would need to serialize
				// the actual shape data using Jolt's binary serialization when available.
				// For now, we store the essential metadata that can identify the shape.
				
				OLO_CORE_TRACE("JoltBinaryStreamUtils::SerializeShape: Successfully serialized shape metadata (type: {}, subtype: {})", 
							   static_cast<u32>(shapeType), static_cast<u32>(shapeSubType));
				
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
		/**
		 * @brief Deserialize a Jolt shape from binary data
		 * Note: This is a basic implementation that reads shape metadata.
		 * A complete implementation would recreate the actual shape.
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
				// Read shape metadata that was written during serialization
				JPH::EShapeType shapeType;
				inReader.ReadBytes(&shapeType, sizeof(shapeType));
				
				JPH::EShapeSubType shapeSubType;
				inReader.ReadBytes(&shapeSubType, sizeof(shapeSubType));
				
				u64 userData;
				inReader.ReadBytes(&userData, sizeof(userData));
				
				JPH::AABox localBounds;
				inReader.ReadBytes(&localBounds, sizeof(localBounds));
				
				f32 innerRadius;
				inReader.ReadBytes(&innerRadius, sizeof(innerRadius));
				
				// TODO: For a complete implementation, this would reconstruct the actual shape
				// using the serialized data and Jolt's deserialization when available.
				// For now, we've successfully read the metadata but can't recreate the shape.
				
				OLO_CORE_WARN("JoltBinaryStreamUtils::DeserializeShape: Shape deserialization read metadata (type: {}, subtype: {}) but shape reconstruction not yet implemented", 
							 static_cast<u32>(shapeType), static_cast<u32>(shapeSubType));
				
				// Return nullptr for now since we can't reconstruct the shape yet
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
		 * @brief Compress shape data using simple RLE compression
		 * 
		 * @param inputBuffer The uncompressed shape data
		 * @return Compressed buffer, or original buffer if compression isn't beneficial
		 */
		Buffer CompressShapeData(const Buffer& inputBuffer)
		{
			if (!inputBuffer.Data || inputBuffer.Size == 0)
			{
				OLO_CORE_WARN("JoltBinaryStreamUtils::CompressShapeData: Input buffer is empty");
				return Buffer();
			}

			// For small buffers, compression isn't worth it
			if (inputBuffer.Size < 64)
			{
				return Buffer::Copy(inputBuffer);
			}

			// Simple Run-Length Encoding compression
			try
			{
				const u8* input = static_cast<const u8*>(inputBuffer.Data);
				std::vector<u8> compressed;
				compressed.reserve(inputBuffer.Size); // Reserve space for worst case

				u8 currentByte = input[0];
				u8 runLength = 1;

				for (sizet i = 1; i < inputBuffer.Size; ++i)
				{
					if (input[i] == currentByte && runLength < 255)
					{
						runLength++;
					}
					else
					{
						// Store run length and byte
						compressed.push_back(runLength);
						compressed.push_back(currentByte);
						
						currentByte = input[i];
						runLength = 1;
					}
				}

				// Store the last run
				compressed.push_back(runLength);
				compressed.push_back(currentByte);

				// Only use compression if it actually reduces size
				if (compressed.size() < inputBuffer.Size)
				{
					Buffer result;
					result.Allocate(compressed.size());
					::memcpy(result.Data, compressed.data(), compressed.size());
					
					OLO_CORE_TRACE("JoltBinaryStreamUtils::CompressShapeData: Compressed {} bytes to {} bytes ({:.1f}% reduction)", 
								   inputBuffer.Size, compressed.size(), 
								   100.0f * (1.0f - static_cast<f32>(compressed.size()) / static_cast<f32>(inputBuffer.Size)));
					
					return result;
				}
				else
				{
					// Compression wasn't beneficial, return original
					OLO_CORE_TRACE("JoltBinaryStreamUtils::CompressShapeData: Compression not beneficial, returning original buffer");
					return Buffer::Copy(inputBuffer);
				}
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::CompressShapeData: Exception during compression: {}", e.what());
				return Buffer::Copy(inputBuffer);
			}
		}

		/**
		 * @brief Decompress shape data using RLE decompression
		 * 
		 * @param compressedBuffer The compressed shape data
		 * @return Decompressed buffer, or original buffer if not compressed or decompression fails
		 */
		Buffer DecompressShapeData(const Buffer& compressedBuffer)
		{
			if (!compressedBuffer.Data || compressedBuffer.Size == 0)
			{
				OLO_CORE_WARN("JoltBinaryStreamUtils::DecompressShapeData: Input buffer is empty");
				return Buffer();
			}

			// If buffer size is odd, it might not be RLE compressed (RLE produces pairs)
			if (compressedBuffer.Size % 2 != 0)
			{
				OLO_CORE_TRACE("JoltBinaryStreamUtils::DecompressShapeData: Buffer size suggests no RLE compression, returning original");
				return Buffer::Copy(compressedBuffer);
			}

			try
			{
				const u8* input = static_cast<const u8*>(compressedBuffer.Data);
				std::vector<u8> decompressed;
				
				// Estimate decompressed size (worst case: each pair expands to 255 bytes)
				decompressed.reserve(compressedBuffer.Size * 128);

				for (sizet i = 0; i < compressedBuffer.Size; i += 2)
				{
					u8 runLength = input[i];
					u8 byte = input[i + 1];
					
					// Expand the run
					for (u8 j = 0; j < runLength; ++j)
					{
						decompressed.push_back(byte);
					}
				}

				if (!decompressed.empty())
				{
					Buffer result;
					result.Allocate(decompressed.size());
					::memcpy(result.Data, decompressed.data(), decompressed.size());
					
					OLO_CORE_TRACE("JoltBinaryStreamUtils::DecompressShapeData: Decompressed {} bytes to {} bytes", 
								   compressedBuffer.Size, decompressed.size());
					
					return result;
				}
				else
				{
					OLO_CORE_WARN("JoltBinaryStreamUtils::DecompressShapeData: Decompression resulted in empty buffer");
					return Buffer::Copy(compressedBuffer);
				}
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::DecompressShapeData: Exception during decompression: {}", e.what());
				return Buffer::Copy(compressedBuffer);
			}
		}

	} // namespace JoltBinaryStreamUtils

} // namespace OloEngine