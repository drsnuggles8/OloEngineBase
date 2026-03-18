#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Core/Log.h"
#include <unordered_set>

namespace OloEngine
{
    const std::string AnimationGraph::s_EmptyString;

    void AnimationGraph::Start()
    {
        OLO_PROFILE_FUNCTION();
        for (auto& layer : Layers)
        {
            if (layer.StateMachine)
            {
                layer.StateMachine->Start(Parameters);
            }
        }
    }

    void AnimationGraph::Update(f32 dt, sizet boneCount, std::vector<glm::mat4>& outFinalBoneMatrices,
                                const std::vector<std::string>& boneNames)
    {
        OLO_PROFILE_FUNCTION();
        if (Layers.empty() || boneCount == 0)
        {
            outFinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
            return;
        }

        // Start with identity transforms
        std::vector<BoneTransform> accumulatedTransforms(boneCount);

        // Evaluate each layer bottom-to-top
        for (auto& layer : Layers)
        {
            if (!layer.StateMachine || layer.Weight <= 0.0f)
            {
                continue;
            }

            std::vector<BoneTransform> layerTransforms;
            layer.StateMachine->Update(dt, Parameters, boneCount, layerTransforms);

            if (layerTransforms.size() < boneCount)
            {
                layerTransforms.resize(boneCount);
            }

            // Apply layer with weight and bone mask using skeleton bone names
            ApplyLayerTransforms(layer, layerTransforms, boneNames, accumulatedTransforms);
        }

        // Convert BoneTransform to matrices
        outFinalBoneMatrices.resize(boneCount);
        for (sizet i = 0; i < boneCount; ++i)
        {
            outFinalBoneMatrices[i] = BoneTransformToMatrix(accumulatedTransforms[i]);
        }
    }

    Ref<AnimationGraph> AnimationGraph::Clone() const
    {
        OLO_PROFILE_FUNCTION();
        auto clone = Ref<AnimationGraph>::Create();
        clone->Parameters = Parameters;
        clone->Layers.reserve(Layers.size());
        for (auto const& layer : Layers)
        {
            AnimationLayer clonedLayer;
            clonedLayer.Name = layer.Name;
            clonedLayer.Mode = layer.Mode;
            clonedLayer.Weight = layer.Weight;
            clonedLayer.AffectedBones = layer.AffectedBones;
            clonedLayer.AvatarMask = layer.AvatarMask;

            if (layer.StateMachine)
            {
                auto sm = Ref<AnimationStateMachine>::Create();
                for (auto const& [name, state] : layer.StateMachine->GetStates())
                {
                    AnimationState clonedState = state;
                    // Deep-clone BlendTree so ResolveClips doesn't mutate the template
                    if (state.Tree)
                    {
                        auto treeCopy = Ref<BlendTree>::Create();
                        treeCopy->Type = state.Tree->Type;
                        treeCopy->BlendParameterX = state.Tree->BlendParameterX;
                        treeCopy->BlendParameterY = state.Tree->BlendParameterY;
                        treeCopy->Children = state.Tree->Children;
                        clonedState.Tree = treeCopy;
                    }
                    sm->AddState(clonedState);
                }
                for (auto const& transition : layer.StateMachine->GetTransitions())
                {
                    sm->AddTransition(transition);
                }
                sm->SetDefaultState(layer.StateMachine->GetDefaultState());
                clonedLayer.StateMachine = sm;
            }
            clone->Layers.push_back(std::move(clonedLayer));
        }
        return clone;
    }

    void AnimationGraph::ResolveClips(const std::vector<Ref<AnimationClip>>& availableClips)
    {
        OLO_PROFILE_FUNCTION();

        if (availableClips.empty())
        {
            return;
        }

        // Build a name-to-clip lookup map
        std::unordered_map<std::string, Ref<AnimationClip>> clipMap;
        clipMap.reserve(availableClips.size());
        for (auto const& clip : availableClips)
        {
            if (clip && !clip->Name.empty())
            {
                clipMap[clip->Name] = clip;
            }
        }

        // Resolve clips in all layers' states and blend trees
        for (auto& layer : Layers)
        {
            if (!layer.StateMachine)
            {
                continue;
            }

            for (auto& [stateName, state] : layer.StateMachine->GetMutableStates())
            {
                // Resolve single-clip states
                if (state.Type == AnimationState::MotionType::SingleClip && !state.Clip && !state.ClipName.empty())
                {
                    if (auto it = clipMap.find(state.ClipName); it != clipMap.end())
                    {
                        state.Clip = it->second;
                    }
                    else
                    {
                        OLO_CORE_WARN("AnimationGraph::ResolveClips - Clip '{}' not found for state '{}'",
                                      state.ClipName, stateName);
                    }
                }

                // Resolve blend tree children
                if (state.Type == AnimationState::MotionType::BlendTree && state.Tree)
                {
                    for (auto& child : state.Tree->Children)
                    {
                        if (!child.Clip && !child.ClipName.empty())
                        {
                            if (auto it = clipMap.find(child.ClipName); it != clipMap.end())
                            {
                                child.Clip = it->second;
                            }
                            else
                            {
                                OLO_CORE_WARN("AnimationGraph::ResolveClips - Clip '{}' not found for blend tree child",
                                              child.ClipName);
                            }
                        }
                    }
                }
            }
        }
    }

