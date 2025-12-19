#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"

#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>

// Centralized YAML converter utility functions
// Use these functions instead of template specializations to avoid ODR violations
namespace OloEngine::YAMLUtils
{

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

        v.x = node[0].as<f32>();
        v.y = node[1].as<f32>();
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

        v.x = node[0].as<f32>();
        v.y = node[1].as<f32>();
        v.z = node[2].as<f32>();
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

        v.x = node[0].as<f32>();
        v.y = node[1].as<f32>();
        v.z = node[2].as<f32>();
        v.w = node[3].as<f32>();
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
                m[i][j] = node[i * 3 + j].as<f32>();
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
                m[i][j] = node[i * 4 + j].as<f32>();
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
