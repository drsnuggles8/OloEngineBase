#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Value.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"
#include "OloEngine/Audio/SoundGraph/Events.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// ADSREnvelope - Classic Attack-Decay-Sustain-Release envelope generator using new Hazel-style foundation
	/// Provides full ADSR envelope with note-on/note-off control
	/// Essential for musical instruments and expressive sound design
	class ADSREnvelope : public NodeProcessor
	{
	public:
		enum class State
		{
			Idle,
			Attack,
			Decay,
			Sustain,
			Release
		};

	private:
		// Input streams for connected values
		ValueView<f32> m_AttackTimeStream;
		ValueView<f32> m_DecayTimeStream;
		ValueView<f32> m_SustainLevelStream;
		ValueView<f32> m_ReleaseTimeStream;
		ValueView<f32> m_AttackCurveStream;
		ValueView<f32> m_DecayCurveStream;
		ValueView<f32> m_ReleaseCurveStream;
		ValueView<f32> m_PeakStream;
		ValueView<f32> m_VelocityStream;
		
		// Output stream for envelope values
		ValueView<f32> m_OutputStream;
		ValueView<i32> m_StateOutputStream;

		// Current parameter values (for single-value processing)
		f32 m_CurrentAttackTime = 0.1f;     // seconds
		f32 m_CurrentDecayTime = 0.2f;      // seconds
		f32 m_CurrentSustainLevel = 0.5f;   // 0.0 to 1.0
		f32 m_CurrentReleaseTime = 0.3f;    // seconds
		f32 m_CurrentAttackCurve = 1.0f;    // curve factor
		f32 m_CurrentDecayCurve = 1.0f;     // curve factor
		f32 m_CurrentReleaseCurve = 1.0f;   // curve factor
		f32 m_CurrentPeak = 1.0f;           // peak level
		f32 m_CurrentVelocity = 1.0f;       // note velocity
		f32 m_CurrentOutput = 0.0f;
		i32 m_CurrentStateOutput = 0;       // state as integer

		// Envelope state
		State m_CurrentState = State::Idle;
		f32 m_CurrentValue = 0.0f;
		f32 m_TargetValue = 0.0f;
		u32 m_AttackSamples = 0;
		u32 m_DecaySamples = 0;
		u32 m_ReleaseSamples = 0;
		u32 m_CurrentSample = 0;
		f32 m_StartValue = 0.0f;
		f64 m_SampleRate = 44100.0;

		// Events and flags
		Flag m_NoteOnFlag;
		Flag m_NoteOffFlag;

		// Parameter limits
		static constexpr f32 MIN_TIME_SECONDS = 0.001f;  // 1ms minimum
		static constexpr f32 MAX_TIME_SECONDS = 10.0f;   // 10s maximum
		static constexpr f32 MIN_LEVEL = 0.0f;
		static constexpr f32 MAX_LEVEL = 1.0f;
		static constexpr f32 MIN_CURVE = 0.1f;
		static constexpr f32 MAX_CURVE = 10.0f;

	public:
		ADSREnvelope()
		{
			// Create input events for receiving parameter values
			auto attackTimeEvent = std::make_shared<InputEvent>("AttackTime", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentAttackTime = glm::clamp(value.Get<f32>(), MIN_TIME_SECONDS, MAX_TIME_SECONDS);
				}
			});

			auto decayTimeEvent = std::make_shared<InputEvent>("DecayTime", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentDecayTime = glm::clamp(value.Get<f32>(), MIN_TIME_SECONDS, MAX_TIME_SECONDS);
				}
			});

			auto sustainLevelEvent = std::make_shared<InputEvent>("SustainLevel", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentSustainLevel = glm::clamp(value.Get<f32>(), MIN_LEVEL, MAX_LEVEL);
				}
			});

			auto releaseTimeEvent = std::make_shared<InputEvent>("ReleaseTime", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentReleaseTime = glm::clamp(value.Get<f32>(), MIN_TIME_SECONDS, MAX_TIME_SECONDS);
				}
			});

			auto attackCurveEvent = std::make_shared<InputEvent>("AttackCurve", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentAttackCurve = glm::clamp(value.Get<f32>(), MIN_CURVE, MAX_CURVE);
				}
			});

			auto decayCurveEvent = std::make_shared<InputEvent>("DecayCurve", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentDecayCurve = glm::clamp(value.Get<f32>(), MIN_CURVE, MAX_CURVE);
				}
			});

			auto releaseCurveEvent = std::make_shared<InputEvent>("ReleaseCurve", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentReleaseCurve = glm::clamp(value.Get<f32>(), MIN_CURVE, MAX_CURVE);
				}
			});

			auto peakEvent = std::make_shared<InputEvent>("Peak", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentPeak = glm::clamp(value.Get<f32>(), MIN_LEVEL, MAX_LEVEL);
				}
			});

			auto velocityEvent = std::make_shared<InputEvent>("Velocity", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentVelocity = glm::clamp(value.Get<f32>(), MIN_LEVEL, MAX_LEVEL);
				}
			});

			auto noteOnEvent = std::make_shared<InputEvent>("NoteOn", [this](const Value& value) {
				if (value.GetType() == ValueType::Bool && value.Get<bool>())
				{
					TriggerNoteOn();
				}
			});

			auto noteOffEvent = std::make_shared<InputEvent>("NoteOff", [this](const Value& value) {
				if (value.GetType() == ValueType::Bool && value.Get<bool>())
				{
					TriggerNoteOff();
				}
			});

			// Register all input events
			AddInputEvent(attackTimeEvent);
			AddInputEvent(decayTimeEvent);
			AddInputEvent(sustainLevelEvent);
			AddInputEvent(releaseTimeEvent);
			AddInputEvent(attackCurveEvent);
			AddInputEvent(decayCurveEvent);
			AddInputEvent(releaseCurveEvent);
			AddInputEvent(peakEvent);
			AddInputEvent(velocityEvent);
			AddInputEvent(noteOnEvent);
			AddInputEvent(noteOffEvent);

			// Create output events for sending envelope values
			auto outputEvent = std::make_shared<OutputEvent>("Output");
			auto stateOutputEvent = std::make_shared<OutputEvent>("StateOutput");
			AddOutputEvent(outputEvent);
			AddOutputEvent(stateOutputEvent);
		}

		virtual ~ADSREnvelope() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize ValueView streams with proper capacity
			m_AttackTimeStream = ValueView<f32>(maxBufferSize);
			m_DecayTimeStream = ValueView<f32>(maxBufferSize);
			m_SustainLevelStream = ValueView<f32>(maxBufferSize);
			m_ReleaseTimeStream = ValueView<f32>(maxBufferSize);
			m_AttackCurveStream = ValueView<f32>(maxBufferSize);
			m_DecayCurveStream = ValueView<f32>(maxBufferSize);
			m_ReleaseCurveStream = ValueView<f32>(maxBufferSize);
			m_PeakStream = ValueView<f32>(maxBufferSize);
			m_VelocityStream = ValueView<f32>(maxBufferSize);
			m_OutputStream = ValueView<f32>(maxBufferSize);
			m_StateOutputStream = ValueView<i32>(maxBufferSize);

			// Reset envelope state
			m_CurrentState = State::Idle;
			m_CurrentValue = 0.0f;
			m_TargetValue = 0.0f;
			m_CurrentSample = 0;
			m_StartValue = 0.0f;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Handle stream processing mode (real-time audio)
			if (outputs && outputs[0])
			{
				// Process envelope for each sample
				for (u32 sample = 0; sample < numSamples; ++sample)
				{
					// Check for note events (flags set from input events)
					if (m_NoteOnFlag.CheckAndResetIfDirty())
					{
						TriggerNoteOn();
					}
					
					if (m_NoteOffFlag.CheckAndResetIfDirty())
					{
						TriggerNoteOff();
					}

					// Update envelope based on current state
					UpdateEnvelope();

					// Fill output stream and audio buffer
					m_OutputStream[sample] = m_CurrentValue;
					m_StateOutputStream[sample] = static_cast<i32>(m_CurrentState);
					outputs[0][sample] = m_CurrentValue;
				}

				// Update current output values with last sample
				m_CurrentOutput = m_OutputStream[numSamples - 1];
				m_CurrentStateOutput = m_StateOutputStream[numSamples - 1];

				// Send outputs via event system
				auto outputEvent = GetOutputEvent("Output");
				auto stateOutputEvent = GetOutputEvent("StateOutput");
				if (outputEvent)
				{
					outputEvent->SendValue(Value(m_CurrentOutput));
				}
				if (stateOutputEvent)
				{
					stateOutputEvent->SendValue(Value(m_CurrentStateOutput));
				}
			}
			else
			{
				// Handle single-value processing mode (control parameters)
				// Check for note events
				if (m_NoteOnFlag.CheckAndResetIfDirty())
				{
					TriggerNoteOn();
				}
				
				if (m_NoteOffFlag.CheckAndResetIfDirty())
				{
					TriggerNoteOff();
				}

				// Update envelope for single sample
				UpdateEnvelope();

				m_CurrentOutput = m_CurrentValue;
				m_CurrentStateOutput = static_cast<i32>(m_CurrentState);

				// Send outputs via event system
				auto outputEvent = GetOutputEvent("Output");
				auto stateOutputEvent = GetOutputEvent("StateOutput");
				if (outputEvent)
				{
					outputEvent->SendValue(Value(m_CurrentOutput));
				}
				if (stateOutputEvent)
				{
					stateOutputEvent->SendValue(Value(m_CurrentStateOutput));
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("ADSREnvelope");
		}

		const char* GetDisplayName() const override
		{
			return "ADSR Envelope";
		}

		//======================================================================
		// Envelope Specific Methods
		//======================================================================

		void TriggerNoteOn()
		{
			// Calculate envelope parameters using current values
			f32 attackTime = glm::clamp(m_CurrentAttackTime, MIN_TIME_SECONDS, MAX_TIME_SECONDS);
			f32 decayTime = glm::clamp(m_CurrentDecayTime, MIN_TIME_SECONDS, MAX_TIME_SECONDS);

			m_AttackSamples = static_cast<u32>(attackTime * m_SampleRate);
			m_DecaySamples = static_cast<u32>(decayTime * m_SampleRate);

			// Start attack phase
			m_CurrentState = State::Attack;
			m_CurrentSample = 0;
			m_StartValue = m_CurrentValue; // Start from current value for smooth transitions
		}

		void TriggerNoteOff()
		{
			if (m_CurrentState != State::Idle)
			{
				f32 releaseTime = glm::clamp(m_CurrentReleaseTime, MIN_TIME_SECONDS, MAX_TIME_SECONDS);
				m_ReleaseSamples = static_cast<u32>(releaseTime * m_SampleRate);

				m_CurrentState = State::Release;
				m_CurrentSample = 0;
				m_StartValue = m_CurrentValue; // Start release from current value
			}
		}

		void UpdateEnvelope()
		{
			const f32 peak = m_CurrentPeak * m_CurrentVelocity;
			const f32 sustainLevel = m_CurrentSustainLevel * peak;
			const f32 attackCurve = glm::clamp(m_CurrentAttackCurve, MIN_CURVE, MAX_CURVE);
			const f32 decayCurve = glm::clamp(m_CurrentDecayCurve, MIN_CURVE, MAX_CURVE);
			const f32 releaseCurve = glm::clamp(m_CurrentReleaseCurve, MIN_CURVE, MAX_CURVE);

			switch (m_CurrentState)
			{
			case State::Attack:
				if (m_CurrentSample < m_AttackSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_AttackSamples;
					f32 curvedProgress = glm::pow(progress, 1.0f / attackCurve);
					m_CurrentValue = m_StartValue + (peak - m_StartValue) * curvedProgress;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = peak;
					m_CurrentState = State::Decay;
					m_CurrentSample = 0;
					m_StartValue = peak;
				}
				break;

			case State::Decay:
				if (m_CurrentSample < m_DecaySamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_DecaySamples;
					f32 curvedProgress = glm::pow(progress, decayCurve);
					m_CurrentValue = peak + (sustainLevel - peak) * curvedProgress;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = sustainLevel;
					m_CurrentState = State::Sustain;
				}
				break;

			case State::Sustain:
				m_CurrentValue = sustainLevel;
				break;

			case State::Release:
				if (m_CurrentSample < m_ReleaseSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_ReleaseSamples;
					f32 curvedProgress = glm::pow(progress, releaseCurve);
					m_CurrentValue = m_StartValue * (1.0f - curvedProgress);
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = 0.0f;
					m_CurrentState = State::Idle;
				}
				break;

			case State::Idle:
				m_CurrentValue = 0.0f;
				break;
			}
		}

		void ResetEnvelope()
		{
			m_CurrentState = State::Idle;
			m_CurrentValue = 0.0f;
			m_CurrentSample = 0;
			m_StartValue = 0.0f;
		}

		// Legacy compatibility methods for direct access
		State GetCurrentState() const { return m_CurrentState; }
		f32 GetCurrentValue() const { return m_CurrentValue; }
		f32 GetOutput() const { return m_CurrentOutput; }
		i32 GetStateOutput() const { return m_CurrentStateOutput; }
		bool IsActive() const { return m_CurrentState != State::Idle; }
		bool IsInSustain() const { return m_CurrentState == State::Sustain; }

		// Direct parameter setters
		void SetAttackTime(f32 time) { m_CurrentAttackTime = glm::clamp(time, MIN_TIME_SECONDS, MAX_TIME_SECONDS); }
		void SetDecayTime(f32 time) { m_CurrentDecayTime = glm::clamp(time, MIN_TIME_SECONDS, MAX_TIME_SECONDS); }
		void SetSustainLevel(f32 level) { m_CurrentSustainLevel = glm::clamp(level, MIN_LEVEL, MAX_LEVEL); }
		void SetReleaseTime(f32 time) { m_CurrentReleaseTime = glm::clamp(time, MIN_TIME_SECONDS, MAX_TIME_SECONDS); }
		void SetPeak(f32 peak) { m_CurrentPeak = glm::clamp(peak, MIN_LEVEL, MAX_LEVEL); }
		void SetVelocity(f32 velocity) { m_CurrentVelocity = glm::clamp(velocity, MIN_LEVEL, MAX_LEVEL); }

		// Note control methods
		void NoteOn(f32 velocity = 1.0f) 
		{ 
			SetVelocity(velocity);
			m_NoteOnFlag.SetDirty();
		}
		void NoteOff() 
		{ 
			m_NoteOffFlag.SetDirty();
		}

		// ValueView accessors for advanced stream processing
		const ValueView<f32>& GetAttackTimeStream() const { return m_AttackTimeStream; }
		const ValueView<f32>& GetDecayTimeStream() const { return m_DecayTimeStream; }
		const ValueView<f32>& GetSustainLevelStream() const { return m_SustainLevelStream; }
		const ValueView<f32>& GetReleaseTimeStream() const { return m_ReleaseTimeStream; }
		const ValueView<f32>& GetOutputStream() const { return m_OutputStream; }
		const ValueView<i32>& GetStateOutputStream() const { return m_StateOutputStream; }
	};

} // namespace OloEngine::Audio::SoundGraph