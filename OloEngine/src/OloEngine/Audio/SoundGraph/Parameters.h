#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include "Events.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <type_traits>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Configuration for parameter interpolation
	struct InterpolationConfig
	{
		/// Number of samples over which to interpolate (default: 480 samples = 10ms at 48kHz)
		u32 InterpolationSamples = 480;
		
		/// Sample rate for calculating interpolation timing
		f64 SampleRate = 48000.0;
		
		/// Whether interpolation is enabled globally
		bool EnableInterpolation = true;
		
		/// Get interpolation time in seconds
		f64 GetInterpolationTimeSeconds() const { return InterpolationSamples / SampleRate; }
		
		/// Set interpolation time in seconds
		void SetInterpolationTimeSeconds(f64 timeSeconds) 
		{ 
			InterpolationSamples = static_cast<u32>(timeSeconds * SampleRate); 
		}
	};
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
		
		/// Check if this parameter supports interpolation
		virtual bool SupportsInterpolation() const { return false; }
		
		/// Process interpolation (called once per audio frame)
		virtual void ProcessInterpolation() {}
		
		/// Set interpolation configuration
		virtual void SetInterpolationConfig(const InterpolationConfig& config) {}
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
	/// Interpolated parameter for smooth value transitions (numeric types only)
	template<typename T>
	struct InterpolatedParameter : public TypedParameter<T>
	{
		static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, 
					  "InterpolatedParameter only supports numeric types (not bool)");

		InterpolatedParameter(const Identifier& id, const std::string& name, T initialValue) noexcept
			: TypedParameter<T>(id, name, initialValue)
			, m_CurrentValue(initialValue)
			, m_TargetValue(initialValue)
			, m_Increment(T{})
			, m_RemainingSteps(0)
		{
		}

		bool SupportsInterpolation() const override { return true; }

		void ProcessInterpolation() override
		{
			if (m_RemainingSteps > 0)
			{
				m_CurrentValue += m_Increment;
				--m_RemainingSteps;

				if (m_RemainingSteps == 0)
				{
					// Ensure we hit the exact target value
					m_CurrentValue = m_TargetValue;
				}

				// Update the public Value to the interpolated current value
				this->Value = m_CurrentValue;
			}
		}

		void SetInterpolationConfig(const InterpolationConfig& config) override
		{
			m_InterpolationConfig = config;
		}

		/// Set target value with interpolation
		void SetTargetValue(T newTarget, bool interpolate = true)
		{
			m_TargetValue = newTarget;

			if (!interpolate || !m_InterpolationConfig.EnableInterpolation)
			{
				// Immediate value change
				m_CurrentValue = newTarget;
				this->Value = newTarget;
				m_RemainingSteps = 0;
				m_Increment = T{};
			}
			else
			{
				// Calculate interpolation
				const T delta = m_TargetValue - m_CurrentValue;
				m_RemainingSteps = m_InterpolationConfig.InterpolationSamples;
				
				if (m_RemainingSteps > 0)
				{
					m_Increment = delta / static_cast<T>(m_RemainingSteps);
				}
				else
				{
					// No interpolation if steps is 0
					m_CurrentValue = newTarget;
					this->Value = newTarget;
					m_Increment = T{};
				}
			}
		}

		/// Get the current interpolated value
		T GetCurrentValue() const { return m_CurrentValue; }

		/// Get the target value
		T GetTargetValue() const { return m_TargetValue; }

		/// Check if currently interpolating
		bool IsInterpolating() const { return m_RemainingSteps > 0; }

		/// Get interpolation progress (0.0 = start, 1.0 = complete)
		f32 GetInterpolationProgress() const
		{
			if (m_InterpolationConfig.InterpolationSamples == 0)
				return 1.0f;
			
			const f32 completed = static_cast<f32>(m_InterpolationConfig.InterpolationSamples - m_RemainingSteps);
			return completed / static_cast<f32>(m_InterpolationConfig.InterpolationSamples);
		}

		/// Reset interpolation to immediate mode
		void ResetInterpolation()
		{
			m_CurrentValue = m_TargetValue;
			this->Value = m_CurrentValue;
			m_RemainingSteps = 0;
			m_Increment = T{};
		}

	private:
		T m_CurrentValue;
		T m_TargetValue;
		T m_Increment;
		u32 m_RemainingSteps;
		InterpolationConfig m_InterpolationConfig;
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

		/// Add an interpolated parameter (numeric types only)
		template<typename T>
		void AddInterpolatedParameter(const Identifier& id, const std::string& name, T initialValue, 
									  const InterpolationConfig& config = {})
		{
			static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, 
						  "Interpolated parameters only support numeric types");
			
			auto param = std::make_shared<InterpolatedParameter<T>>(id, name, initialValue);
			param->SetInterpolationConfig(config);
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

		/// Get interpolated parameter by ID
		template<typename T>
		std::shared_ptr<InterpolatedParameter<T>> GetInterpolatedParameter(const Identifier& id) const
		{
			auto it = m_Parameters.find(id);
			if (it != m_Parameters.end())
			{
				return std::dynamic_pointer_cast<InterpolatedParameter<T>>(it->second);
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

		/// Set parameter value by ID (with optional interpolation for interpolated parameters)
		template<typename T>
		void SetParameterValue(const Identifier& id, T value, bool interpolate = true)
		{
			// Try interpolated parameter first, but only for arithmetic types (not bool)
			if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
			{
				if (auto interpParam = GetInterpolatedParameter<T>(id))
				{
					interpParam->SetTargetValue(value, interpolate);
					return;
				}
			}
			
			// Fall back to regular parameter
			if (auto param = GetParameter<T>(id))
			{
				param->Value = value;
			}
		}

		/// Process all interpolated parameters (call once per audio frame)
		void ProcessInterpolation()
		{
			for (auto& [id, param] : m_Parameters)
			{
				if (param->SupportsInterpolation())
				{
					param->ProcessInterpolation();
				}
			}
		}

		/// Set interpolation configuration for all interpolated parameters
		void SetInterpolationConfig(const InterpolationConfig& config)
		{
			m_InterpolationConfig = config;
			for (auto& [id, param] : m_Parameters)
			{
				param->SetInterpolationConfig(config);
			}
		}

		/// Get current interpolation configuration
		const InterpolationConfig& GetInterpolationConfig() const { return m_InterpolationConfig; }

		/// Check if parameter exists
		bool HasParameter(const Identifier& id) const
		{
			return m_Parameters.find(id) != m_Parameters.end();
		}

		/// Check if parameter supports interpolation
		bool ParameterSupportsInterpolation(const Identifier& id) const
		{
			auto it = m_Parameters.find(id);
			return it != m_Parameters.end() && it->second->SupportsInterpolation();
		}

		/// Get all parameters
		const std::unordered_map<Identifier, std::shared_ptr<Parameter>>& GetParameters() const
		{
			return m_Parameters;
		}

	private:
		std::unordered_map<Identifier, std::shared_ptr<Parameter>> m_Parameters;
		InterpolationConfig m_InterpolationConfig;
	};

	//==============================================================================
	/// Interpolation utility functions
	namespace InterpolationUtils
	{
		/// Create a default interpolation config for a given sample rate
		inline InterpolationConfig CreateDefaultConfig(f64 sampleRate, f64 interpolationTimeSeconds = 0.01)
		{
			InterpolationConfig config;
			config.SampleRate = sampleRate;
			config.SetInterpolationTimeSeconds(interpolationTimeSeconds);
			config.EnableInterpolation = true;
			return config;
		}

		/// Create a config for immediate parameter changes (no interpolation)
		inline InterpolationConfig CreateImmediateConfig()
		{
			InterpolationConfig config;
			config.EnableInterpolation = false;
			config.InterpolationSamples = 0;
			return config;
		}

		/// Create a config for fast parameter changes (1ms interpolation)
		inline InterpolationConfig CreateFastConfig(f64 sampleRate)
		{
			return CreateDefaultConfig(sampleRate, 0.001); // 1ms
		}

		/// Create a config for slow parameter changes (50ms interpolation)
		inline InterpolationConfig CreateSlowConfig(f64 sampleRate)
		{
			return CreateDefaultConfig(sampleRate, 0.05); // 50ms
		}
	}

} // namespace OloEngine::Audio::SoundGraph