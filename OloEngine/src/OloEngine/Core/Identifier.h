#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Hash.h"
#include <cstddef>
#include <functional>
#include <string_view>

namespace OloEngine
{
    //==============================================================================
    /// Compile-time string identifier using FNV hash
    /// Perfect for efficient parameter lookups and event routing in audio systems
    ///
    /// TODO: Future Identifier/Lookup System Options (as additional classes):
    ///
    /// Perfect Hash Functions:
    ///   - Zero collisions for known string sets
    ///   - Compile-time generation: template<const char* strings[], size_t N>
    ///   - Fastest possible lookup (direct array access)
    ///   - Best for: Audio parameter names known at compile-time
    ///   - Implementation: PerfectIdentifier<string_set>
    ///
    /// String Interning System:
    ///   - Runtime string deduplication with pointer-based comparison
    ///   - Extremely fast equality checks (pointer comparison)
    ///   - Memory overhead for string storage and hash table
    ///   - Best for: Dynamic parameter names, plugin systems
    ///   - Implementation: InternedIdentifier class
    ///
    /// Frozen Hash Maps (compile-time):
    ///   - All mappings resolved at compile-time
    ///   - Zero runtime overhead for lookups
    ///   - constexpr frozen::unordered_map support
    ///   - Best for: Static parameter→value mappings
    ///   - Implementation: FrozenIdentifier<Key, Value>
    ///
    /// Robin Hood Hash Tables:
    ///   - Better collision handling than standard hash tables
    ///   - Consistent lookup times even with collisions
    ///   - Runtime overhead but predictable performance
    ///   - Best for: Real-time systems requiring guaranteed timing
    ///
    /// Hierarchical Identifiers:
    ///   - Path-based: "oscillator.frequency", "filter.cutoff"
    ///   - Namespace support for complex audio graphs
    ///   - Tree-based lookup with caching
    ///   - Implementation: HierarchicalIdentifier class
    ///
    /// 64-bit Identifiers:
    ///   - Extended hash space for collision resistance
    ///   - Support for larger parameter sets
    ///   - Implementation: Identifier64 class
    ///
    /// Implementation Strategy:
    ///   - Keep Identifier as lightweight 32-bit default
    ///   - Add specialized classes for specific use cases
    ///   - Template-based: Identifier<HashAlgorithm, SizeType>
    ///   - Maintain compile-time evaluation where possible
    ///
    class Identifier
    {
      public:
        //==============================================================================
        /// Constructors
        constexpr Identifier() noexcept : m_Hash(0) {}

        constexpr Identifier(std::string_view name) noexcept
            : m_Hash(Hash::GenerateFNVHash(name))
        {
        }

        constexpr explicit Identifier(u32 hash) noexcept
            : m_Hash(hash)
        {
        }

        //==============================================================================
        /// Operators
        constexpr bool operator==(const Identifier& other) const noexcept
        {
            return m_Hash == other.m_Hash;
        }

        // Note: operator< intentionally removed to prevent issues with ordered containers
        // due to potential hash collisions. Use std::unordered_map/set for Identifier keys.
        // If ordering is required, consider using a different identifier type that stores
        // the original string for deterministic comparison.

        //==============================================================================
        /// Conversion operators
        constexpr operator u32() const noexcept
        {
            return m_Hash;
        }

        //==============================================================================
        /// Getters
        constexpr u32 GetHash() const noexcept
        {
            return m_Hash;
        }
        constexpr bool IsValid() const noexcept
        {
            return m_Hash != 0;
        }

        //==============================================================================
        /// Static helpers
        static constexpr Identifier Invalid() noexcept
        {
            return Identifier(0);
        }

      private:
        u32 m_Hash;
    };

//==============================================================================
/// Macro for compile-time identifier creation
/// Usage: DECLARE_IDENTIFIER(PlayButton); creates compile-time identifier
#define DECLARE_IDENTIFIER(name)                  \
    static constexpr ::OloEngine::Identifier name \
    {                                             \
        #name                                     \
    }

} // namespace OloEngine

//==============================================================================
/// Standard library hash specialization
namespace std
{
    template<>
    struct hash<OloEngine::Identifier>
    {
        std::size_t operator()(const OloEngine::Identifier& id) const noexcept
        {
            return static_cast<std::size_t>(id.GetHash());
        }
    };
} // namespace std
