#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    void AnimationParameterSet::DefineFloat(const std::string& name, f32 defaultValue)
    {
        AnimationParameter param;
        param.Name = name;
        param.ParamType = AnimationParameterType::Float;
        param.FloatValue = defaultValue;
        m_Parameters[name] = param;
    }

    void AnimationParameterSet::DefineBool(const std::string& name, bool defaultValue)
    {
        AnimationParameter param;
        param.Name = name;
        param.ParamType = AnimationParameterType::Bool;
        param.BoolValue = defaultValue;
        m_Parameters[name] = param;
    }

    void AnimationParameterSet::DefineInt(const std::string& name, i32 defaultValue)
    {
        AnimationParameter param;
        param.Name = name;
        param.ParamType = AnimationParameterType::Int;
        param.IntValue = defaultValue;
        m_Parameters[name] = param;
    }

    void AnimationParameterSet::DefineTrigger(const std::string& name)
    {
        AnimationParameter param;
        param.Name = name;
        param.ParamType = AnimationParameterType::Trigger;
        param.BoolValue = false;
        param.TriggerConsumed = false;
        m_Parameters[name] = param;
    }

    void AnimationParameterSet::RemoveParameter(const std::string& name)
    {
        m_Parameters.erase(name);
    }

    void AnimationParameterSet::SetFloat(const std::string& name, f32 value)
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            it->second.FloatValue = value;
        }
        else
        {
            OLO_CORE_WARN("AnimationParameterSet::SetFloat - parameter '{}' not found", name);
        }
    }

    void AnimationParameterSet::SetBool(const std::string& name, bool value)
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            it->second.BoolValue = value;
        }
        else
        {
            OLO_CORE_WARN("AnimationParameterSet::SetBool - parameter '{}' not found", name);
        }
    }

    void AnimationParameterSet::SetInt(const std::string& name, i32 value)
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            it->second.IntValue = value;
        }
        else
        {
            OLO_CORE_WARN("AnimationParameterSet::SetInt - parameter '{}' not found", name);
        }
    }

    void AnimationParameterSet::SetTrigger(const std::string& name)
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            if (it->second.ParamType != AnimationParameterType::Trigger)
            {
                OLO_CORE_WARN("AnimationParameterSet::SetTrigger - parameter '{}' is not a Trigger", name);
                return;
            }
            it->second.BoolValue = true;
            it->second.TriggerConsumed = false;
        }
        else
        {
            OLO_CORE_WARN("AnimationParameterSet::SetTrigger - parameter '{}' not found", name);
        }
    }

    f32 AnimationParameterSet::GetFloat(const std::string& name) const
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            return it->second.FloatValue;
        }
        return 0.0f;
    }

    bool AnimationParameterSet::GetBool(const std::string& name) const
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            return it->second.BoolValue;
        }
        return false;
    }

    i32 AnimationParameterSet::GetInt(const std::string& name) const
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            return it->second.IntValue;
        }
        return 0;
    }

    bool AnimationParameterSet::IsTriggerSet(const std::string& name) const
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            if (it->second.ParamType != AnimationParameterType::Trigger)
            {
                return false;
            }
            return it->second.BoolValue && !it->second.TriggerConsumed;
        }
        return false;
    }

    void AnimationParameterSet::ConsumeTrigger(const std::string& name)
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            if (it->second.ParamType != AnimationParameterType::Trigger)
            {
                return;
            }
            it->second.TriggerConsumed = true;
            it->second.BoolValue = false;
        }
    }

    bool AnimationParameterSet::HasParameter(const std::string& name) const
    {
        return m_Parameters.contains(name);
    }

    const AnimationParameter* AnimationParameterSet::GetParameter(const std::string& name) const
    {
        if (auto it = m_Parameters.find(name); it != m_Parameters.end())
        {
            return &it->second;
        }
        return nullptr;
    }
} // namespace OloEngine
