#include "OloEnginePCH.h"
#include "JoltBinaryStream.h"

#include "OloEngine/Core/Log.h"
#include "JoltUtils.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/StreamIn.h>
#include <sstream>

namespace OloEngine {

	namespace JoltBinaryStreamUtils {

		/**
		 * @brief Adapter to bridge OloEngine JoltBinaryStreamWriter to Jolt's StreamOut interface
		 */
		class JoltStreamOutAdapter : public JPH::StreamOut
		{
		public:
			explicit JoltStreamOutAdapter(JoltBinaryStreamWriter& writer) : m_Writer(writer) {}

			void WriteBytes(const void* inData, size_t inNumBytes) override
			{
				m_Writer.WriteBytes(inData, inNumBytes);
			}

			bool IsFailed() const override
			{
				return m_Writer.IsFailed();
			}

		private:
			JoltBinaryStreamWriter& m_Writer;
		};

		/**
		 * @brief Adapter to bridge OloEngine JoltBinaryStreamReader to Jolt's StreamIn interface
		 */
		class JoltStreamInAdapter : public JPH::StreamIn
		{
		public:
			explicit JoltStreamInAdapter(JoltBinaryStreamReader& reader) : m_Reader(reader) {}

			void ReadBytes(void* outData, size_t inNumBytes) override
			{
				m_Reader.ReadBytes(outData, inNumBytes);
			}

			bool IsEOF() const override
			{
				return m_Reader.IsEOF();
			}

			bool IsFailed() const override
			{
				return m_Reader.IsFailed();
			}

		private:
			JoltBinaryStreamReader& m_Reader;
		};

		/**
		 * @brief Serialize a Jolt shape to binary data using Jolt's native binary serialization
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
				// Use Jolt's native binary serialization system
				JoltStreamOutAdapter streamAdapter(outWriter);
				
				// Serialize shape type first for type identification during deserialization
				JPH::EShapeType shapeType = shape->GetType();
				JPH::EShapeSubType shapeSubType = shape->GetSubType();
				streamAdapter.Write(shapeType);
				streamAdapter.Write(shapeSubType);
				
				// Use Jolt's SaveBinaryState for complete shape serialization
				shape->SaveBinaryState(streamAdapter);
				
				// Save materials used by this shape
				JPH::PhysicsMaterialList materials;
				shape->SaveMaterialState(materials);
				streamAdapter.Write(static_cast<u32>(materials.size()));
				
				// For now, save material count only (material properties are handled through contact callbacks in Jolt)
				// In the future, this could be enhanced to serialize custom material properties
				for (const JPH::PhysicsMaterialRefC& material : materials)
				{
					// Save material debug name if available (for debugging/identification)
					std::string debugName = (material != nullptr) ? material->GetDebugName() : "Default";
					u32 nameLength = static_cast<u32>(debugName.length());
					streamAdapter.Write(nameLength);
					streamAdapter.WriteBytes(debugName.c_str(), nameLength);
				}
				
				// Save sub-shapes if any
				JPH::ShapeList subShapes;
				shape->SaveSubShapeState(subShapes);
				streamAdapter.Write(static_cast<u32>(subShapes.size()));
				
				// For now, we'll handle sub-shapes in a future enhancement
				// Each sub-shape would need recursive serialization
				if (!subShapes.empty())
				{
					OLO_CORE_WARN("JoltBinaryStreamUtils::SerializeShape: Shape has {} sub-shapes - recursive serialization not yet implemented", subShapes.size());
				}
				
				if (streamAdapter.IsFailed())
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShape: Stream adapter failed during serialization");
					return false;
				}
				
				OLO_CORE_TRACE("JoltBinaryStreamUtils::SerializeShape: Successfully serialized shape (type: {}, subtype: {}, {} materials, {} sub-shapes)", 
							   static_cast<u32>(shapeType), static_cast<u32>(shapeSubType), materials.size(), subShapes.size());
				
				return true;
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
		 * @brief Deserialize a Jolt shape from binary data using Jolt's native binary deserialization
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
				// Use Jolt's native binary deserialization system
				JoltStreamInAdapter streamAdapter(inReader);
				
				// Read shape type first to verify we can deserialize this shape
				JPH::EShapeType shapeType;
				JPH::EShapeSubType shapeSubType;
				streamAdapter.Read(shapeType);
				streamAdapter.Read(shapeSubType);
				
				if (streamAdapter.IsFailed() || streamAdapter.IsEOF())
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Failed to read shape type/subtype");
					return nullptr;
				}
				
				// Use Jolt's sRestoreFromBinaryState for complete shape deserialization
				JPH::Shape::ShapeResult shapeResult = JPH::Shape::sRestoreFromBinaryState(streamAdapter);
				if (shapeResult.HasError())
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Failed to restore shape from binary state: {}", shapeResult.GetError().c_str());
					return nullptr;
				}
				
				JPH::Ref<JPH::Shape> shape = shapeResult.Get();
				if (shape == nullptr)
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Shape restoration returned null shape");
					return nullptr;
				}
				
				// Read material count and restore materials
				u32 materialCount;
				streamAdapter.Read(materialCount);
				
				if (materialCount > 0)
				{
					std::vector<JPH::PhysicsMaterialRefC> materials;
					materials.reserve(materialCount);
					
					for (u32 i = 0; i < materialCount; ++i)
					{
						// Read material debug name
						u32 nameLength;
						streamAdapter.Read(nameLength);
						
						std::string debugName;
						if (nameLength > 0 && nameLength < 1024) // Sanity check
						{
							debugName.resize(nameLength);
							streamAdapter.ReadBytes(debugName.data(), nameLength);
						}
						
						// Create a new simple material for now
						// In the future, this could use a material registry to reuse materials
						JPH::PhysicsMaterialRefC material = JPH::PhysicsMaterial::sDefault;
						materials.push_back(material);
					}
					
					// Restore material references (if the shape supports it)
					if (!materials.empty())
					{
						shape->RestoreMaterialState(materials.data(), static_cast<u32>(materials.size()));
					}
				}
				
				// Read sub-shape count
				u32 subShapeCount;
				streamAdapter.Read(subShapeCount);
				
				// For now, we expect no sub-shapes since recursive serialization isn't implemented yet
				if (subShapeCount > 0)
				{
					OLO_CORE_WARN("JoltBinaryStreamUtils::DeserializeShape: Shape has {} sub-shapes but recursive deserialization not yet implemented", subShapeCount);
				}
				
				if (streamAdapter.IsFailed())
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Stream adapter failed during deserialization");
					return nullptr;
				}
				
				OLO_CORE_TRACE("JoltBinaryStreamUtils::DeserializeShape: Successfully deserialized shape (type: {}, subtype: {}, {} materials, {} sub-shapes)", 
							   static_cast<u32>(shapeType), static_cast<u32>(shapeSubType), materialCount, subShapeCount);
				
				return shape;
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