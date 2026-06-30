#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <yaml-cpp/yaml.h>

#include <cmath>

// Centralized YAML converter utility functions
// Use these functions instead of template specializations to avoid ODR violations
namespace OloEngine::YAMLUtils
{
    // Read a finite f32 from a YAML node. Returns false if the node is missing
    // or if the value is NaN / Inf — callers should fail-fast and let the
    // calling Decode return false, which propagates a parse error to yaml-cpp.
    // See docs/agent-rules/cpp-coding-quality.md §2b.
    inline bool TryReadFiniteF32(const YAML::Node& node, f32& out)
    {
        if (!node)
            return false;
        const auto value = node.as<f32>();
        if (!std::isfinite(value))
            return false;
        out = value;
        return true;
    }

    // UUID conversion functions
    inline YAML::Node EncodeUUID(const UUID& uuid)
    {
        YAML::Node node;
        node.push_back(static_cast<u64>(uuid));
        return node;
    }

    inline bool DecodeUUID(const YAML::Node& node, UUID& uuid)
    {
        if (!node.IsScalar())
            return false;

        uuid = node.as<u64>();
        return true;
    }

    // Note: AssetHandle is just a type alias for UUID, so we don't need separate functions

    // glm::vec2 conversion functions
    inline YAML::Node EncodeVec2(const glm::vec2& v)
    {
        YAML::Node node;
        node.push_back(v.x);
        node.push_back(v.y);
        node.SetStyle(YAML::EmitterStyle::Flow);
        return node;
    }

    inline bool DecodeVec2(const YAML::Node& node, glm::vec2& v)
    {
        if ((!node.IsSequence()) || (node.size() != 2))
            return false;

        if (!TryReadFiniteF32(node[0], v.x))
            return false;
        if (!TryReadFiniteF32(node[1], v.y))
            return false;
        return true;
    }

    // glm::vec3 conversion functions
    inline YAML::Node EncodeVec3(const glm::vec3& v)
    {
        YAML::Node node;
        node.push_back(v.x);
        node.push_back(v.y);
        node.push_back(v.z);
        node.SetStyle(YAML::EmitterStyle::Flow);
        return node;
    }

    inline bool DecodeVec3(const YAML::Node& node, glm::vec3& v)
    {
        if ((!node.IsSequence()) || (node.size() != 3))
            return false;

        if (!TryReadFiniteF32(node[0], v.x))
            return false;
        if (!TryReadFiniteF32(node[1], v.y))
            return false;
        if (!TryReadFiniteF32(node[2], v.z))
            return false;
        return true;
    }

    // glm::vec4 conversion functions
    inline YAML::Node EncodeVec4(const glm::vec4& v)
    {
        YAML::Node node;
        node.push_back(v.x);
        node.push_back(v.y);
        node.push_back(v.z);
        node.push_back(v.w);
        node.SetStyle(YAML::EmitterStyle::Flow);
        return node;
    }

    inline bool DecodeVec4(const YAML::Node& node, glm::vec4& v)
    {
        if ((!node.IsSequence()) || (node.size() != 4))
            return false;

        if (!TryReadFiniteF32(node[0], v.x))
            return false;
        if (!TryReadFiniteF32(node[1], v.y))
            return false;
        if (!TryReadFiniteF32(node[2], v.z))
            return false;
        if (!TryReadFiniteF32(node[3], v.w))
            return false;
        return true;
    }

    // glm::quat conversion functions. Serialized as a 4-element flow sequence in
    // scalar-first [w, x, y, z] order — matching glm's quat(w, x, y, z) constructor
    // argument order (the .x/.y/.z/.w members store the imaginary-first layout, but
    // the on-disk order is the human-readable constructor order). All four
    // components are validated finite; decode does NOT normalize — leave that to the
    // component (e.g. TransformComponent::SetRotation), which has bespoke handling.
    inline YAML::Node EncodeQuat(const glm::quat& q)
    {
        YAML::Node node;
        node.push_back(q.w);
        node.push_back(q.x);
        node.push_back(q.y);
        node.push_back(q.z);
        node.SetStyle(YAML::EmitterStyle::Flow);
        return node;
    }

