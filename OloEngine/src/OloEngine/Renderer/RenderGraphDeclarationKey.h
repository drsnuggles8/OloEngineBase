#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <string_view>
#include <type_traits>
#include <utility>

namespace OloEngine
{
    // @brief Accumulates the inputs a render-graph declaration depends on into
    // one 64-bit key (issue #1333).
    //
    // `RenderGraph::BuildFrameGraph` and `RenderPipeline::PopulateBlackboard`
    // both skip their work while the key matches the previous frame, so the key
    // must move whenever a declaration could. Everything that writes into it
    // goes through this type, so every writer hashes the same way. The only
    // types it accepts are the ones a declaration can branch on: a bool, an
    // integer, an enum, or a resource identity. A float is refused at compile
    // time, because a declaration never branches on one, and hashing one would
    // rebuild the graph every time a slider moved.
    class RGDeclarationKey
    {
      public:
        static constexpr u64 kFnv1aOffset = 0xcbf29ce484222325ull;
        static constexpr u64 kFnv1aPrime = 0x100000001b3ull;

        constexpr void AddByte(const u8 value) noexcept
        {
            m_Hash = (m_Hash ^ value) * kFnv1aPrime;
        }

        constexpr void Add(const bool value) noexcept
        {
            AddByte(value ? 1u : 0u);
        }

        template<typename T>
            requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
        constexpr void Add(const T value) noexcept
        {
            auto bits = static_cast<u64>(static_cast<std::make_unsigned_t<T>>(value));
            for (sizet byte = 0; byte < sizeof(T); ++byte)
            {
                AddByte(static_cast<u8>(bits & 0xffu));
                bits >>= 8u;
            }
        }

        template<typename T>
            requires std::is_enum_v<T>
        constexpr void Add(const T value) noexcept
        {
            Add(std::to_underlying(value));
        }

        // By IDENTITY (index + generation), never by native driver name: a
        // destroyed texture's GL name can be reissued to its replacement, and a
        // name-keyed import would then describe the old resource (issue #691).
        template<typename Tag>
        constexpr void Add(const RHI::Handle<Tag> handle) noexcept
        {
            Add(RHI::HashKey(handle));
        }

        constexpr void Add(const std::string_view text) noexcept
        {
            Add(static_cast<u64>(text.size()));
            for (const char c : text)
                AddByte(static_cast<u8>(c));
        }

        template<typename T>
            requires std::is_floating_point_v<T>
        void Add(T) = delete;

        [[nodiscard]] constexpr u64 Get() const noexcept
        {
            // 0 is reserved: BuildFrameGraph reads a zero key as "do not cache".
            return m_Hash == 0u ? 1u : m_Hash;
        }

      private:
        u64 m_Hash = kFnv1aOffset;
    };
} // namespace OloEngine
