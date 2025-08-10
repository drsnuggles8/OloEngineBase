#include "YAMLConverters.h"

namespace YAML {

    // UUID converter implementation
    Node convert<OloEngine::UUID>::encode(const OloEngine::UUID& uuid)
    {
        Node node;
        node.push_back(static_cast<u64>(uuid));
        return node;
    }

    bool convert<OloEngine::UUID>::decode(const Node& node, OloEngine::UUID& uuid)
    {
        uuid = node.as<u64>();
        return true;
    }

    // AssetHandle converter implementation
    Node convert<OloEngine::AssetHandle>::encode(const OloEngine::AssetHandle& rhs)
    {
        Node node;
        node.push_back(static_cast<u64>(rhs));
        return node;
    }

    bool convert<OloEngine::AssetHandle>::decode(const Node& node, OloEngine::AssetHandle& rhs)
    {
        if (!node.IsScalar())
            return false;

        rhs = node.as<u64>();
        return true;
    }

    // glm::vec2 converter implementation
    Node convert<glm::vec2>::encode(const glm::vec2& rhs)
    {
        Node node;
        node.push_back(rhs.x);
        node.push_back(rhs.y);
        node.SetStyle(EmitterStyle::Flow);
        return node;
    }

    bool convert<glm::vec2>::decode(const Node& node, glm::vec2& rhs)
    {
        if ((!node.IsSequence()) || (node.size() != 2))
        {
            return false;
        }

        rhs.x = node[0].as<f32>();
        rhs.y = node[1].as<f32>();
        return true;
    }

    // glm::vec3 converter implementation
    Node convert<glm::vec3>::encode(const glm::vec3& rhs)
    {
        Node node;
        node.push_back(rhs.x);
        node.push_back(rhs.y);
        node.push_back(rhs.z);
        node.SetStyle(EmitterStyle::Flow);
        return node;
    }

    bool convert<glm::vec3>::decode(const Node& node, glm::vec3& rhs)
    {
        if ((!node.IsSequence()) || (node.size() != 3))
        {
            return false;
        }

        rhs.x = node[0].as<f32>();
        rhs.y = node[1].as<f32>();
        rhs.z = node[2].as<f32>();
        return true;
    }

    // glm::vec4 converter implementation
    Node convert<glm::vec4>::encode(const glm::vec4& rhs)
    {
        Node node;
        node.push_back(rhs.x);
        node.push_back(rhs.y);
        node.push_back(rhs.z);
        node.push_back(rhs.w);
        node.SetStyle(EmitterStyle::Flow);
        return node;
    }

    bool convert<glm::vec4>::decode(const Node& node, glm::vec4& rhs)
    {
        if ((!node.IsSequence()) || (node.size() != 4))
        {
            return false;
        }

        rhs.x = node[0].as<f32>();
        rhs.y = node[1].as<f32>();
        rhs.z = node[2].as<f32>();
        rhs.w = node[3].as<f32>();
        return true;
    }

    // glm::mat3 converter implementation
    Node convert<glm::mat3>::encode(const glm::mat3& rhs)
    {
        Node node;
        node.SetStyle(EmitterStyle::Flow);
        for (i32 i = 0; i < 3; ++i)
        {
            for (i32 j = 0; j < 3; ++j)
            {
                node.push_back(rhs[i][j]);
            }
        }
        return node;
    }

    bool convert<glm::mat3>::decode(const Node& node, glm::mat3& rhs)
    {
        if (!node.IsSequence() || node.size() != 9)
            return false;

        for (i32 i = 0; i < 3; ++i)
        {
            for (i32 j = 0; j < 3; ++j)
            {
                rhs[i][j] = node[i * 3 + j].as<f32>();
            }
        }
        return true;
    }

    // glm::mat4 converter implementation
    Node convert<glm::mat4>::encode(const glm::mat4& rhs)
    {
        Node node;
        node.SetStyle(EmitterStyle::Flow);
        for (i32 i = 0; i < 4; ++i)
        {
            for (i32 j = 0; j < 4; ++j)
            {
                node.push_back(rhs[i][j]);
            }
        }
        return node;
    }

    bool convert<glm::mat4>::decode(const Node& node, glm::mat4& rhs)
    {
        if (!node.IsSequence() || node.size() != 16)
            return false;

        for (i32 i = 0; i < 4; ++i)
        {
            for (i32 j = 0; j < 4; ++j)
            {
                rhs[i][j] = node[i * 4 + j].as<f32>();
            }
        }
        return true;
    }

}
