#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Base.h"
#include <cmath>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// ADEnvelope - Attack-Decay envelope generator
	/// Provides a simple two-phase envelope with trigger capability
	/// Ideal for percussive sounds and basic dynamics control
	class ADEnvelope : public NodeProcessor
	{
	public:
		enum class State
		{
			Idle,
			Attack,
			Decay
		};

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<f32> m_AttackTimeView;
		ValueView<f32> m_DecayTimeView;
		ValueView<f32> m_AttackCurveView;
		ValueView<f32> m_DecayCurveView;
		ValueView<f32> m_PeakView;
		ValueView<f32> m_SustainView;
		ValueView<f32> m_LoopView;
		ValueView<f32> m_TriggerView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values and Envelope State
		//======================================================================
		
		f32 m_CurrentAttackTime = 0.01f;
		f32 m_CurrentDecayTime = 0.3f;
		f32 m_CurrentAttackCurve = 1.0f;
		f32 m_CurrentDecayCurve = 1.0f;
		f32 m_CurrentPeak = 1.0f;
		f32 m_CurrentSustain = 0.0f;
		f32 m_CurrentLoop = 0.0f;
		f32 m_CurrentTrigger = 0.0f;
		
		State m_CurrentState = State::Idle;
		f32 m_CurrentValue = 0.0f;
		f32 m_AttackRate = 0.0f;
		f32 m_DecayRate = 0.0f;
		u32 m_AttackSamples = 0;
		u32 m_DecaySamples = 0;
		u32 m_CurrentSample = 0;
		
		bool m_PrevTriggerState = false;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit ADEnvelope(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_AttackTimeView("AttackTime", 0.01f)
			, m_DecayTimeView("DecayTime", 0.3f)
			, m_AttackCurveView("AttackCurve", 1.0f)
			, m_DecayCurveView("DecayCurve", 1.0f)
			, m_PeakView("Peak", 1.0f)
			, m_SustainView("Sustain", 0.0f)
			, m_LoopView("Loop", 0.0f)
			, m_TriggerView("Trigger", 0.0f)
			, m_OutputView("Output", 0.0f)
		{
			// Create Input/Output events
			RegisterInputEvent<f32>("AttackTime", [this](f32 value) { m_CurrentAttackTime = value; });
			RegisterInputEvent<f32>("DecayTime", [this](f32 value) { m_CurrentDecayTime = value; });
			RegisterInputEvent<f32>("AttackCurve", [this](f32 value) { m_CurrentAttackCurve = value; });
			RegisterInputEvent<f32>("DecayCurve", [this](f32 value) { m_CurrentDecayCurve = value; });
			RegisterInputEvent<f32>("Peak", [this](f32 value) { m_CurrentPeak = value; });
			RegisterInputEvent<f32>("Sustain", [this](f32 value) { m_CurrentSustain = value; });
			RegisterInputEvent<f32>("Loop", [this](f32 value) { m_CurrentLoop = value; });
			RegisterInputEvent<f32>("Trigger", [this](f32 value) { m_CurrentTrigger = value; });
			
			RegisterOutputEvent<f32>("Output");
			RegisterOutputEvent<f32>("Completed");
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_AttackTimeView.Initialize(maxBufferSize);
			m_DecayTimeView.Initialize(maxBufferSize);
			m_AttackCurveView.Initialize(maxBufferSize);
			m_DecayCurveView.Initialize(maxBufferSize);
			m_PeakView.Initialize(maxBufferSize);
			m_SustainView.Initialize(maxBufferSize);
			m_LoopView.Initialize(maxBufferSize);
			m_TriggerView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
			
			// Reset envelope state
			ResetEnvelope();
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_AttackTimeView.UpdateFromConnections(inputs, numSamples);
			m_DecayTimeView.UpdateFromConnections(inputs, numSamples);
			m_AttackCurveView.UpdateFromConnections(inputs, numSamples);
			m_DecayCurveView.UpdateFromConnections(inputs, numSamples);
			m_PeakView.UpdateFromConnections(inputs, numSamples);
			m_SustainView.UpdateFromConnections(inputs, numSamples);
			m_LoopView.UpdateFromConnections(inputs, numSamples);
			m_TriggerView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 attackTime = m_AttackTimeView.GetValue(sample);
				f32 decayTime = m_DecayTimeView.GetValue(sample);
				f32 attackCurve = m_AttackCurveView.GetValue(sample);
				f32 decayCurve = m_DecayCurveView.GetValue(sample);
				f32 peak = m_PeakView.GetValue(sample);
				f32 sustain = m_SustainView.GetValue(sample);
				f32 loop = m_LoopView.GetValue(sample);
				f32 trigger = m_TriggerView.GetValue(sample);
				
				// Update internal state
				m_CurrentAttackTime = attackTime;
				m_CurrentDecayTime = decayTime;
				m_CurrentAttackCurve = attackCurve;
				m_CurrentDecayCurve = decayCurve;
				m_CurrentPeak = peak;
				m_CurrentSustain = sustain;
				m_CurrentLoop = loop;
				m_CurrentTrigger = trigger;
				
				// Check for trigger (positive edge)
				bool triggerState = trigger > 0.5f;
				if (triggerState && !m_PrevTriggerState)
				{
					TriggerEnvelope();
				}
				m_PrevTriggerState = triggerState;
				
				// Update envelope
				UpdateEnvelope();
				
				// Set output value
				m_OutputView.SetValue(sample, m_CurrentValue);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Envelope Specific Methods
		//======================================================================

		void TriggerEnvelope()
		{
			// Recalculate envelope parameters based on current settings
			CalculateEnvelopeParameters();

			// Start envelope from attack phase
			m_CurrentState = State::Attack;
			m_CurrentSample = 0;
			
			// If attack time is zero, jump directly to decay
			if (m_AttackSamples == 0)
			{
				m_CurrentValue = m_CurrentPeak;
				m_CurrentState = State::Decay;
			}
		}

		void ResetEnvelope()
		{
			m_CurrentState = State::Idle;
			m_CurrentValue = m_CurrentSustain;
			m_CurrentSample = 0;
			m_AttackRate = 0.0f;
			m_DecayRate = 0.0f;
			m_AttackSamples = 0;
			m_DecaySamples = 0;
		}

		f32 GetCurrentValue() const { return m_CurrentValue; }
		State GetCurrentState() const { return m_CurrentState; }
		bool IsActive() const { return m_CurrentState != State::Idle; }

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("ADEnvelope");
		}

		const char* GetDisplayName() const override
		{
			return "AD Envelope";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("AttackTime")) m_CurrentAttackTime = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("DecayTime")) m_CurrentDecayTime = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("AttackCurve")) m_CurrentAttackCurve = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("DecayCurve")) m_CurrentDecayCurve = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Peak")) m_CurrentPeak = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Sustain")) m_CurrentSustain = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Loop")) m_CurrentLoop = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Trigger")) m_CurrentTrigger = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("AttackTime")) return static_cast<T>(m_CurrentAttackTime);
			else if (id == OLO_IDENTIFIER("DecayTime")) return static_cast<T>(m_CurrentDecayTime);
			else if (id == OLO_IDENTIFIER("AttackCurve")) return static_cast<T>(m_CurrentAttackCurve);
			else if (id == OLO_IDENTIFIER("DecayCurve")) return static_cast<T>(m_CurrentDecayCurve);
			else if (id == OLO_IDENTIFIER("Peak")) return static_cast<T>(m_CurrentPeak);
			else if (id == OLO_IDENTIFIER("Sustain")) return static_cast<T>(m_CurrentSustain);
			else if (id == OLO_IDENTIFIER("Loop")) return static_cast<T>(m_CurrentLoop);
			else if (id == OLO_IDENTIFIER("Trigger")) return static_cast<T>(m_CurrentTrigger);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<T>(m_CurrentValue);
			else if (id == OLO_IDENTIFIER("Completed")) return static_cast<T>(m_CurrentState == State::Idle ? 1.0f : 0.0f);
			return T{};
		}

	private:
		//======================================================================
		// Internal Envelope Processing
		//======================================================================
		
		void CalculateEnvelopeParameters()
		{
			// Ensure minimum times to avoid division by zero
			f32 attackTime = std::max(m_CurrentAttackTime, 1e-6f);
			f32 decayTime = std::max(m_CurrentDecayTime, 1e-6f);
			
			// Calculate sample counts
			m_AttackSamples = static_cast<u32>(attackTime * m_SampleRate);
			m_DecaySamples = static_cast<u32>(decayTime * m_SampleRate);
			
			// Ensure minimum of 1 sample
			m_AttackSamples = std::max(m_AttackSamples, 1u);
			m_DecaySamples = std::max(m_DecaySamples, 1u);
			
			// Calculate rates for linear interpolation (used as fallback)
			m_AttackRate = (m_CurrentPeak - m_CurrentSustain) / m_AttackSamples;
			m_DecayRate = (m_CurrentSustain - m_CurrentPeak) / m_DecaySamples;
		}

		void UpdateEnvelope()
		{
			switch (m_CurrentState)
			{
				case State::Idle:
					// Envelope is idle, output sustain level
					m_CurrentValue = m_CurrentSustain;
					break;

				case State::Attack:
				{
					// Calculate normalized progress (0.0 to 1.0)
					f32 progress = static_cast<f32>(m_CurrentSample) / m_AttackSamples;
					progress = std::clamp(progress, 0.0f, 1.0f);
					
					// Apply curve transformation
					f32 curvedProgress = ApplyCurve(progress, m_CurrentAttackCurve);
					
					// Calculate envelope value
					m_CurrentValue = m_CurrentSustain + curvedProgress * (m_CurrentPeak - m_CurrentSustain);
					
					// Advance to next sample
					m_CurrentSample++;
					
					// Check if attack phase is complete
					if (m_CurrentSample >= m_AttackSamples)
					{
						m_CurrentValue = m_CurrentPeak;
						m_CurrentState = State::Decay;
						m_CurrentSample = 0;
					}
					break;
				}

				case State::Decay:
				{
					// Calculate normalized progress (0.0 to 1.0)
					f32 progress = static_cast<f32>(m_CurrentSample) / m_DecaySamples;
					progress = std::clamp(progress, 0.0f, 1.0f);
					
					// Apply curve transformation
					f32 curvedProgress = ApplyCurve(progress, m_CurrentDecayCurve);
					
					// Calculate envelope value
					m_CurrentValue = m_CurrentPeak + curvedProgress * (m_CurrentSustain - m_CurrentPeak);
					
					// Advance to next sample
					m_CurrentSample++;
					
					// Check if decay phase is complete
					if (m_CurrentSample >= m_DecaySamples)
					{
						m_CurrentValue = m_CurrentSustain;
						
						// Check for looping
						if (m_CurrentLoop > 0.5f)
						{
							// Restart envelope
							m_CurrentState = State::Attack;
							m_CurrentSample = 0;
							
							// If attack time is zero, jump directly to decay
							if (m_AttackSamples == 0)
							{
								m_CurrentValue = m_CurrentPeak;
								m_CurrentState = State::Decay;
							}
						}
						else
						{
							// Envelope completed
							m_CurrentState = State::Idle;
							
							// Trigger completion event
							TriggerOutputEvent<f32>("Completed", 1.0f);
						}
					}
					break;
				}
			}
		}

		f32 ApplyCurve(f32 progress, f32 curve) const
		{
			// Ensure curve is positive and non-zero
			curve = std::max(curve, 0.001f);
			
			// Apply power curve transformation
			return std::pow(progress, 1.0f / curve);
		}
	};

} // namespace OloEngine::Audio::SoundGraph