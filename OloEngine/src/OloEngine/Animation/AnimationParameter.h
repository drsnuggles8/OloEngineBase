#pragma once

#include "OloEngine/Core/Base.h"
#include <string>
#include <unordered_map>

namespace OloEngine
{
    enum class AnimationParameterType : u8
    {
        Float,
        Int,
        Bool,
        Trigger
    };

    struct AnimationParameter
    {
        std::string Name;
        AnimationParameterType ParamType = AnimationParameterType::Float;

        f32 FloatValue = 0.0f;
        i32 IntValue = 0;
        bool BoolValue = false;
        bool TriggerConsumed = false;
    };

    class AnimationParameterSet
    {
      public:
        // The sole member is a std::unordered_map whose move constructor is not
        // guaranteed noexcept (it may allocate). Declare the moves noexcept so
        // this type — and anything that holds it (AnimationGraphComponent,
        // AnimationLayer) — moves without throwing (S5018).
        AnimationParameterSet() = default;
        AnimationParameterSet(const AnimationParameterSet&) = default;
        AnimationParameterSet& operator=(const AnimationParameterSet&) = default;
        AnimationParameterSet(AnimationParameterSet&&) noexcept = default;
        AnimationParameterSet& operator=(AnimationParameterSet&&) noexcept = default;

        void DefineFloat(const std::string& name, f32 defaultValue = 0.0f);
        void DefineBool(const std::string& name, bool defaultValue = false);
        void DefineInt(const std::string& name, i32 defaultValue = 0);
        void DefineTrigger(const std::string& name);
        void RemoveParameter(const std::string& name);

        void SetFloat(const std::string& name, f32 value);
        void SetBool(const std::string& name, bool value);
        void SetInt(const std::string& name, i32 value);
        void SetTrigger(const std::string& name);

        [[nodiscard("parameter value must be used")]] f32 GetFloat(const std::string& name) const;
        [[nodiscard("parameter value must be used")]] bool GetBool(const std::string& name) const;
        [[nodiscard("parameter value must be used")]] i32 GetInt(const std::string& name) const;
        [[nodiscard("trigger state must be checked before consuming")]] bool IsTriggerSet(const std::string& name) const;
        void ConsumeTrigger(const std::string& name);

        [[nodiscard("existence check must be used")]] bool HasParameter(const std::string& name) const;
        [[nodiscard("parameter pointer must be used")]] const AnimationParameter* GetParameter(const std::string& name) const;
        [[nodiscard("parameters map needed for enumeration")]] const std::unordered_map<std::string, AnimationParameter>& GetAll() const
        {
            return m_Parameters;
        }

      private:
        std::unordered_map<std::string, AnimationParameter> m_Parameters;
    };
} // namespace OloEngine