    inline bool DecodeQuat(const YAML::Node& node, glm::quat& q)
    {
        if ((!node.IsSequence()) || (node.size() != 4))
            return false;

        if (!TryReadFiniteF32(node[0], q.w))
            return false;
        if (!TryReadFiniteF32(node[1], q.x))
            return false;
        if (!TryReadFiniteF32(node[2], q.y))
            return false;
        if (!TryReadFiniteF32(node[3], q.z))
            return false;
        return true;
    }

    // glm::ivec2 conversion functions (integers — no finiteness check needed)
    inline YAML::Node EncodeIVec2(const glm::ivec2& v)
    {
        YAML::Node node;
        node.push_back(v.x);
        node.push_back(v.y);
        node.SetStyle(YAML::EmitterStyle::Flow);
        return node;
    }

    inline bool DecodeIVec2(const YAML::Node& node, glm::ivec2& v)
    {
        if ((!node.IsSequence()) || (node.size() != 2))
            return false;

        v.x = node[0].as<i32>();
        v.y = node[1].as<i32>();
        return true;
    }

    // glm::ivec3 conversion functions
    inline YAML::Node EncodeIVec3(const glm::ivec3& v)
    {
        YAML::Node node;
        node.push_back(v.x);
        node.push_back(v.y);
        node.push_back(v.z);
        node.SetStyle(YAML::EmitterStyle::Flow);
        return node;
    }

    inline bool DecodeIVec3(const YAML::Node& node, glm::ivec3& v)
    {
        if ((!node.IsSequence()) || (node.size() != 3))
            return false;

        v.x = node[0].as<i32>();
        v.y = node[1].as<i32>();
        v.z = node[2].as<i32>();
        return true;
    }

    // glm::ivec4 conversion functions (integers — no finiteness check needed)
    inline YAML::Node EncodeIVec4(const glm::ivec4& v)
    {
        YAML::Node node;
        node.push_back(v.x);
        node.push_back(v.y);
        node.push_back(v.z);
        node.push_back(v.w);
        node.SetStyle(YAML::EmitterStyle::Flow);
        return node;
    }

    inline bool DecodeIVec4(const YAML::Node& node, glm::ivec4& v)
    {
        if ((!node.IsSequence()) || (node.size() != 4))
            return false;

        v.x = node[0].as<i32>();
        v.y = node[1].as<i32>();
        v.z = node[2].as<i32>();
        v.w = node[3].as<i32>();
        return true;
    }

    // glm::mat3 conversion functions
    inline YAML::Node EncodeMat3(const glm::mat3& m)
    {
        YAML::Node node;
        node.SetStyle(YAML::EmitterStyle::Flow);
        for (i32 i = 0; i < 3; ++i)
        {
            for (i32 j = 0; j < 3; ++j)
            {
                node.push_back(m[i][j]);
            }
        }
        return node;
    }

    inline bool DecodeMat3(const YAML::Node& node, glm::mat3& m)
    {
        if (!node.IsSequence() || node.size() != 9)
            return false;

        for (i32 i = 0; i < 3; ++i)
        {
            for (i32 j = 0; j < 3; ++j)
            {
                if (!TryReadFiniteF32(node[i * 3 + j], m[i][j]))
                    return false;
            }
        }
        return true;
    }

    // glm::mat4 conversion functions
    inline YAML::Node EncodeMat4(const glm::mat4& m)
    {
        YAML::Node node;
        node.SetStyle(YAML::EmitterStyle::Flow);
        for (i32 i = 0; i < 4; ++i)
        {
            for (i32 j = 0; j < 4; ++j)
            {
                node.push_back(m[i][j]);
            }
        }
        return node;
    }

    inline bool DecodeMat4(const YAML::Node& node, glm::mat4& m)
    {
        if (!node.IsSequence() || node.size() != 16)
            return false;

        for (i32 i = 0; i < 4; ++i)
        {
            for (i32 j = 0; j < 4; ++j)
            {
                if (!TryReadFiniteF32(node[i * 4 + j], m[i][j]))
                    return false;
            }
        }
        return true;
    }

} // namespace OloEngine::YAMLUtils

