#pragma once

#include "NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/FastRandom.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Example Random node demonstrating the enhanced event system
	/// This node generates random values when triggered and has reset functionality
	template<typename T>
	class RandomNode : public NodeProcessor
	{
	public:
		RandomNode() : NodeProcessor()
		{
			SetupEndpoints();
		}

		virtual ~RandomNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Process events first
			ProcessTriggerEvents();

			// If we have output and it's been triggered, output the current value
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = static_cast<f32>(m_CurrentValue);
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			SeedRandomGenerator();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("RandomNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Random (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Random (i32)";
			else
				return "Random";
		}

	private:
		void SetupEndpoints()
		{
			// Input parameters
			AddParameter<T>(OLO_IDENTIFIER("MinValue"), "MinValue", T{0});
			AddParameter<T>(OLO_IDENTIFIER("MaxValue"), "MaxValue", T{1});
			AddParameter<i32>(OLO_IDENTIFIER("Seed"), "Seed", i32{-1}); // -1 means use time-based seed

			// Input events with flags
			m_NextEvent = AddInputEvent(OLO_IDENTIFIER("Next"), "Next", 
				[this](f32) { m_NextFlag.SetDirty(); });
			m_ResetEvent = AddInputEvent(OLO_IDENTIFIER("Reset"), "Reset", 
				[this](f32) { m_ResetFlag.SetDirty(); });

			// Output events
			m_OnNextEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnNext"), "OnNext");
			m_OnResetEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnReset"), "OnReset");

			// Output parameters
			AddParameter<T>(OLO_IDENTIFIER("Value"), "Value", T{0});
		}

		void ProcessTriggerEvents()
		{
			// Process reset event
			if (m_ResetFlag.CheckAndResetIfDirty())
			{
				SeedRandomGenerator();
				if (m_OnResetEvent)
					(*m_OnResetEvent)(1.0f);
			}

			// Process next event
			if (m_NextFlag.CheckAndResetIfDirty())
			{
				GenerateNextValue();
				if (m_OnNextEvent)
					(*m_OnNextEvent)(1.0f);
			}
		}

		void SeedRandomGenerator()
		{
			const i32 seedValue = m_Parameters.GetParameter<i32>(OLO_IDENTIFIER("Seed"));
			if (seedValue == -1)
			{
				// Use time-based seed
				m_Random.SetSeed(OloEngine::RandomUtils::GetTimeBasedSeed());
			}
			else
			{
				m_Random.SetSeed(seedValue);
			}
		}

		void GenerateNextValue()
		{
			const T minVal = m_Parameters.GetParameter<T>(OLO_IDENTIFIER("MinValue"));
			const T maxVal = m_Parameters.GetParameter<T>(OLO_IDENTIFIER("MaxValue"));

			if constexpr (std::is_same_v<T, f32>)
			{
				m_CurrentValue = m_Random.GetFloat32InRange(minVal, maxVal);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				m_CurrentValue = m_Random.GetInt32InRange(minVal, maxVal);
			}
			else
			{
				// Fallback for other types
				m_CurrentValue = static_cast<T>(m_Random.GetFloat32InRange(
					static_cast<f32>(minVal), static_cast<f32>(maxVal)));
			}

			// Update output parameter
			m_Parameters.SetParameter(OLO_IDENTIFIER("Value"), m_CurrentValue);
		}

	private:
		/// Event flags
		Flag m_NextFlag;
		Flag m_ResetFlag;

		/// Event endpoints (stored as members)
		std::shared_ptr<InputEvent> m_NextEvent;
		std::shared_ptr<InputEvent> m_ResetEvent;
		std::shared_ptr<OutputEvent> m_OnNextEvent;
		std::shared_ptr<OutputEvent> m_OnResetEvent;

		/// Random generator
		OloEngine::FastRandom m_Random;

		/// Current generated value
		T m_CurrentValue = T{0};
	};

	// Type aliases for common usage
	using RandomNodeF32 = RandomNode<f32>;
	using RandomNodeI32 = RandomNode<i32>;

	//==============================================================================
	/// Example TriggerCounter node showing complex event routing
	class TriggerCounter : public NodeProcessor
	{
	public:
		TriggerCounter() : NodeProcessor()
		{
			SetupEndpoints();
		}

		virtual ~TriggerCounter() = default;

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			ProcessEvents();

			// Output current count value
			if (outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = static_cast<f32>(m_Count);
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			m_Count = m_Parameters.GetParameter<i32>(OLO_IDENTIFIER("StartValue"));
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("TriggerCounter");
		}

		const char* GetDisplayName() const override
		{
			return "Trigger Counter";
		}

	private:
		void SetupEndpoints()
		{
			// Input parameters
			AddParameter<i32>(OLO_IDENTIFIER("StartValue"), "StartValue", 0);
			AddParameter<i32>(OLO_IDENTIFIER("StepSize"), "StepSize", 1);
			AddParameter<i32>(OLO_IDENTIFIER("ResetCount"), "ResetCount", 0);

			// Input events
			m_TriggerEvent = AddInputEvent(OLO_IDENTIFIER("Trigger"), "Trigger", 
				[this](f32) { m_TriggerFlag.SetDirty(); });
			m_ResetEvent = AddInputEvent(OLO_IDENTIFIER("Reset"), "Reset", 
				[this](f32) { m_ResetFlag.SetDirty(); });

			// Output events
			m_OnTriggerEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnTrigger"), "OnTrigger");
			m_OnResetEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnReset"), "OnReset");

			// Output parameters
			AddParameter<i32>(OLO_IDENTIFIER("Count"), "Count", 0);
			AddParameter<i32>(OLO_IDENTIFIER("Value"), "Value", 0);
		}

		void ProcessEvents()
		{
			// Process reset
			if (m_ResetFlag.CheckAndResetIfDirty())
			{
				m_Count = m_Parameters.GetParameter<i32>(OLO_IDENTIFIER("ResetCount"));
				m_Parameters.SetParameter(OLO_IDENTIFIER("Count"), m_Count);
				m_Parameters.SetParameter(OLO_IDENTIFIER("Value"), m_Count);

				if (m_OnResetEvent)
					(*m_OnResetEvent)(static_cast<f32>(m_Count));
			}

			// Process trigger
			if (m_TriggerFlag.CheckAndResetIfDirty())
			{
				const i32 stepSize = m_Parameters.GetParameter<i32>(OLO_IDENTIFIER("StepSize"));
				m_Count += stepSize;
				
				m_Parameters.SetParameter(OLO_IDENTIFIER("Count"), m_Count);
				m_Parameters.SetParameter(OLO_IDENTIFIER("Value"), m_Count);

				if (m_OnTriggerEvent)
					(*m_OnTriggerEvent)(static_cast<f32>(m_Count));
			}
		}

	private:
		Flag m_TriggerFlag;
		Flag m_ResetFlag;
		i32 m_Count = 0;

		/// Event endpoints (stored as members)
		std::shared_ptr<InputEvent> m_TriggerEvent;
		std::shared_ptr<InputEvent> m_ResetEvent;
		std::shared_ptr<OutputEvent> m_OnTriggerEvent;
		std::shared_ptr<OutputEvent> m_OnResetEvent;
	};

} // namespace OloEngine::Audio::SoundGraph