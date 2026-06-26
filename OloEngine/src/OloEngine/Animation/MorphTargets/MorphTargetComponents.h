#pragma once

#include "MorphTargetSet.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <algorithm>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    struct MorphTargetComponent
    {
        // Morph target data for this entity's mesh
        Ref<MorphTargetSet> MorphTargets;

        // Per-target weights (target name -> weight 0.0 to 1.0)
        std::unordered_map<std::string, f32> Weights;

        // Cached base mesh data for CPU morph evaluation (populated once from MeshSource)
        std::vector<glm::vec3> BasePositions;
        std::vector<glm::vec3> BaseNormals;

        // Tracks whether morph weights were active last frame (for transition detection)
        bool WasMorphActive = false;

        MorphTargetComponent() = default;
        MorphTargetComponent(const MorphTargetComponent&) = default;

        void SetWeight(const std::string& targetName, f32 weight)
        {
            Weights[targetName] = std::clamp(weight, 0.0f, 1.0f);
        }

        [[nodiscard("weight value must be used")]] f32 GetWeight(const std::string& targetName) const
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
        [[nodiscard("active weight check drives morph target updates")]] bool HasActiveWeights() const
        {
            return std::ranges::any_of(Weights, [](const auto& kv)
                                       { return kv.second > 1e-4f; });
        }

        // Build a flat weight vector matching the MorphTargetSet order
        [[nodiscard("ordered weights needed for GPU upload")]] std::vector<f32> GetOrderedWeights() const
        {
            if (!MorphTargets)
                return {};

            std::vector<f32> ordered(MorphTargets->GetTargetCount(), 0.0f);
            for (const auto& [name, weight] : Weights)
            {
                if (auto idx = MorphTargets->FindTargetCached(name); idx >= 0)
                    ordered[static_cast<sizet>(idx)] = weight;
            }
            return ordered;
        }
    };
} // namespace OloEngine
