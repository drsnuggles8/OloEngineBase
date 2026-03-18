#pragma once

#include "MorphTargetSet.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <unordered_map>

namespace OloEngine
{
    struct MorphTargetComponent
    {
        // Morph target data for this entity's mesh
        Ref<MorphTargetSet> MorphTargets;

        // Per-target weights (target name -> weight 0.0 to 1.0)
        std::unordered_map<std::string, f32> Weights;

        MorphTargetComponent() = default;
        MorphTargetComponent(const MorphTargetComponent&) = default;

        void SetWeight(const std::string& targetName, f32 weight)
        {
            Weights[targetName] = std::clamp(weight, 0.0f, 1.0f);
        }

        [[nodiscard]] f32 GetWeight(const std::string& targetName) const
        {
            auto it = Weights.find(targetName);
            return (it != Weights.end()) ? it->second : 0.0f;
        }

        void ResetAllWeights()
        {
            for (auto& [name, weight] : Weights)
            {
                weight = 0.0f;
            }
        }

        // Check if any morph target has a non-zero weight
        [[nodiscard]] bool HasActiveWeights() const
        {
            for (const auto& [name, weight] : Weights)
            {
                if (weight > 1e-4f)
                    return true;
            }
            return false;
        }

        // Build a flat weight vector matching the MorphTargetSet order
        [[nodiscard]] std::vector<f32> GetOrderedWeights() const
        {
            if (!MorphTargets)
                return {};

            std::vector<f32> ordered(MorphTargets->GetTargetCount(), 0.0f);
            for (const auto& [name, weight] : Weights)
            {
                i32 idx = MorphTargets->FindTarget(name);
                if (idx >= 0)
                    ordered[idx] = weight;
            }
            return ordered;
        }
    };
} // namespace OloEngine
