#pragma once

#include "OloEngine/Core/Base.h"
#include <string>
#include <string_view>

namespace OloEngine
{
	//==============================================================================
	/// Hash utility class for compile-time and runtime string hashing
	/// Currently implements FNV-1a (compile-time) and CRC32 (runtime) algorithms
	/// 
	/// TODO: Future Hash Algorithm Options (as additional methods, not replacements):
	/// 
	/// xxHash32/64:
	///   - 2-3x faster than CRC32 with excellent distribution
	///   - Industry standard (MySQL, RocksDB, Redis, Unreal Engine)
	///   - Perfect for high-performance runtime hashing
	///   - Reference: https://xxhash.com/
	/// 
	/// MurmurHash3:
	///   - Good balance of speed vs. collision resistance
	///   - Widely adopted, simpler implementation than xxHash
	///   - Available in 32-bit and 128-bit variants
	/// 
	/// CityHash/FarmHash:
	///   - Google's high-performance hash functions
	///   - Optimized for x86-64 architectures
	///   - Excellent for bulk data processing
	/// 
	/// FNV-1a 64-bit:
	///   - Extended version of current FNV for better collision resistance
	///   - Same simplicity, larger hash space
	///   - constexpr uint64_t GenerateFNVHash64(std::string_view str)
	/// 
	/// Blake2/Blake3:
	///   - Cryptographically secure but extremely fast
	///   - Overkill for simple hashing, good for security-sensitive data
	/// 
	/// SIMD-Optimized Variants:
	///   - Vectorized implementations of xxHash, CRC32
	///   - For bulk processing of many strings/data blocks
	/// 
	/// Implementation Strategy:
	///   - Keep FNV-1a as compile-time default (simple, reliable)
	///   - Add Hash::XXHash32() for high-performance runtime needs
	///   - Add Hash::MurmurHash3() as alternative runtime option
	///   - Template-based: Hash::Compute<Algorithm>(data) for flexibility
	/// 
	class Hash
	{
	public:
		//==============================================================================
		/// Compile-time FNV-1a hash implementation
		/// Perfect for creating compile-time identifiers from string literals
		static constexpr u32 GenerateFNVHash(std::string_view str) noexcept
		{
			constexpr u32 FNV_PRIME = 16777619u;
			constexpr u32 OFFSET_BASIS = 2166136261u;

			const sizet length = str.length();
			const char* data = str.data();

			u32 hash = OFFSET_BASIS;
			for (sizet i = 0; i < length; ++i)
			{
				hash ^= static_cast<u32>(*data++);
				hash *= FNV_PRIME;
			}
			
			return hash;
		}

		//==============================================================================
		/// Runtime CRC32 hash for larger strings or dynamic content
		static u32 CRC32(const char* str);
		static u32 CRC32(const std::string& string);
		
		//==============================================================================
		/// Simple 64-bit hash for UUIDs and other numeric data
		static constexpr u64 Hash64(u64 value) noexcept
		{
			// FNV-like mixing for 64-bit values
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdULL;
			value ^= value >> 33;
			value *= 0xc4ceb9fe1a85ec53ULL;
			value ^= value >> 33;
			return value;
		}

		//==============================================================================
		/// Combine two hash values (useful for multi-part identifiers)
		static constexpr u32 Combine(u32 hash1, u32 hash2) noexcept
		{
			return hash1 ^ (hash2 + 0x9e3779b9 + (hash1 << 6) + (hash1 >> 2));
		}
	};

} // namespace OloEngine