    bool AnimationGraph::HasUnresolvedClips() const
    {
        for (auto const& layer : Layers)
        {
            if (!layer.StateMachine)
            {
                continue;
            }
            for (auto const& [stateName, state] : layer.StateMachine->GetStates())
            {
                if (state.Type == AnimationState::MotionType::SingleClip && !state.Clip && !state.ClipName.empty())
                {
                    return true;
                }
                if (state.Type == AnimationState::MotionType::BlendTree && state.Tree)
                {
                    for (auto const& child : state.Tree->Children)
                    {
                        if (!child.Clip && !child.ClipName.empty())
                        {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    const std::string& AnimationGraph::GetCurrentStateName(i32 layerIndex) const
    {
        OLO_PROFILE_FUNCTION();
        if (layerIndex >= 0 && layerIndex < static_cast<i32>(Layers.size()))
        {
            if (Layers[layerIndex].StateMachine)
            {
                return Layers[layerIndex].StateMachine->GetCurrentStateName();
            }
        }
        return s_EmptyString;
    }

    bool AnimationGraph::IsInTransition(i32 layerIndex) const
    {
        OLO_PROFILE_FUNCTION();
        if (layerIndex >= 0 && layerIndex < static_cast<i32>(Layers.size()))
        {
            if (Layers[layerIndex].StateMachine)
            {
                return Layers[layerIndex].StateMachine->IsInTransition();
            }
        }
        return false;
    }

    void AnimationGraph::ApplyLayerTransforms(const AnimationLayer& layer,
                                              const std::vector<BoneTransform>& layerTransforms,
                                              const std::vector<std::string>& boneNames,
                                              std::vector<BoneTransform>& accumulatedTransforms) const
    {
        OLO_PROFILE_SCOPE("ApplyLayerTransforms");

        // Build fast bone mask lookup once per layer
        std::unordered_set<std::string> affectedBoneSet(layer.AffectedBones.begin(), layer.AffectedBones.end());
        bool hasMask = !affectedBoneSet.empty();

        for (sizet i = 0; i < accumulatedTransforms.size() && i < layerTransforms.size(); ++i)
        {
            // Check bone mask using cached set
            if (hasMask)
            {
                std::string boneName = (i < boneNames.size()) ? boneNames[i] : "";
                if (boneName.empty() || affectedBoneSet.find(boneName) == affectedBoneSet.end())
                {
                    continue;
                }
            }

            f32 weight = glm::clamp(layer.Weight, 0.0f, 1.0f);
            const auto& src = layerTransforms[i];
            auto& dst = accumulatedTransforms[i];

            switch (layer.Mode)
            {
                case AnimationLayer::BlendMode::Override:
                    dst.Translation = glm::mix(dst.Translation, src.Translation, weight);
                    dst.Rotation = glm::slerp(dst.Rotation, src.Rotation, weight);
                    dst.Scale = glm::mix(dst.Scale, src.Scale, weight);
                    break;

                case AnimationLayer::BlendMode::Additive:
                    dst.Translation += src.Translation * weight;
                    // Additive rotation: multiply the delta rotation scaled by weight
                    {
                        glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
                        glm::quat additiveRot = glm::slerp(identity, src.Rotation, weight);
                        dst.Rotation = additiveRot * dst.Rotation;
                    }
                    dst.Scale *= glm::mix(glm::vec3(1.0f), src.Scale, weight);
                    break;
            }
        }
    }

    bool AnimationGraph::IsBoneAffected(const AnimationLayer& layer, const std::string& boneName) const
    {
        OLO_PROFILE_FUNCTION();
        // Empty bone list = all bones affected
        if (layer.AffectedBones.empty())
        {
            return true;
        }

        // Masked layer with no bone name info — skip to avoid applying to all bones
        if (boneName.empty())
        {
            return false;
        }

        for (auto const& affectedBone : layer.AffectedBones)
        {
            if (affectedBone == boneName)
            {
                return true;
            }
        }
        return false;
    }

    glm::mat4 AnimationGraph::BoneTransformToMatrix(const BoneTransform& transform)
    {
        OLO_PROFILE_FUNCTION();
        return glm::translate(glm::mat4(1.0f), transform.Translation) * glm::mat4_cast(transform.Rotation) * glm::scale(glm::mat4(1.0f), transform.Scale);
    }
} // namespace OloEngine
