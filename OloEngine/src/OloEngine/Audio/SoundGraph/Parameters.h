#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include "Events.h"
#include <string>
#include <memory>
#include <unordered_map>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Base parameter class for different data types
	struct Parameter
	{
		Parameter(const Identifier& id, const std::string& name) noexcept
			: ID(id), Name(name)
		{
		}

		virtual ~Parameter() = default;

		Identifier ID;
		std::string Name;

		/// Get parameter value as string for debugging/UI
		virtual std::string ToString() const = 0;
	};

	//==============================================================================
	/// Typed parameter for specific data types
	template<typename T>
	struct TypedParameter : public Parameter
	{
		TypedParameter(const Identifier& id, const std::string& name, T initialValue) noexcept
			: Parameter(id, name), Value(initialValue)
		{
		}

		std::string ToString() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return std::to_string(Value);
			else if constexpr (std::is_same_v<T, i32>)
				return std::to_string(Value);
			else if constexpr (std::is_same_v<T, bool>)
				return Value ? "true" : "false";
			else
				return "unknown";
		}

		T Value;
	};

	//==============================================================================
	/// Parameter registry for a node
	class ParameterRegistry
	{
	public:
		/// Add a parameter of specific type
		template<typename T>
		void AddParameter(const Identifier& id, const std::string& name, T initialValue)
		{
			auto param = std::make_shared<TypedParameter<T>>(id, name, initialValue);
			m_Parameters[id] = param;
		}

		/// Get parameter by ID
		template<typename T>
		std::shared_ptr<TypedParameter<T>> GetParameter(const Identifier& id) const
		{
			auto it = m_Parameters.find(id);
			if (it != m_Parameters.end())
			{
				return std::dynamic_pointer_cast<TypedParameter<T>>(it->second);
			}
			return nullptr;
		}

		/// Get parameter value by ID
		template<typename T>
		T GetParameterValue(const Identifier& id, T defaultValue = T{}) const
		{
			auto param = GetParameter<T>(id);
			return param ? param->Value : defaultValue;
		}

		/// Set parameter value by ID
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			auto param = GetParameter<T>(id);
			if (param)
				param->Value = value;
		}

		/// Get all parameters
		const std::unordered_map<Identifier, std::shared_ptr<Parameter>>& GetParameters() const
		{
			return m_Parameters;
		}

	private:
		std::unordered_map<Identifier, std::shared_ptr<Parameter>> m_Parameters;
	};

} // namespace OloEngine::Audio::SoundGraph