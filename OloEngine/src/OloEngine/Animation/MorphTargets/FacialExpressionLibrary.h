#pragma once

#include "MorphTargetComponents.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    struct FacialExpression
    {
        std::string Name;
        std::unordered_map<std::string, f32> TargetWeights;
    };

    class FacialExpressionLibrary
    {
      public:
        static void RegisterExpression(const FacialExpression& expr)
        {
            OLO_PROFILE_FUNCTION();
            GetExpressions()[expr.Name] = expr;
        }

        static void ApplyExpression(MorphTargetComponent& morphComp, const std::string& exprName, f32 blend = 1.0f)
        {
            OLO_PROFILE_FUNCTION();

            auto it = GetExpressions().find(exprName);
            if (it == GetExpressions().end())
                return;

            for (const auto& [targetName, weight] : it->second.TargetWeights)
            {
                morphComp.SetWeight(targetName, weight * blend);
            }
        }

        static void BlendExpressions(MorphTargetComponent& morphComp,
                                     const std::string& fromExpr, const std::string& toExpr, f32 t)
        {
            OLO_PROFILE_FUNCTION();

            auto& exprs = GetExpressions();
            auto fromIt = exprs.find(fromExpr);
            auto toIt = exprs.find(toExpr);

            if (fromIt == exprs.end() || toIt == exprs.end())
                return;

            // Collect all unique target names from both expressions
            std::unordered_map<std::string, f32> blended;
            for (const auto& [name, weight] : fromIt->second.TargetWeights)
            {
                blended[name] = weight * (1.0f - t);
            }
            for (const auto& [name, weight] : toIt->second.TargetWeights)
            {
                blended[name] += weight * t;
            }

            for (const auto& [name, weight] : blended)
            {
                morphComp.SetWeight(name, weight);
            }
        }

        static bool HasExpression(const std::string& name)
        {
            return GetExpressions().contains(name);
        }

        static const std::unordered_map<std::string, FacialExpression>& GetAllExpressions()
        {
            return GetExpressions();
        }

        static std::vector<std::string> GetExpressionNames()
        {
            std::vector<std::string> names;
            for (const auto& [name, _] : GetExpressions())
                names.push_back(name);
            std::sort(names.begin(), names.end());
            return names;
        }

        // Serialize a single expression to a .oloexpression YAML file
        static bool SaveExpression(const std::string& filePath, const FacialExpression& expr);

        // Load a single expression from a .oloexpression YAML file and register it
        static bool LoadExpression(const std::string& filePath);

      private:
        static std::unordered_map<std::string, FacialExpression>& GetExpressions()
        {
            static std::unordered_map<std::string, FacialExpression> s_Expressions;
            return s_Expressions;
        }
    };
} // namespace OloEngine