// Template specializations for YAML-cpp
// Each specialization is conditionally defined to avoid ODR violations
namespace YAML
{

#ifndef OLOENGINE_YAML_UUID_DEFINED
#define OLOENGINE_YAML_UUID_DEFINED
    template<>
    struct convert<OloEngine::UUID>
    {
        static Node encode(const OloEngine::UUID& uuid)
        {
            return OloEngine::YAMLUtils::EncodeUUID(uuid);
        }

        static bool decode(const Node& node, OloEngine::UUID& uuid)
        {
            return OloEngine::YAMLUtils::DecodeUUID(node, uuid);
        }
    };
#endif

    // Note: AssetHandle is a type alias for UUID, so the above specialization handles both types

// YAML::Emitter operator overloads for glm types
// These provide streaming support for writing YAML files
#ifndef OLOENGINE_YAML_EMITTER_GLM_DEFINED
#define OLOENGINE_YAML_EMITTER_GLM_DEFINED

    inline Emitter& operator<<(Emitter& out, const glm::vec2& v)
    {
        out << Flow;
        out << BeginSeq << v.x << v.y << EndSeq;
        return out;
    }

    inline Emitter& operator<<(Emitter& out, const glm::vec3& v)
    {
        out << Flow;
        out << BeginSeq << v.x << v.y << v.z << EndSeq;
        return out;
    }

    inline Emitter& operator<<(Emitter& out, const glm::vec4& v)
    {
        out << Flow;
        out << BeginSeq << v.x << v.y << v.z << v.w << EndSeq;
        return out;
    }

    inline Emitter& operator<<(Emitter& out, const glm::ivec3& v)
    {
        out << Flow;
        out << BeginSeq << v.x << v.y << v.z << EndSeq;
        return out;
    }

    inline Emitter& operator<<(Emitter& out, const glm::ivec2& v)
    {
        out << Flow;
        out << BeginSeq << v.x << v.y << EndSeq;
        return out;
    }

    inline Emitter& operator<<(Emitter& out, const glm::ivec4& v)
    {
        out << Flow;
        out << BeginSeq << v.x << v.y << v.z << v.w << EndSeq;
        return out;
    }

    // Scalar-first [w, x, y, z] order — matches EncodeQuat / DecodeQuat.
    inline Emitter& operator<<(Emitter& out, const glm::quat& q)
    {
        out << Flow;
        out << BeginSeq << q.w << q.x << q.y << q.z << EndSeq;
        return out;
    }

    // mat3 / mat4 emitted as a flat flow sequence of 9 / 16 floats in glm's native
    // column-major order: glm::mat* is column-major, so m[i] is column i and the
    // m[i][j] loop walks column-by-column. Matches EncodeMat3 / EncodeMat4 (same
    // iteration) and DecodeMat3 / DecodeMat4 (which read node[i*N+j] back into
    // m[i][j]), so the round-trip is order-consistent.
    inline Emitter& operator<<(Emitter& out, const glm::mat3& m)
    {
        out << Flow << BeginSeq;
        for (i32 i = 0; i < 3; ++i)
            for (i32 j = 0; j < 3; ++j)
                out << m[i][j];
        out << EndSeq;
        return out;
    }

    inline Emitter& operator<<(Emitter& out, const glm::mat4& m)
    {
        out << Flow << BeginSeq;
        for (i32 i = 0; i < 4; ++i)
            for (i32 j = 0; j < 4; ++j)
                out << m[i][j];
        out << EndSeq;
        return out;
    }

#endif

#ifndef OLOENGINE_YAML_VEC2_DEFINED
#define OLOENGINE_YAML_VEC2_DEFINED
    template<>
    struct convert<glm::vec2>
    {
        static Node encode(const glm::vec2& v)
        {
            return OloEngine::YAMLUtils::EncodeVec2(v);
        }

