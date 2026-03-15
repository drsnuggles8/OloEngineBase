#pragma once

// @file ArchiveExtensions.h
// @brief FArchive operator<< overloads for GLM types, containers, UUID, and AssetHandle
//
// Extends the base FArchive serialization with support for:
// - GLM vectors (vec2/3/4, ivec2/3/4), matrices (mat3/4), quaternions
// - Standard containers (std::vector<T>, std::unordered_map<K,V>)
// - OloEngine types (UUID, AssetHandle)

#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <concepts>
#include <limits>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Note: GLM/mat/quat operators are templated with std::derived_from<FArchive>
    // to win overload resolution against the unconstrained OStream template
    // operator<< in Log.h (which matches any type including FMemoryWriter/Reader).

    // ========================================================================
    // GLM Vector Types
    // ========================================================================

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::vec2& v)
    {
        static_cast<FArchive&>(ar) << v.x << v.y;
        return ar;
    }

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::vec3& v)
    {
        static_cast<FArchive&>(ar) << v.x << v.y << v.z;
        return ar;
    }

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::vec4& v)
    {
        static_cast<FArchive&>(ar) << v.x << v.y << v.z << v.w;
        return ar;
    }

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::ivec2& v)
    {
        static_cast<FArchive&>(ar) << v.x << v.y;
        return ar;
    }

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::ivec3& v)
    {
        static_cast<FArchive&>(ar) << v.x << v.y << v.z;
        return ar;
    }

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::ivec4& v)
    {
        static_cast<FArchive&>(ar) << v.x << v.y << v.z << v.w;
        return ar;
    }

    // ========================================================================
    // GLM Matrix Types
    // ========================================================================

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::mat3& m)
    {
        for (i32 i = 0; i < 3; ++i)
        {
            ar << m[i];
        }
        return ar;
    }

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::mat4& m)
    {
        for (i32 i = 0; i < 4; ++i)
        {
            ar << m[i];
        }
        return ar;
    }

    // ========================================================================
    // GLM Quaternion
    // ========================================================================

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, glm::quat& q)
    {
        static_cast<FArchive&>(ar) << q.x << q.y << q.z << q.w;
        return ar;
    }

    // ========================================================================
    // UUID and AssetHandle
    // ========================================================================

    template<std::derived_from<FArchive> Ar>
    Ar& operator<<(Ar& ar, UUID& uuid)
    {
        u64 val = static_cast<u64>(uuid);
        static_cast<FArchive&>(ar) << val;
        if (ar.IsLoading())
        {
            uuid = UUID(val);
        }
        return ar;
    }

    // AssetHandle is a typedef for UUID, so the UUID overload covers it.

    // ========================================================================
    // Container Types
    // ========================================================================

    // Sanity cap for container deserialization to prevent huge allocations from corrupt data
    static constexpr u32 kMaxContainerDeserializeCount = 10'000'000;

    template<std::derived_from<FArchive> Ar, typename T>
    Ar& operator<<(Ar& ar, std::vector<T>& vec)
    {
        if (!ar.IsLoading())
        {
            sizet rawSize = vec.size();
            if (rawSize > kMaxContainerDeserializeCount || rawSize > std::numeric_limits<u32>::max())
            {
                ar.SetError();
                return ar;
            }
        }

        u32 count = static_cast<u32>(vec.size());
        static_cast<FArchive&>(ar) << count;

        if (ar.IsLoading())
        {
            if (count > kMaxContainerDeserializeCount)
            {
                ar.SetError();
                return ar;
            }
            std::vector<T> temp(count);
            for (u32 i = 0; i < count; ++i)
            {
                ar << temp[i];
                if (ar.IsError())
                {
                    return ar;
                }
            }
            vec = std::move(temp);
        }
        else
        {
            for (u32 i = 0; i < count; ++i)
            {
                ar << vec[i];
            }
        }
        return ar;
    }

    template<std::derived_from<FArchive> Ar, typename K, typename V>
    Ar& operator<<(Ar& ar, std::unordered_map<K, V>& map)
    {
        if (!ar.IsLoading())
        {
            sizet rawSize = map.size();
            if (rawSize > kMaxContainerDeserializeCount || rawSize > std::numeric_limits<u32>::max())
            {
                ar.SetError();
                return ar;
            }
        }

        u32 count = static_cast<u32>(map.size());
        static_cast<FArchive&>(ar) << count;

        if (ar.IsLoading())
        {
            if (count > kMaxContainerDeserializeCount)
            {
                ar.SetError();
                return ar;
            }
            std::unordered_map<K, V> temp;
            temp.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                K key{};
                V value{};
                ar << key << value;
                if (ar.IsError())
                {
                    return ar;
                }
                auto [it, inserted] = temp.emplace(std::move(key), std::move(value));
                if (!inserted)
                {
                    ar.SetError();
                    return ar;
                }
            }
            map = std::move(temp);
        }
        else
        {
            for (auto& [key, value] : map)
            {
                // Copy to mutable local: FArchive::operator<< takes non-const ref,
                // but structured binding key is const for map types.
                K k = key;
                ar << k << value;
            }
        }
        return ar;
    }

} // namespace OloEngine
