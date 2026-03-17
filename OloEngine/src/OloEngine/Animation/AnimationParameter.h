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
		void DefineFloat(const std::string& name, f32 defaultValue = 0.0f);
		void DefineBool(const std::string& name, bool defaultValue = false);
		void DefineInt(const std::string& name, i32 defaultValue = 0);
		void DefineTrigger(const std::string& name);
		void RemoveParameter(const std::string& name);

		void SetFloat(const std::string& name, f32 value);
		void SetBool(const std::string& name, bool value);
		void SetInt(const std::string& name, i32 value);
		void SetTrigger(const std::string& name);

		[[nodiscard]] f32 GetFloat(const std::string& name) const;
		[[nodiscard]] bool GetBool(const std::string& name) const;
		[[nodiscard]] i32 GetInt(const std::string& name) const;
		[[nodiscard]] bool IsTriggerSet(const std::string& name) const;
		void ConsumeTrigger(const std::string& name);

		[[nodiscard]] bool HasParameter(const std::string& name) const;
		[[nodiscard]] const AnimationParameter* GetParameter(const std::string& name) const;
		[[nodiscard]] const std::unordered_map<std::string, AnimationParameter>& GetAll() const { return m_Parameters; }

	private:
		std::unordered_map<std::string, AnimationParameter> m_Parameters;
	};
} // namespace OloEngine
