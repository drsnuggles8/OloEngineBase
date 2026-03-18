#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include <cmath>

namespace OloEngine
{
    static constexpr f32 kFloatEpsilon = 1e-6f;

    bool TransitionCondition::Evaluate(const AnimationParameterSet& params) const
    {
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
                if (param->ParamType == AnimationParameterType::Float)
                    return param->FloatValue > FloatThreshold;
                if (param->ParamType == AnimationParameterType::Int)
                    return param->IntValue > IntThreshold;
                return false;

            case Comparison::Less:
                if (param->ParamType == AnimationParameterType::Float)
                    return param->FloatValue < FloatThreshold;
                if (param->ParamType == AnimationParameterType::Int)
                    return param->IntValue < IntThreshold;
                return false;

            case Comparison::Equal:
                if (param->ParamType == AnimationParameterType::Float)
                    return std::abs(param->FloatValue - FloatThreshold) < kFloatEpsilon;
                if (param->ParamType == AnimationParameterType::Int)
                    return param->IntValue == IntThreshold;
                if (param->ParamType == AnimationParameterType::Bool)
                    return param->BoolValue == BoolValue;
                return false;

            case Comparison::NotEqual:
                if (param->ParamType == AnimationParameterType::Float)
                    return std::abs(param->FloatValue - FloatThreshold) >= kFloatEpsilon;
                if (param->ParamType == AnimationParameterType::Int)
                    return param->IntValue != IntThreshold;
                if (param->ParamType == AnimationParameterType::Bool)
                    return param->BoolValue != BoolValue;
                return false;
        }

        return false;
    }

    bool AnimationTransition::Evaluate(const AnimationParameterSet& params) const
    {
        for (const auto& condition : Conditions)
        {
            if (!condition.Evaluate(params))
            {
                return false;
            }
        }
        return true;
    }
} // namespace OloEngine
