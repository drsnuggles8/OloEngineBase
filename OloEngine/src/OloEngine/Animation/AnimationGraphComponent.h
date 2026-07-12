#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationParameter.h"

#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    struct AnimationGraphComponent
    {
        AssetHandle AnimationGraphAssetHandle = 0;

        // Runtime
        Ref<AnimationGraph> RuntimeGraph;

        // Per-entity parameter instance (copied from graph at start)
        AnimationParameterSet Parameters;

        // Runtime-only cache of the entity skeleton's bind-pose local transforms
        // in TRS form, so AnimationGraphSystem::Update doesn't re-decompose every
        // bind-pose mat4 each tick (the bind pose is stable for a given skeleton).
        // Filled lazily and rebuilt only when the bone count changes; reset on
        // copy like RuntimeGraph.
        std::vector<BoneTransform> BindPoseLocalTRS;

        // Runtime, per-tick (not serialized): the masked root-motion delta the
        // last graph update extracted from the base layer, in entity/model
        // space. Overwritten every update; consumed and cleared by
        // Scene::UpdateRootMotion on the runtime path (issue #631).
        glm::vec3 RootMotionTranslation = glm::vec3(0.0f);
        glm::quat RootMotionRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        bool HasRootMotion = false;

        AnimationGraphComponent() = default;

        // Copying shares the RuntimeGraph Ref which causes aliasing.
        // Reset RuntimeGraph (and the bind-pose cache) on copy so each entity gets
        // its own runtime instance via lazy-load.
        AnimationGraphComponent(const AnimationGraphComponent& other)
            : AnimationGraphAssetHandle(other.AnimationGraphAssetHandle), Parameters(other.Parameters)
        {
        }
        AnimationGraphComponent(AnimationGraphComponent&&) noexcept = default;
        AnimationGraphComponent& operator=(const AnimationGraphComponent& other)
        {
            if (this != &other)
            {
                AnimationGraphAssetHandle = other.AnimationGraphAssetHandle;
                RuntimeGraph = nullptr;
                Parameters = other.Parameters;
                BindPoseLocalTRS.clear();
                RootMotionTranslation = glm::vec3(0.0f);
                RootMotionRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                HasRootMotion = false;
            }
            return *this;
        }
        AnimationGraphComponent& operator=(AnimationGraphComponent&&) noexcept = default;

        // Rule of five: a user-managed copy/move set should also declare the
        // destructor (S3624). The members clean themselves up, so default it.
        ~AnimationGraphComponent() = default;
    };
} // namespace OloEngine
