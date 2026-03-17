#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    const std::string AnimationGraph::s_EmptyString;

    void AnimationGraph::Start()
    {
        for (auto& layer : Layers)
        {
            if (layer.StateMachine)
            {
                layer.StateMachine->Start(Parameters);
            }
        }
    }

    void AnimationGraph::Update(f32 dt, sizet boneCount, std::vector<glm::mat4>& outFinalBoneMatrices)
    {
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

            // Apply layer with weight and bone mask
            // For now, we use a simple bone name list (empty = all bones affected)
            // In production, you'd pass bone names from the skeleton
            std::vector<std::string> emptyBoneNames; // Placeholder - applied to all
            ApplyLayerTransforms(layer, layerTransforms, emptyBoneNames, accumulatedTransforms);
        }

        // Convert BoneTransform to matrices
        outFinalBoneMatrices.resize(boneCount);
        for (sizet i = 0; i < boneCount; ++i)
        {
            outFinalBoneMatrices[i] = BoneTransformToMatrix(accumulatedTransforms[i]);
        }
    }

    const std::string& AnimationGraph::GetCurrentStateName(i32 layerIndex) const
    {
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
        for (sizet i = 0; i < accumulatedTransforms.size() && i < layerTransforms.size(); ++i)
        {
            // Check bone mask
            std::string boneName = (i < boneNames.size()) ? boneNames[i] : "";
            if (!IsBoneAffected(layer, boneName))
            {
                continue;
            }

            f32 weight = layer.Weight;
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
        // Empty bone list = all bones affected
        if (layer.AffectedBones.empty())
        {
            return true;
        }

        // Empty bone name (no skeleton info) = affect it
        if (boneName.empty())
        {
            return true;
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
        return glm::translate(glm::mat4(1.0f), transform.Translation) * glm::mat4_cast(transform.Rotation) * glm::scale(glm::mat4(1.0f), transform.Scale);
    }
} // namespace OloEngine
