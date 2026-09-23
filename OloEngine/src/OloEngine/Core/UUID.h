#pragma once

#include "OloEngine/Core/Base.h" // u64, sizet typedefs used in this header

namespace OloEngine
{
    class UUID
    {
      public:
        // Draws a fresh random ID. Safe to call from any thread, and two IDs
        // drawn by the same process cannot collide before counter wrap
        // (issue #1420, see UUID.cpp).
        UUID();
        // constexpr and defined inline so a UUID constant can be constant-initialised
        // rather than needing a global constructor (issue #763). The default ctor stays
        // out of line: it needs real OS entropy and cannot be constexpr.
        explicit(false) constexpr UUID(u64 uuid)
            : m_UUID(uuid)
        {
        }
        UUID(const UUID&) = default;

        // How many times the calling thread has drawn from the generator. Every
        // UUID() call adds at least one. Lets a test prove that building a value
        // did not draw an ID (issue #1420: POD render commands built on
        // submission workers must not).
        [[nodiscard]] static u64 GetDrawCountOnThisThread();

        operator u64()
        {
            return m_UUID;
        }
        operator u64() const
        {
            return m_UUID;
        }

      private:
        u64 m_UUID;
    };
} // namespace OloEngine

namespace std
{
    template<typename T>
    struct hash;

    template<>
    struct hash<OloEngine::UUID>
    {
        sizet operator()(const OloEngine::UUID& uuid) const
        {
            return static_cast<u64>(uuid);
        }
    };
} // namespace std
