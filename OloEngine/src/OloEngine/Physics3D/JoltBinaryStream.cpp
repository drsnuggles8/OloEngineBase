#include "OloEnginePCH.h"
#include "JoltBinaryStream.h"

#include "OloEngine/Core/Log.h"
#include "JoltUtils.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/StreamIn.h>
#include <array>
#include <cstring>
#include <limits>

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
				
				// Use Jolt's SaveBinaryState for complete shape serialization
				// Note: SaveBinaryState internally writes EShapeType/EShapeSubType headers
				shape->SaveBinaryState(streamAdapter);
				
				// Save materials used by this shape
				JPH::PhysicsMaterialList materials;
				shape->SaveMaterialState(materials);
				streamAdapter.Write(static_cast<u32>(materials.size()));
				
				// For now, save material count only (material properties are handled through contact callbacks in Jolt)
				// Material debug names are not serialized since deserialization only uses PhysicsMaterial::sDefault
				// In the future, this could be enhanced to serialize custom material properties with a registry
				for ([[maybe_unused]] const JPH::PhysicsMaterialRefC& material : materials)
				{
					// Write zero name length to indicate no name data (reduces blob size)
					constexpr u32 nameLength = 0;
					streamAdapter.Write(nameLength);
					// No name bytes written - deserialization will skip reading names
				}
				
				// Save sub-shapes if any
				JPH::ShapeList subShapes;
				shape->SaveSubShapeState(subShapes);
				
				// Fail fast if sub-shapes are present to prevent silent data loss
				if (!subShapes.empty())
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShape: Shape has {} sub-shapes but recursive serialization is unsupported - aborting to prevent data loss", subShapes.size());
					return false;
				}
				
				streamAdapter.Write(static_cast<u32>(subShapes.size()));
				
				if (streamAdapter.IsFailed())
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShape: Stream adapter failed during serialization");
					return false;
				}
				
				OLO_CORE_TRACE("JoltBinaryStreamUtils::SerializeShape: Successfully serialized shape (type: {}, subtype: {}, {} materials, {} sub-shapes)", 
							   static_cast<u32>(shape->GetType()), static_cast<u32>(shape->GetSubType()), materials.size(), subShapes.size());
				
				return true;
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShape: Exception during serialization: {}", e.what());
				return false;
			}
		}

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
				
				// Use Jolt's sRestoreFromBinaryState for complete shape deserialization
				// Note: sRestoreFromBinaryState internally reads EShapeType/EShapeSubType headers
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
				
				// Validate material count to prevent OOM attacks from malformed input
				constexpr u32 MAX_MATERIALS = 1024;
				if (materialCount == 0)
				{
					// No materials to process, skip material handling
				}
				else if (materialCount > MAX_MATERIALS)
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Material count {} exceeds maximum allowed {} - aborting to prevent OOM", materialCount, MAX_MATERIALS);
					return nullptr;
				}
				else
				{
					// Safe to allocate and process materials
					std::vector<JPH::PhysicsMaterialRefC> materials;
					materials.reserve(static_cast<size_t>(materialCount));
					
					for (u32 i = 0; i < materialCount; ++i)
					{
						// Read material debug name length (expected to be zero)
						u32 nameLength;
						streamAdapter.Read(nameLength);
						
						// Skip any name data if present (for backwards compatibility)
						if (nameLength > 0)
						{
							// Drain the name bytes to maintain stream consistency
							constexpr u32 DRAIN_CHUNK_SIZE = 256;
							std::array<u8, DRAIN_CHUNK_SIZE> drainBuffer;
							
							u32 remainingBytes = nameLength;
							while (remainingBytes > 0)
							{
								u32 chunkSize = std::min(remainingBytes, DRAIN_CHUNK_SIZE);
								streamAdapter.ReadBytes(drainBuffer.data(), chunkSize);
								remainingBytes -= chunkSize;
							}
							
							OLO_CORE_WARN("JoltBinaryStreamUtils::DeserializeShape: Skipping material debug name ({} bytes) - names not used in current implementation", nameLength);
						}
						
						// Always use default material since we don't have a material registry yet
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
				
				// Fail fast if sub-shapes are present to prevent silent data loss
				if (subShapeCount > 0)
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Shape has {} sub-shapes but recursive deserialization is unsupported - aborting to prevent data loss", subShapeCount);
					return nullptr;
				}
				
				if (streamAdapter.IsFailed())
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DeserializeShape: Stream adapter failed during deserialization");
					return nullptr;
				}
				
				OLO_CORE_TRACE("JoltBinaryStreamUtils::DeserializeShape: Successfully deserialized shape (type: {}, subtype: {}, {} materials, {} sub-shapes)", 
							   static_cast<u32>(shape->GetType()), static_cast<u32>(shape->GetSubType()), materialCount, subShapeCount);
				
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
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShapeToBuffer: Cannot serialize null shape");
				return Buffer();
			}

			JoltBinaryStreamWriter writer;
			if (SerializeShape(shape, writer))
			{
				return writer.CreateBuffer();
			}

			// Log detailed error information for failed serialization
			OLO_CORE_ERROR("JoltBinaryStreamUtils::SerializeShapeToBuffer: Failed to serialize shape (ptr: {:#x}, type: {}, subtype: {})", 
						   reinterpret_cast<uintptr_t>(shape),
						   static_cast<u32>(shape->GetType()),
						   static_cast<u32>(shape->GetSubType()));

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
		 * @param deepValidation If true, performs full deserialization for thorough validation
		 * @return true if the buffer contains valid shape data
		 */
		bool ValidateShapeData(const Buffer& buffer, bool deepValidation)
		{
			if (!buffer.Data || buffer.Size == 0)
				return false;

		// Quick validation using GetShapeInfo first (cheap header/magic/size checks)
		JPH::EShapeType shapeType;
		sizet dataSize;
		if (!GetShapeInfo(buffer, shapeType, dataSize))
			return false;

		// Basic sanity check on data size
		if (dataSize < sizeof(JPH::EShapeType))
			return false;
		
		// Only perform expensive full deserialization if deep validation is requested
		if (deepValidation)
		{
			JPH::Ref<JPH::Shape> shape = DeserializeShapeFromBuffer(buffer);
			return shape != nullptr;
		}

		// Quick validation passed - buffer is plausible
		return true;
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
			if (!buffer.Data || buffer.Size < sizeof(JPH::EShapeType))
				return false;

			try
			{
				// Read shape type from Jolt's internal binary format
				// SaveBinaryState writes EShapeType as the first field
				const u8* data = static_cast<const u8*>(buffer.Data);
				::memcpy(&outShapeType, data, sizeof(JPH::EShapeType));
				
				// The entire buffer contains Jolt's binary shape data
				// No manual headers are added, so the full size is shape data
				outDataSize = buffer.Size;
				
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
			if (!buffer.Data || buffer.Size == 0)
				return 0;

			// Start with the serialized buffer size as base memory usage
			sizet totalMemory = buffer.Size;

			try 
			{
				// Try to deserialize the shape to get more accurate memory estimate
				JPH::EShapeType shapeType;
				sizet dataSize;
				if (GetShapeInfo(buffer, shapeType, dataSize))
				{
					// Add estimated runtime memory overhead based on shape type
					// These are rough estimates based on Jolt Physics internal structures
					switch (shapeType)
					{
					case JPH::EShapeType::Convex:
						// Convex shapes: vertices, edges, faces + collision data
						totalMemory += dataSize * 2; // ~2x overhead for runtime structures
						break;
					case JPH::EShapeType::Mesh:
						// Mesh shapes: triangle soup + spatial acceleration structures
						totalMemory += dataSize * 3; // ~3x overhead for octree/BVH structures
						break;
					case JPH::EShapeType::HeightField:
						// Height fields: height data + cached normals
						totalMemory += static_cast<sizet>(dataSize * 1.5f); // ~1.5x overhead for normal cache
						break;
					case JPH::EShapeType::Compound:
						// Compound shapes: child shapes + transformation matrices
						totalMemory += static_cast<sizet>(dataSize * 1.2f); // ~1.2x overhead for hierarchy
						break;
					default:
						// Basic shapes (sphere, box, capsule, etc.)
						totalMemory += 128; // Small fixed overhead for basic shapes
						break;
					}
				}
				else
				{
					// If we can't determine the shape type, use a conservative estimate
					totalMemory *= 2; // Double the serialized size as fallback
				}
			}
			catch (...)
			{
				// If any error occurs during analysis, fall back to basic estimate
				OLO_CORE_WARN("JoltBinaryStreamUtils::CalculateShapeMemoryUsage: Error analyzing shape data, using basic estimate");
				totalMemory *= 2; // Conservative fallback estimate
			}

			return totalMemory;
		}

		/**
		 * @brief Compress shape data using simple RLE compression (opt-in for suitable content)
		 * 
		 * @param inputBuffer The uncompressed shape data
		 * @param forceCompression If true, attempts compression regardless of content heuristics
		 * @return Always returns an owning Buffer - either compressed data or a copy of input buffer
		 */
		Buffer CompressShapeData(const Buffer& inputBuffer, bool forceCompression)
		{
			if (!inputBuffer.Data || inputBuffer.Size == 0)
			{
				OLO_CORE_WARN("JoltBinaryStreamUtils::CompressShapeData: Input buffer is empty");
				return Buffer();
			}

			// For small buffers, compression isn't worth it - return copy of original
			if (inputBuffer.Size < 64)
			{
				OLO_CORE_TRACE("JoltBinaryStreamUtils::CompressShapeData: Buffer too small for compression ({}B), returning copy", inputBuffer.Size);
				return Buffer::Copy(inputBuffer);
			}

			// Validate that input size fits in 32-bit for header
			if (inputBuffer.Size > UINT32_MAX)
			{
				OLO_CORE_WARN("JoltBinaryStreamUtils::CompressShapeData: Input size too large for 32-bit header, returning copy of original");
				return Buffer::Copy(inputBuffer);
			}

			// Content-aware compression gating: Skip RLE for binary data that's unlikely to compress well
			if (!forceCompression)
			{
				// Sample first 1KB to check for entropy (binary shape data often has high entropy)
				const u8* sampleData = static_cast<const u8*>(inputBuffer.Data);
				sizet sampleSize = std::min(inputBuffer.Size, static_cast<sizet>(1024));
				
				// Count unique bytes in sample
				bool seenBytes[256] = {false};
				sizet uniqueBytes = 0;
				for (sizet i = 0; i < sampleSize; ++i)
				{
					if (!seenBytes[sampleData[i]])
					{
						seenBytes[sampleData[i]] = true;
						uniqueBytes++;
					}
				}
				
				// If sample has high entropy (>75% unique bytes), skip compression
				f32 entropyRatio = static_cast<f32>(uniqueBytes) / 256.0f;
				if (entropyRatio > 0.75f)
				{
					OLO_CORE_TRACE("JoltBinaryStreamUtils::CompressShapeData: High entropy content detected ({:.1f}%), skipping RLE compression", entropyRatio * 100.0f);
					return Buffer::Copy(inputBuffer);
				}
			}

			// Simple Run-Length Encoding compression
			const u8* input = static_cast<const u8*>(inputBuffer.Data);
			std::vector<u8> compressed;
			
		// Guard against overflow and excessive allocation in reserve calculation
		constexpr sizet MAX_SAFE_RESERVE = 1024 * 1024 * 100; // 100MB cap
		
		sizet desiredReserve;
		if (inputBuffer.Size <= std::numeric_limits<sizet>::max() / 2)
		{
			desiredReserve = 2 * inputBuffer.Size;  // Safe multiplication
		}
		else
		{
			// Skip reserving if multiplication would overflow
			desiredReserve = 0;
		}
		
		// Only reserve if the desired size is reasonable
		if (desiredReserve > 0 && desiredReserve <= MAX_SAFE_RESERVE)
		{
			compressed.reserve(desiredReserve);
		}
		// For very large inputs, let vector grow naturally to avoid huge upfront allocation

		u8 currentByte = input[0];
		u8 runLength = 1;			for (sizet i = 1; i < inputBuffer.Size; ++i)
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

			// Calculate total size with header: magic (4 bytes) + original size (4 bytes) + compressed data
			constexpr sizet headerSize = 8; // 4-byte magic + 4-byte size
			sizet totalCompressedSize = headerSize + compressed.size();

			// Only use compression if it actually reduces size (including header overhead)
			if (totalCompressedSize < inputBuffer.Size)
			{
				Buffer result;
				result.Allocate(totalCompressedSize);
				
				u8* resultData = static_cast<u8*>(result.Data);
				
				// Write magic number "JRLE"
				resultData[0] = 'J';
				resultData[1] = 'R';
				resultData[2] = 'L';
				resultData[3] = 'E';
				
				// Write original size as 32-bit little-endian
				u32 originalSize = static_cast<u32>(inputBuffer.Size);
				resultData[4] = static_cast<u8>(originalSize & 0xFF);
				resultData[5] = static_cast<u8>((originalSize >> 8) & 0xFF);
				resultData[6] = static_cast<u8>((originalSize >> 16) & 0xFF);
				resultData[7] = static_cast<u8>((originalSize >> 24) & 0xFF);
				
				// Copy compressed payload
				::memcpy(resultData + headerSize, compressed.data(), compressed.size());
				
				OLO_CORE_TRACE("JoltBinaryStreamUtils::CompressShapeData: Compressed {} bytes to {} bytes with header (payload: {} bytes, {:.1f}% reduction)", 
							   inputBuffer.Size, totalCompressedSize, compressed.size(),
							   100.0f * (1.0f - static_cast<f32>(totalCompressedSize) / static_cast<f32>(inputBuffer.Size)));
				
				return result;
			}
			else
			{
				// Compression wasn't beneficial, return copy of original buffer for memory safety
				OLO_CORE_TRACE("JoltBinaryStreamUtils::CompressShapeData: Compression not beneficial (would be {} bytes vs {} original), returning copy of original buffer", 
							   totalCompressedSize, inputBuffer.Size);
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

			// Check if buffer has our compression header (magic + size)
			constexpr sizet headerSize = 8; // 4-byte magic + 4-byte size
			if (compressedBuffer.Size < headerSize)
			{
				OLO_CORE_TRACE("JoltBinaryStreamUtils::DecompressShapeData: Buffer too small for header, returning original");
				return Buffer::Copy(compressedBuffer);
			}

			const u8* input = static_cast<const u8*>(compressedBuffer.Data);
			
			// Check for magic number "JRLE"
			if (input[0] != 'J' || input[1] != 'R' || input[2] != 'L' || input[3] != 'E')
			{
				OLO_CORE_TRACE("JoltBinaryStreamUtils::DecompressShapeData: No compression magic found, returning original");
				return Buffer::Copy(compressedBuffer);
			}

			// Read original size from header (32-bit little-endian)
			u32 originalSize = static_cast<u32>(input[4]) |
							  (static_cast<u32>(input[5]) << 8) |
							  (static_cast<u32>(input[6]) << 16) |
							  (static_cast<u32>(input[7]) << 24);

			// Get compressed payload (skip header)
			const u8* compressedPayload = input + headerSize;
			sizet payloadSize = compressedBuffer.Size - headerSize;

			// If payload size is odd, it might not be valid RLE data (RLE produces pairs)
			if (payloadSize % 2 != 0)
			{
				OLO_CORE_WARN("JoltBinaryStreamUtils::DecompressShapeData: Invalid RLE payload size (odd), returning original");
				return Buffer::Copy(compressedBuffer);
			}

			// Preflight: validate all runs and compute total output size
			sizet totalOutputSize = 0;
			for (sizet i = 0; i < payloadSize; i += 2)
			{
				// Ensure run pair exists
				if (i + 1 >= payloadSize)
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DecompressShapeData: Incomplete RLE run at offset {}", i);
					return Buffer::Copy(compressedBuffer);
				}
				
				u8 runLength = compressedPayload[i];
				totalOutputSize += static_cast<sizet>(runLength);
				
				// Early bounds check
				if (totalOutputSize > originalSize)
				{
					OLO_CORE_ERROR("JoltBinaryStreamUtils::DecompressShapeData: RLE output would exceed original size ({} > {})", totalOutputSize, originalSize);
					return Buffer::Copy(compressedBuffer);
				}
			}
			
			// Validate final computed size matches header
			if (totalOutputSize != originalSize)
			{
				OLO_CORE_ERROR("JoltBinaryStreamUtils::DecompressShapeData: RLE computed size mismatch (got {} bytes, expected {} bytes)", 
							   totalOutputSize, originalSize);
				return Buffer::Copy(compressedBuffer);
			}

			// Allocate output buffer once with exact size
			std::vector<u8> decompressed(originalSize);
			u8* writePtr = decompressed.data();
			
			// Decompress directly into preallocated buffer
			for (sizet i = 0; i < payloadSize; i += 2)
			{
				u8 runLength = compressedPayload[i];
				u8 byte = compressedPayload[i + 1];
				
				// Write run directly to buffer
				std::memset(writePtr, byte, runLength);
				writePtr += runLength;
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

	} // namespace JoltBinaryStreamUtils

} // namespace OloEngine