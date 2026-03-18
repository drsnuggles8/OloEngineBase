#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationParameter.h"

namespace OloEngine
{
    struct AnimationGraphComponent
    {
        AssetHandle AnimationGraphAssetHandle = 0;

        // Runtime
        Ref<AnimationGraph> RuntimeGraph;

        // Per-entity parameter instance (copied from graph at start)
        AnimationParameterSet Parameters;

        AnimationGraphComponent() = default;

        // Copying shares the RuntimeGraph Ref which causes aliasing.
        // Reset RuntimeGraph on copy so each entity gets its own runtime instance via lazy-load.
        AnimationGraphComponent(const AnimationGraphComponent& other)
            : AnimationGraphAssetHandle(other.AnimationGraphAssetHandle), Parameters(other.Parameters)
        {
        }
        AnimationGraphComponent& operator=(const AnimationGraphComponent& other)
        {
            if (this != &other)
            {
                AnimationGraphAssetHandle = other.AnimationGraphAssetHandle;
                RuntimeGraph = nullptr;
                Parameters = other.Parameters;
            }
            return *this;
        }
        AnimationGraphComponent(AnimationGraphComponent&&) = default;
        AnimationGraphComponent& operator=(AnimationGraphComponent&&) = default;
    };
} // namespace OloEngine
