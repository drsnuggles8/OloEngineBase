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
        [[nodiscard]] static bool Serialize(const Ref<AnimationGraph>& graph, const std::string& filepath);
        [[nodiscard]] static Ref<AnimationGraph> Deserialize(const std::string& filepath);
        [[nodiscard]] static Ref<AnimationGraph> DeserializeFromString(const std::string& yamlString);

        [[nodiscard]] static bool SerializeAsset(const Ref<AnimationGraphAsset>& asset, const std::string& filepath);
        [[nodiscard]] static Ref<AnimationGraphAsset> DeserializeAsset(const std::string& filepath);

        [[nodiscard]] static std::string SerializeToString(const Ref<AnimationGraph>& graph);
    };
} // namespace OloEngine
