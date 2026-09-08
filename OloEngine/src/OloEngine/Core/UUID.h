#pragma once

#include "OloEngine/Core/Base.h" // u64, sizet typedefs used in this header

namespace OloEngine
{
    class UUID
    {
      public:
        UUID();
        // constexpr and defined inline so a UUID constant can be constant-initialised
        // rather than needing a global constructor (issue #763). The default ctor stays
        // out of line: it needs real OS entropy and cannot be constexpr.
        explicit(false) constexpr UUID(u64 uuid)
            : m_UUID(uuid)
        {
        }
        UUID(const UUID&) = default;

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
