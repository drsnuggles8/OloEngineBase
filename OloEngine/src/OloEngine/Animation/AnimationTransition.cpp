#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include <algorithm>
#include <cmath>

namespace OloEngine
{
    static constexpr f32 kFloatEpsilon = 1e-6f;

    bool TransitionCondition::Evaluate(const AnimationParameterSet& params) const
    {
        using enum AnimationParameterType;
        const AnimationParameter* param = params.GetParameter(ParameterName);
        if (!param)
        {
            return false;
        }

        switch (Op)
        {
            case Comparison::TriggerSet:
                return params.IsTriggerSet(ParameterName);

            case Comparison::Greater:
                if (param->ParamType == Float)
                    return param->FloatValue > FloatThreshold;
                if (param->ParamType == Int)
                    return param->IntValue > IntThreshold;
                return false;

            case Comparison::Less:
                if (param->ParamType == Float)
                    return param->FloatValue < FloatThreshold;
                if (param->ParamType == Int)
                    return param->IntValue < IntThreshold;
                return false;

            case Comparison::Equal:
                if (param->ParamType == Float)
                    return std::abs(param->FloatValue - FloatThreshold) < kFloatEpsilon;
                if (param->ParamType == Int)
                    return param->IntValue == IntThreshold;
                if (param->ParamType == Bool)
                    return param->BoolValue == BoolValue;
                return false;

            case Comparison::NotEqual:
                if (param->ParamType == Float)
                    return std::abs(param->FloatValue - FloatThreshold) >= kFloatEpsilon;
                if (param->ParamType == Int)
                    return param->IntValue != IntThreshold;
                if (param->ParamType == Bool)
                    return param->BoolValue != BoolValue;
                return false;

            default:
                return false;
        }

        return false;
    }

    bool AnimationTransition::Evaluate(const AnimationParameterSet& params) const
    {
        return std::ranges::all_of(Conditions, [&params](const auto& condition)
                                   { return condition.Evaluate(params); });
    }
} // namespace OloEngine