        static bool decode(const Node& node, glm::vec2& v)
        {
            return OloEngine::YAMLUtils::DecodeVec2(node, v);
        }
    };
#endif

#ifndef OLOENGINE_YAML_VEC3_DEFINED
#define OLOENGINE_YAML_VEC3_DEFINED
    template<>
    struct convert<glm::vec3>
    {
        static Node encode(const glm::vec3& v)
        {
            return OloEngine::YAMLUtils::EncodeVec3(v);
        }

        static bool decode(const Node& node, glm::vec3& v)
        {
            return OloEngine::YAMLUtils::DecodeVec3(node, v);
        }
    };
#endif

#ifndef OLOENGINE_YAML_VEC4_DEFINED
#define OLOENGINE_YAML_VEC4_DEFINED
    template<>
    struct convert<glm::vec4>
    {
        static Node encode(const glm::vec4& v)
        {
            return OloEngine::YAMLUtils::EncodeVec4(v);
        }

        static bool decode(const Node& node, glm::vec4& v)
        {
            return OloEngine::YAMLUtils::DecodeVec4(node, v);
        }
    };
#endif

#ifndef OLOENGINE_YAML_IVEC2_DEFINED
#define OLOENGINE_YAML_IVEC2_DEFINED
    template<>
    struct convert<glm::ivec2>
    {
        static Node encode(const glm::ivec2& v)
        {
            return OloEngine::YAMLUtils::EncodeIVec2(v);
        }

        static bool decode(const Node& node, glm::ivec2& v)
        {
            return OloEngine::YAMLUtils::DecodeIVec2(node, v);
        }
    };
#endif

#ifndef OLOENGINE_YAML_IVEC3_DEFINED
#define OLOENGINE_YAML_IVEC3_DEFINED
    template<>
    struct convert<glm::ivec3>
    {
        static Node encode(const glm::ivec3& v)
        {
            return OloEngine::YAMLUtils::EncodeIVec3(v);
        }

        static bool decode(const Node& node, glm::ivec3& v)
        {
            return OloEngine::YAMLUtils::DecodeIVec3(node, v);
        }
    };
#endif

#ifndef OLOENGINE_YAML_IVEC4_DEFINED
#define OLOENGINE_YAML_IVEC4_DEFINED
    template<>
    struct convert<glm::ivec4>
    {
        static Node encode(const glm::ivec4& v)
        {
            return OloEngine::YAMLUtils::EncodeIVec4(v);
        }

        static bool decode(const Node& node, glm::ivec4& v)
        {
            return OloEngine::YAMLUtils::DecodeIVec4(node, v);
        }
    };
#endif

#ifndef OLOENGINE_YAML_QUAT_DEFINED
#define OLOENGINE_YAML_QUAT_DEFINED
    template<>
    struct convert<glm::quat>
    {
        static Node encode(const glm::quat& q)
        {
            return OloEngine::YAMLUtils::EncodeQuat(q);
        }

        static bool decode(const Node& node, glm::quat& q)
        {
            return OloEngine::YAMLUtils::DecodeQuat(node, q);
        }
    };
#endif

#ifndef OLOENGINE_YAML_MAT3_DEFINED
#define OLOENGINE_YAML_MAT3_DEFINED
    template<>
    struct convert<glm::mat3>
    {
        static Node encode(const glm::mat3& m)
        {
            return OloEngine::YAMLUtils::EncodeMat3(m);
        }

        static bool decode(const Node& node, glm::mat3& m)
        {
            return OloEngine::YAMLUtils::DecodeMat3(node, m);
        }
    };
#endif

#ifndef OLOENGINE_YAML_MAT4_DEFINED
#define OLOENGINE_YAML_MAT4_DEFINED
    template<>
    struct convert<glm::mat4>
    {
        static Node encode(const glm::mat4& m)
        {
            return OloEngine::YAMLUtils::EncodeMat4(m);
        }

        static bool decode(const Node& node, glm::mat4& m)
        {
            return OloEngine::YAMLUtils::DecodeMat4(node, m);
        }
    };
#endif

} // namespace YAML
