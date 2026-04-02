#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationGraphAsset.h"
#include <string>

namespace OloEngine
{
    class AnimationGraphSerializer
    {
      public:
        [[nodiscard("serialization success must be checked")]] static bool Serialize(const Ref<AnimationGraph>& graph, const std::string& filepath);
        [[nodiscard("deserialized graph must be assigned")]] static Ref<AnimationGraph> Deserialize(const std::string& filepath);
        [[nodiscard("deserialized graph must be assigned")]] static Ref<AnimationGraph> DeserializeFromString(const std::string& yamlString);

        [[nodiscard("serialization success must be checked")]] static bool SerializeAsset(const Ref<AnimationGraphAsset>& asset, const std::string& filepath);
        [[nodiscard("deserialized asset must be assigned")]] static Ref<AnimationGraphAsset> DeserializeAsset(const std::string& filepath);

        [[nodiscard("serialized string must be used")]] static std::string SerializeToString(const Ref<AnimationGraph>& graph);
    };
} // namespace OloEngine
